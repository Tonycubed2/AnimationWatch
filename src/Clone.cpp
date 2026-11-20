// The preview clone - AnimationWatch 1.7.0
//
// Private viewing room: the clone does not stand in the game world. "Play" first takes the player to the viewing room
// in AnimationWatch.esp (an ESL-flagged plugin with one interior cell, AnimationWatchViewingRoom, object 800: a
// 1280 x 1280 room of the game's GenKit dungeon pieces, lit by two white lights and the cell's own light). Without
// that plugin enabled it falls back to a small closed test room in Skyrim.esm that no quest uses (cell CTest,
// 000B1783). It leaves an invisible marker (XMarker 0000003B, persistent) where the player
// stood. The clone is made there, in front of the player. "Remove clone" deletes it and takes the player back to the
// marker, then deletes the marker. The marker's ID is kept in the SKSE co-save, so a save made in the room still
// returns the player to where they were when it is loaded.
// In the room the player is hidden (third person, 3D culled) so only the clone is seen, and anyone who came along
// (followers, pets, summons) is sent back to the marker; they rejoin when the player comes back.
// Leaving: the "Leave the viewing room" button, or Esc while in the room (the Journal menu that Esc opens is closed
// again). Both wait until the F1 menu is closed, go back, and then open F1 again.
// A new reference of the player's own base form (PlaceObjectAtMe), so race, sex, face / head parts, hair, weight,
// height and skin colour come from the same record. What the player wears (armor, clothing, jewelry, both hands,
// ammo) is then added to it and equipped. Its AI is off, so it stands still and its own behavior graph does not move
// its bones: the animation player (Player.cpp) poses it every frame.
// It stands 150 units in front of the player, facing the player. It is temporary: "Remove clone" deletes it, and a
// clone found after loading a save (saved while it existed) is removed automatically.
// Known limits: gear is copied without enchantments or tempering, spells are not copied, RaceMenu body morphs and
// overlays are not copied; a copy of the player's base form can confuse a few quests while it exists.
// Based on the CloneManager in SkyAnimViewer's source (the same author's project).

#include "Clone.h"
#include "FreeCam.h"

#include "SKSEMenuFramework.h"

#ifdef GetObject
#	undef GetObject  // windows.h macro clashes with BGSDefaultObjectManager::GetObject
#endif

namespace Clone
{
	namespace
	{
		constexpr float              kDistance = 250.0f;  // far enough that the whole clone fits the first-person view
		constexpr RE::FormID         kTestCell = 0x000B1783;  // CTest in Skyrim.esm, the fallback
		constexpr RE::FormID         kXMarker = 0x0000003B;
		const RE::NiPoint3           kTestSpot{ 1984.0f, 1984.0f, 6784.0f };  // its COC marker (COCMarkerHeading 000790BA)
		const RE::NiPoint3           kRoomSpot{ 0.0f, -100.0f, 0.0f };        // AnimationWatch.esp room: near the middle, facing +y
		RE::ObjectRefHandle          g_returnMarker;
		RE::FormID                   g_returnMarkerID = 0;  // for the co-save
		bool                         g_travelling = false;
		bool                         g_playerHidden = false;  // the view is set up (first person, facing the clone)
		bool                         g_wasThirdPerson = false;
		std::atomic<bool>            g_leaveRequested{ false };
		bool                         g_reopenF1 = false;
		int                          g_evictTimer = 0;
		bool                         g_playerCulled = false;  // the player's third-person body is culled in the room (1.8.7)
		std::int64_t                 g_cameraTryAt = 0;      // last ForceFirstPerson (ms), 0 = not tried yet
		bool                         g_viewReady = false;    // latched: every gate passed (or the timeout), the world may stand still
		std::int64_t                 g_viewStart = 0;        // when the clone was made (ms), for the timeout
		std::int64_t                 g_gateLogAt = 0;
		int                          g_gearCheck = -1;       // frames until the gear check after CopyGear
		std::size_t                  g_nodesBeforeGear = 0;

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// the clone's fade (0 = invisible, 1 = fully shown); -1 when its 3D is not loaded, 1 when it has no fade node
		float FadeOf(RE::Actor* a_actor)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return -1.0f;
			}
			auto* fade = root->AsFadeNode();
			return fade ? fade->GetRuntimeData().currentFade : 1.0f;
		}

		// a newly placed actor fades in over game time; with the world stood still it stayed part-faded (0.91) and was
		// not drawn. Show it fully at once.
		void ForceShown(RE::Actor* a_actor)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return;
			}
			if (root->GetAppCulled()) {
				root->SetAppCulled(false);
			}
			if (auto* fade = root->AsFadeNode()) {
				fade->GetRuntimeData().currentFade = 1.0f;
			}
			if (a_actor->GetAlpha() < 1.0f) {
				a_actor->SetAlpha(1.0f);
			}
		}

		std::size_t ChildCount(RE::Actor* a_actor)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			auto* node = root ? root->AsNode() : nullptr;
			std::size_t n = 0;
			if (node) {
				for (auto& child : node->GetChildren()) {
					if (child) {
						++n;
					}
				}
			}
			return n;
		}

		int WornCount(RE::Actor* a_actor)
		{
			int worn = 0;
			for (auto& [obj, data] : a_actor->GetInventory()) {
				if (obj && data.second && data.second->IsWorn()) {
					++worn;
				}
			}
			return worn;
		}

		// the player's own third-person body: hidden in the room so it never blocks the view, even in third person
		bool g_playerWasCulled = false;  // its state before we hid it, put back on leaving

		void CullPlayer(bool a_cull)
		{
			if (!a_cull && !g_playerCulled) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* root = player ? player->Get3D(false) : nullptr;
			if (root) {
				if (a_cull) {
					g_playerWasCulled = root->GetAppCulled();
					root->SetAppCulled(true);
				} else {
					root->SetAppCulled(g_playerWasCulled);
				}
			}
			if (g_playerCulled != a_cull) {
				logger::info("Clone: player's third-person body {}", a_cull ? "hidden" : "shown again");
			}
			g_playerCulled = a_cull;
		}

		RE::TESObjectCELL* OwnRoom()
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			return handler ? handler->LookupForm<RE::TESObjectCELL>(0x800, "AnimationWatch.esp") : nullptr;
		}

		RE::TESObjectCELL* Room()
		{
			if (auto* own = OwnRoom()) {
				return own;
			}
			return RE::TESForm::LookupByID<RE::TESObjectCELL>(kTestCell);
		}

		RE::NiPoint3 RoomSpot() { return OwnRoom() ? kRoomSpot : kTestSpot; }

		bool IsRoom(const RE::TESObjectCELL* a_cell) { return a_cell && (a_cell == OwnRoom() || a_cell->GetFormID() == kTestCell); }

		bool InRoom(RE::PlayerCharacter* a_player) { return a_player && IsRoom(a_player->GetParentCell()); }

		using MoveToImpl_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);

		void MoveToCell(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_pos, const RE::NiPoint3& a_rot)
		{
			static const auto fn = reinterpret_cast<MoveToImpl_t>(REL::ID(56626).address());  // TESObjectREFR::MoveTo_Impl (AE)
			fn(a_ref, RE::ObjectRefHandle{}, a_cell, nullptr, a_pos, a_rot);
		}

		std::mutex          g_lock;
		RE::ObjectRefHandle g_handle;
		std::string         g_status = "no clone yet: it is made the first time you play an animation";
		int                 g_gearCountdown = -1;  // frames until the gear copy (needs its 3D)
		std::atomic<bool>   g_ready{ false };

		void SetStatus(std::string a_text)
		{
			logger::info("Clone: {}", a_text);
			std::scoped_lock l{ g_lock };
			g_status = std::move(a_text);
		}

		void PlaceInFront(RE::Actor* a_clone, RE::PlayerCharacter* a_player)
		{
			const float yaw = a_player->GetAngleZ();
			const auto  pos = a_player->GetPosition();
			a_clone->SetPosition(RE::NiPoint3{ pos.x + kDistance * std::sin(yaw), pos.y + kDistance * std::cos(yaw), pos.z }, true);
			a_clone->data.angle.z = yaw + RE::NI_PI;  // facing the player
			a_clone->Update3DPosition(true);
		}

		RE::BGSEquipSlot* LeftHandSlot()
		{
			auto* mgr = RE::BGSDefaultObjectManager::GetSingleton();
			if (!mgr) {
				return nullptr;
			}
			auto** slot = mgr->GetObject<RE::BGSEquipSlot>(RE::DefaultObjectID::kLeftHandEquip);
			return slot ? *slot : nullptr;
		}

		void CopyGear(RE::Actor* a_clone, RE::PlayerCharacter* a_player)
		{
			auto* equip = RE::ActorEquipManager::GetSingleton();
			if (!equip) {
				return;
			}
			int added = 0;
			for (auto& [obj, data] : a_clone->GetInventory()) {
				if (obj && data.second && data.second->IsWorn()) {
					equip->UnequipObject(a_clone, obj, nullptr, 1, nullptr, false, true, false, true);
				}
			}
			for (auto& [obj, data] : a_player->GetInventory()) {
				if (obj && data.second && data.second->IsWorn() && (obj->IsArmor() || obj->Is(RE::FormType::Ammo))) {
					a_clone->AddObjectToContainer(obj, nullptr, 1, nullptr);
					equip->EquipObject(a_clone, obj, nullptr, 1, nullptr, false, true, false, true);
					++added;
				}
			}
			auto equipHand = [&](RE::TESForm* a_form, bool a_left) {
				if (!a_form || a_form->Is(RE::FormType::Spell)) {
					return;
				}
				if (auto* bound = a_form->As<RE::TESBoundObject>()) {
					a_clone->AddObjectToContainer(bound, nullptr, 1, nullptr);
					equip->EquipObject(a_clone, bound, nullptr, 1, a_left ? LeftHandSlot() : nullptr, false, true, false, true);
					++added;
				}
			};
			auto* right = a_player->GetEquippedObject(false);
			auto* left = a_player->GetEquippedObject(true);
			equipHand(right, false);
			if (left && left != right) {
				equipHand(left, true);
			}
			SetStatus(std::format("clone ready ({} items of your gear on it)", added));
		}

		void DoRemove()
		{
			FreeCam::Off("clone removed");
			RE::ObjectRefHandle handle;
			{
				std::scoped_lock l{ g_lock };
				handle = g_handle;
				g_handle.reset();
			}
			g_ready = false;
			g_gearCountdown = -1;
			g_gearCheck = -1;
			g_viewReady = false;
			g_viewStart = 0;
			auto ref = handle.get();
			if (!ref) {
				return;
			}
			if (auto* actor = ref->As<RE::Actor>()) {
				actor->EnableAI(false);
			}
			ref->Disable();
			ref->SetDelete(true);
			SetStatus("clone removed");
		}

		void DoReturn();

		void DoRemoveAndReturn()
		{
			DoRemove();
			DoReturn();
		}

		void DoCreate();

		// take the player to the viewing room, leaving a marker where they stood
		void DoTravel()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* room = Room();
			if (!player || !room) {
				SetStatus("cannot open the viewing room (AnimationWatch.esp not enabled, and cell CTest not found)");
				return;
			}
			if (InRoom(player)) {
				DoCreate();
				return;
			}
			auto* markerBase = RE::TESForm::LookupByID<RE::TESBoundObject>(kXMarker);
			auto  marker = markerBase ? player->PlaceObjectAtMe(markerBase, true) : nullptr;
			if (!marker) {
				SetStatus("cannot leave a return marker; not going to the viewing room");
				return;
			}
			marker->data.angle = player->data.angle;
			{
				std::scoped_lock l{ g_lock };
				g_returnMarker = marker->CreateRefHandle();
				g_returnMarkerID = marker->GetFormID();
			}
			g_travelling = true;
			MoveToCell(player, room, RoomSpot(), RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
			if (!OwnRoom()) {
				logger::warn("Clone: AnimationWatch.esp is not enabled; using the small test room CTest");
			}
			SetStatus("going to the viewing room");
		}

		// take the player back to the marker and delete it
		void DoReturn()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			RE::ObjectRefHandle handle;
			{
				std::scoped_lock l{ g_lock };
				handle = g_returnMarker;
				g_returnMarker.reset();
				g_returnMarkerID = 0;
			}
			g_travelling = false;
			FreeCam::Off("leaving the viewing room");
			CullPlayer(false);
			if ((g_playerHidden || g_cameraTryAt) && player) {
				if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && g_wasThirdPerson) {
					camera->ForceThirdPerson();
				}
			}
			g_playerHidden = false;
			g_cameraTryAt = 0;
			auto marker = handle.get();
			if (!player) {
				return;
			}
			g_reopenF1 = true;
			if (!marker) {
				// the way back was lost (a save made in the room before 1.8.1): a safe place instead
				const bool ok = player->CenterOnCell("RiverwoodSleepingGiantInn");
				SetStatus(ok ? "the way back was lost: you are at the Sleeping Giant Inn in Riverwood" : "the way back was lost; use a door or fast travel");
				return;
			}
			player->MoveTo(marker.get());
			marker->Disable();
			marker->SetDelete(true);
			SetStatus("back from the viewing room");
		}

		void DoCreate()
		{
			DoRemove();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* base = player ? player->GetActorBase() : nullptr;
			if (!player || !base || !player->Get3D(false) || !player->GetParentCell()) {
				SetStatus("cannot make the clone: the player is not loaded");
				return;
			}
			auto  ref = player->PlaceObjectAtMe(base, false);
			auto* clone = ref ? ref->As<RE::Actor>() : nullptr;
			if (!clone) {
				SetStatus("cannot make the clone: PlaceObjectAtMe failed");
				return;
			}
			if (auto* race = player->GetRace(); race && clone->GetRace() != race) {
				clone->SwitchRace(race, false);
			}
			PlaceInFront(clone, player);
			clone->StopCombat();
			clone->EnableAI(false);
			{
				std::scoped_lock l{ g_lock };
				g_handle = clone->CreateRefHandle();
			}
			g_gearCountdown = 15;
			g_gearCheck = -1;
			g_viewReady = false;
			g_viewStart = NowMs();
			g_gateLogAt = 0;
			ForceShown(clone);
			SetStatus(std::format("clone {:08X} made; dressing it when its 3D is loaded", clone->GetFormID()));
		}
	}

	void RequestCreate() { SKSE::GetTaskInterface()->AddTask([] { DoTravel(); }); }
	// done from OnFrame, which only runs once the F1 menu is closed (moving the player while it is open left it stuck)
	void RequestRemove() { g_leaveRequested = true; }

	void RequestMoveToFront()
	{
		SKSE::GetTaskInterface()->AddTask([] {
			auto* clone = Get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (clone && player) {
				PlaceInFront(clone, player);
				SetStatus("clone moved in front of you");
			}
		});
	}

	RE::Actor* Get()
	{
		RE::ObjectRefHandle handle;
		{
			std::scoped_lock l{ g_lock };
			handle = g_handle;
		}
		if (!handle) {
			return nullptr;
		}
		auto ref = handle.get();
		if (!ref || ref->IsDeleted() || ref->IsDisabled()) {
			return nullptr;
		}
		return ref->As<RE::Actor>();
	}

	bool InViewingRoom() { return InRoom(RE::PlayerCharacter::GetSingleton()); }

	// 1.8.7: the world may only stand still once the clone is dressed, fully faded in, and the camera is first
	// person in the room (all of those run on game time). After 5 s it stands still anyway, and the log says why.
	bool ViewReady()
	{
		if (g_viewReady) {
			return true;
		}
		auto* clone = Get();
		if (!clone) {
			return false;
		}
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      camera = RE::PlayerCamera::GetSingleton();
		const bool inRoom = InRoom(player);
		const bool dressed = Ready();
		const float fade = FadeOf(clone);
		const bool faded = fade >= 0.99f;
		const bool firstPerson = camera && camera->IsInFirstPerson();
		const auto now = NowMs();
		if (g_viewStart == 0) {
			g_viewStart = now;
		}
		const auto waited = now - g_viewStart;
		if (inRoom && dressed && faded && firstPerson && g_playerHidden) {
			g_viewReady = true;
			logger::info("Clone: gate: all passed after {} ms (dressed, fade {:.2f}, first person, in the room): the world may stand still", waited, fade);
		} else if (waited > 5000) {
			g_viewReady = true;
			logger::warn("Clone: gate: timeout after {} ms, standing the world still anyway: in room {}, dressed {}, fade {:.2f}, first person {}, view set {}",
				waited, inRoom, dressed, fade, firstPerson, g_playerHidden);
		} else if (now - g_gateLogAt >= 1000) {
			g_gateLogAt = now;
			logger::info("Clone: gate: waiting ({} ms): in room {}, dressed {}, fade {:.2f}, first person {}, view set {}", waited, inRoom, dressed, fade,
				firstPerson, g_playerHidden);
		}
		return g_viewReady;
	}

	bool Ready() { return g_ready && Get() != nullptr; }

	void OnFrame()
	{
		auto* ui = RE::UI::GetSingleton();
		if (g_leaveRequested.exchange(false)) {
			SKSE::GetTaskInterface()->AddTask([] { DoRemoveAndReturn(); });
		}
		if (g_reopenF1 && ui && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (player && cell && !IsRoom(cell) && cell->IsAttached() && player->Get3D(false)) {
				g_reopenF1 = false;
				if (auto* window = SKSEMenuFramework::GetMainWindow()) {
					window->IsOpen = true;
				}
			}
		}
		if (InRoom(RE::PlayerCharacter::GetSingleton()) && !g_travelling) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			// the view: first person (the game does not draw the player's own body then), looking straight at the clone.
			// The third-person camera keeps its own direction and height, so the clone was off to the side of it.
			// 1.8.7: the camera switch runs on game time too, so a one-shot ForceFirstPerson could stall in third
			// person. Check every frame and ask again every 0.5 s until it really is first person.
			if (!g_playerHidden && player->Get3D(false)) {
				if (auto* camera = RE::PlayerCamera::GetSingleton()) {
					const auto now = NowMs();
					if (camera->IsInFirstPerson()) {
						player->data.angle.x = 0.0f;  // level
						player->data.angle.z = 0.0f;  // facing +y, where the clone stands
						g_playerHidden = true;
						logger::info("Clone: gate: camera is first person; view set, facing the clone");
					} else if (g_cameraTryAt == 0 || now - g_cameraTryAt >= 500) {
						if (g_cameraTryAt == 0) {
							g_wasThirdPerson = true;
						}
						g_cameraTryAt = now;
						camera->ForceFirstPerson();
						logger::info("Clone: gate: camera not first person yet; ForceFirstPerson asked");
					}
				}
			}
			// the clone stays fully shown (its 3D could be reloaded), and the player's own body is hidden once the
			// clone's 3D exists
			if (auto* clone = Get(); clone && clone->Get3D(false)) {
				ForceShown(clone);
				if (!g_playerCulled) {
					CullPlayer(true);
					ForceShown(clone);  // in case its 3D shares flags with the player's
				} else if (auto* body = player->Get3D(false); body && !body->GetAppCulled()) {
					body->SetAppCulled(true);  // the camera switch can show it again
				}
			}
			// send anyone who came along back to where the player was
			if (++g_evictTimer >= 20) {
				g_evictTimer = 0;
				SKSE::GetTaskInterface()->AddTask([] {
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* lists = RE::ProcessLists::GetSingleton();
					auto* cell = player ? player->GetParentCell() : nullptr;
					RE::ObjectRefHandle markerHandle;
					{
						std::scoped_lock l{ g_lock };
						markerHandle = g_returnMarker;
					}
					auto  marker = markerHandle.get();
					auto* clone = Get();
					if (!lists || !cell || !marker) {
						return;
					}
					for (auto& handle : lists->highActorHandles) {
						auto actor = handle.get();
						if (actor && actor.get() != player && actor.get() != clone && actor->GetParentCell() == cell && !actor->IsDeleted()) {
							logger::info("Clone: {} ({:08X}) followed into the viewing room; sent back", actor->GetName(), actor->GetFormID());
							actor->MoveTo(marker.get());
						}
					}
				});
			}
		}
		if (g_playerCulled && !InRoom(RE::PlayerCharacter::GetSingleton())) {
			CullPlayer(false);
		}
		if (g_gearCheck > 0 && --g_gearCheck == 0) {
			if (auto* clone = Get()) {
				const auto nodes = ChildCount(clone);
				const int  worn = WornCount(clone);
				logger::info("Clone: gear check: {} items worn, 3D nodes {} -> {}", worn, g_nodesBeforeGear, nodes);
				if (worn > 0 && nodes <= g_nodesBeforeGear) {
					logger::warn("Clone: the gear did not attach to the 3D; rebuilding the clone's model");
					clone->Update3DModel();
				}
				ForceShown(clone);
			}
			g_gearCheck = -1;
		}
		if (g_travelling) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (player && IsRoom(cell) && cell->IsAttached() && player->Get3D(false)) {
				g_travelling = false;
				SKSE::GetTaskInterface()->AddTask([] { DoCreate(); });
			}
			return;
		}
		if (g_gearCountdown <= 0) {
			return;
		}
		auto* clone = Get();
		if (!clone) {
			g_gearCountdown = -1;
			return;
		}
		if (clone->Get3D(false) && --g_gearCountdown == 0) {
			// the clone's 3D can be copied from the player's, which is hidden in the room: make sure it is shown
			auto* root = clone->Get3D(false);
			const auto pos = clone->GetPosition();
			logger::info("Clone: 3D loaded (was hidden: {}, fade {:.2f}, alpha {:.2f}), at {:.0f} {:.0f} {:.0f}", root->GetAppCulled(), FadeOf(clone),
				clone->GetAlpha(), pos.x, pos.y, pos.z);
			ForceShown(clone);
			g_nodesBeforeGear = ChildCount(clone);
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				CopyGear(clone, player);
				PlaceInFront(clone, player);
			}
			ForceShown(clone);
			logger::info("Clone: gate: dressed ({} items worn, fade now {:.2f})", WornCount(clone), FadeOf(clone));
			g_gearCountdown = -1;
			g_gearCheck = 10;
			g_ready = true;
		}
	}

	void OnGameLoad()
	{
		{
			std::scoped_lock l{ g_lock };
			g_handle.reset();
		}
		g_travelling = false;
		g_playerHidden = false;
		g_cameraTryAt = 0;
		g_viewReady = false;
		g_viewStart = 0;
		g_gearCheck = -1;
		CullPlayer(false);
		g_reopenF1 = false;
		g_leaveRequested = false;
		if (InRoom(RE::PlayerCharacter::GetSingleton())) {
			RE::FormID id = 0;
			{
				std::scoped_lock l{ g_lock };
				id = g_returnMarkerID;
			}
			if (auto* marker = id ? RE::TESForm::LookupByID<RE::TESObjectREFR>(id) : nullptr) {
				{
					std::scoped_lock l{ g_lock };
					g_returnMarker = marker->CreateRefHandle();
				}
				SKSE::GetTaskInterface()->AddTask([] { DoReturn(); });
				SetStatus("the save was made in the viewing room: going back");
			} else {
				SetStatus("the save was made in the viewing room and the way back is unknown: use a door or fast travel");
			}
		}
		g_ready = false;
		g_gearCountdown = -1;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		auto* base = player ? player->GetActorBase() : nullptr;
		if (!lists || !base) {
			return;
		}
		int removed = 0;
		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			if (actor && actor.get() != player && actor->GetActorBase() == base && actor->IsDynamicForm() && !actor->IsDeleted()) {
				actor->Disable();
				actor->SetDelete(true);
				++removed;
			}
		}
		if (removed) {
			SetStatus(std::format("removed {} clone(s) left in the save", removed));
		}
	}

	// ---------------------------------------------------------------- Esc in the viewing room
	namespace
	{
		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME && InRoom(RE::PlayerCharacter::GetSingleton())) {
					if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
						queue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					}
					logger::info("Clone: Esc in the viewing room: leaving");
					g_leaveRequested = true;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_menuSink;
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		}
	}

	// ---------------------------------------------------------------- co-save: the return marker
	namespace
	{
		constexpr std::uint32_t kRecord = 'RTRN';

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			RE::FormID id = 0;
			{
				std::scoped_lock l{ g_lock };
				id = g_returnMarkerID;
			}
			if (a_intfc->OpenRecord(kRecord, 1)) {
				a_intfc->WriteRecordData(&id, sizeof(id));
			}
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0, version = 0, length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kRecord && length == sizeof(RE::FormID)) {
					RE::FormID id = 0, resolved = 0;
					a_intfc->ReadRecordData(&id, sizeof(id));
					if (id && a_intfc->ResolveFormID(id, resolved)) {
						std::scoped_lock l{ g_lock };
						g_returnMarkerID = resolved;
					}
				}
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			std::scoped_lock l{ g_lock };
			g_returnMarkerID = 0;
			g_returnMarker.reset();
		}
	}

	void InstallSerialization()
	{
		auto* s = SKSE::GetSerializationInterface();
		s->SetUniqueID('AWCL');
		s->SetSaveCallback(OnSave);
		s->SetLoadCallback(OnLoad);
		s->SetRevertCallback(OnRevert);
	}

	std::string Status()
	{
		std::scoped_lock l{ g_lock };
		return g_status;
	}
}
