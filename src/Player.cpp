// Animation player - AnimationWatch 1.6.0 (Skyrim AE 1.6.1170)
//
// Plays any .hkx animation file on the preview clone (Clone.cpp: a copy of the player standing in front of the player;
// made the first time an animation is played), decoded by the game's own Havok code, so the pose is exactly what the
// game itself would show for that file. Used by the Animation Dolls page: click an animation to watch the player do it.
//
// How (read from SkyrimSE.exe 1.6.1170, Address Library ids):
//  1. The file's path ("meshes\actors\character\animations\...\x.hkx") becomes a BSResource::ID with the game's
//     ID::GenerateFromPath (69979). The animation database is a BSResource::EntryDB<BShkbHkxDB::DBTraits> (global
//     pointer id 402581). Checked in game: 38 of the player's first 40 animations are found with that path form.
//  2. Find (64039: db, const ID*, entry** out) returns the entry when the file is loaded, and takes a reference on it.
//     A file that is not loaded yet is queued the way the game queues a clip: AnimationFileManagerSingleton (id 407512)
//     function 63989 (manager, const ID*), under the manager's lock at +0x14, which asks the database to load it in the
//     background; the entry is then looked up again every frame until it is there.
//  3. The entry's data (+0x28) is a BShkbHkxDB::DBData; its +0x20 is the file's hkRootLevelContainer, whose list of
//     named objects holds the hkaAnimationContainer, whose first binding is the hkaAnimationBinding (what 63863 reads).
//  4. Every frame, after the player's own animation update (PlayerCharacter vfunc 0x7D, UpdateAnimation), the
//     animation is sampled with hkaAnimation::SampleTracks (vfunc 3) at the current time, and each track's bone (the
//     binding's track-to-bone map into the player's animation skeleton, bone names = the player's skeleton nodes) gets
//     that position and rotation; then the player's 3D is updated so the new pose is what gets drawn.
//     Havok and the skeleton nodes use different units and the rotation can be stored transposed: both are measured
//     once per play, from the game's current pose of the same bones (hkbCharacter poseLocal) against the nodes.
//  While the clone exists the rest of the game is paused: the global time multiplier (the console's sgtm) is set to
//  0.0001, so the world, NPCs and the player stand still, and it is put back when the clone is removed or a save is
//  loaded. The clone's animation runs on real time, so it plays at its true speed.
//  In the viewing room a control panel (a window of SKSE Menu Framework) has buttons and sliders: pause / play,
//  restart, frame steps, loop, speed, a time slider, pick another animation, leave. When that window pauses the game,
//  the panel itself drives the playback (Present runs on the main thread). Keys also work while no menu is open:
//  Space pause / play, Left / Right one frame back / forward, Up / Down speed, L loop,
//  Backspace restart, F1 pick another animation, Esc leave the room. They act only in the room with no menu open.
//  Controls (from the dolls page): pause / play, restart, loop (on: until Stop; off: once, then the game's own
//  animation takes over), speed 0.05x to 3x, a time slider and frame steps (pause on a single frame). The game is
//  paused while the F1 menu is open, so changes show once it is closed. Root motion is not
//  applied (the player plays it in place); sounds and other events in the file are not fired.
//  Every read of the loaded file goes through a guarded copy, so a file it does not understand gives a message, not a crash.

#include "Player.h"

#include "Clone.h"
#include "FreeCam.h"
#include "SKSEMenuFramework.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <mutex>

namespace AnimPlayer
{
	namespace
	{
		using ID = RE::BSResource::ID;

		// ------------------------------------------------------------------ guarded reads
		bool Read(const void* a_from, void* a_to, std::size_t a_size)
		{
			__try {
				std::memcpy(a_to, a_from, a_size);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::uintptr_t Q(const void* a_base, std::size_t a_offset)
		{
			std::uintptr_t v = 0;
			if (a_base) {
				Read(static_cast<const std::uint8_t*>(a_base) + a_offset, &v, sizeof(v));
			}
			return v;
		}

		std::int32_t I(const void* a_base, std::size_t a_offset)
		{
			std::int32_t v = 0;
			if (a_base) {
				Read(static_cast<const std::uint8_t*>(a_base) + a_offset, &v, sizeof(v));
			}
			return v;
		}

		float F(const void* a_base, std::size_t a_offset)
		{
			float v = 0;
			if (a_base) {
				Read(static_cast<const std::uint8_t*>(a_base) + a_offset, &v, sizeof(v));
			}
			return v;
		}

		void* P(std::uintptr_t a_value) { return reinterpret_cast<void*>(a_value); }

		std::string Str(std::uintptr_t a_text)
		{
			a_text &= ~std::uintptr_t(1);  // hkStringPtr keeps a flag in the low bit
			char buf[256]{};
			for (int i = 0; a_text && i < 255; ++i) {
				if (!Read(reinterpret_cast<const char*>(a_text) + i, &buf[i], 1) || !buf[i]) {
					break;
				}
			}
			return buf;
		}

		// ------------------------------------------------------------------ the game's functions
		using GenId_t = void (*)(ID*, const char*);
		using Find_t = bool (*)(void*, const ID*, void**);
		using Queue_t = std::int32_t (*)(void*, const ID*);
		using Sample_t = void (*)(const void*, float, void*, float*, void*);

		std::uintptr_t Addr(std::uint64_t a_id) { return REL::ID(a_id).address(); }

		void* Database() { return P(Q(P(Addr(402581)), 0)); }

		bool MakeId(const char* a_path, ID& a_id)
		{
			const auto fn = reinterpret_cast<GenId_t>(Addr(69979));
			__try {
				fn(&a_id, a_path);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool Find(const ID& a_id, void*& a_entry)
		{
			const auto fn = reinterpret_cast<Find_t>(Addr(64039));
			void*      db = Database();
			a_entry = nullptr;
			if (!db) {
				return false;
			}
			__try {
				return fn(db, &a_id, &a_entry) && a_entry;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_entry = nullptr;
				return false;
			}
		}

		bool QueueRaw(void* a_manager, const ID* a_id, std::int32_t& a_result)
		{
			const auto fn = reinterpret_cast<Queue_t>(Addr(63989));
			__try {
				a_result = fn(a_manager, a_id);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool Queue(const ID& a_id, std::int32_t& a_result)
		{
			auto* manager = RE::AnimationFileManagerSingleton::GetSingleton();
			if (!manager) {
				return false;
			}
			RE::BSSpinLockGuard guard(manager->lockB);  // +0x14, taken by the game around the same call
			return QueueRaw(manager, &a_id, a_result);
		}

		bool Sample(const void* a_anim, float a_time, float* a_pose, float* a_floats)
		{
			const auto fn = reinterpret_cast<Sample_t>(Q(P(Q(a_anim, 0)), 3 * 8));
			if (!fn) {
				return false;
			}
			__try {
				fn(a_anim, a_time, a_pose, a_floats, nullptr);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// ------------------------------------------------------------------ the loaded file
		void* BindingOf(void* a_entry, std::string& a_error)
		{
			void* data = P(Q(a_entry, 0x28));  // BShkbHkxDB::DBData
			void* root = P(Q(data, 0x20));      // hkRootLevelContainer
			if (!root) {
				a_error = "the file has no contents";
				return nullptr;
			}
			const auto   variants = Q(root, 0);
			const auto   count = I(root, 8);
			void*        container = nullptr;
			for (std::int32_t i = 0; i < count && i < 64 && !container; ++i) {
				const auto v = variants + static_cast<std::uintptr_t>(i) * 0x18;
				if (Str(Q(P(v), 8)) == "hkaAnimationContainer") {
					container = P(Q(P(v), 0x10));
				}
			}
			if (!container) {
				a_error = "the file holds no animation (no hkaAnimationContainer)";
				return nullptr;
			}
			if (I(container, 0x38) <= 0) {
				a_error = "the file's animation container has no binding";
				return nullptr;
			}
			void* binding = P(Q(P(Q(container, 0x30)), 0));
			if (!binding || Q(binding, 0) != RE::VTABLE_hkaAnimationBinding[0].address()) {
				a_error = "the file's binding is not an hkaAnimationBinding";
				return nullptr;
			}
			return binding;
		}

		// ------------------------------------------------------------------ state
		enum class Phase
		{
			kIdle,
			kWaitClone,
			kLoading,
			kPlaying
		};

		struct Track
		{
			RE::NiAVObject* node = nullptr;
			std::int32_t    bone = -1;
		};

		std::mutex  g_lock;
		std::string g_request;       // path asked for by the menu
		std::string g_requestLabel;
		bool        g_stopRequest = false;
		std::string g_status = "idle";

		Phase              g_phase = Phase::kIdle;
		std::string        g_path;
		std::string        g_label;
		ID                 g_id;
		int                g_frames = 0;
		void*              g_anim = nullptr;
		float              g_duration = 0;
		float              g_time = 0;
		int                g_loops = 0;
		float              g_played = 0;
		std::vector<Track> g_tracks;
		std::vector<float> g_pose;
		std::vector<float> g_floats;
		float              g_scale = 1.0f;
		bool               g_transpose = false;
		int                g_frameCount = 0;

		// controls, set from the menu (render thread), read every frame (main thread)
		std::atomic<float> g_speed{ 1.0f };
		std::atomic<bool>  g_paused{ false };
		std::atomic<bool>  g_loop{ true };  // on by default: the clips are often under a second long
		std::atomic<float> g_seek{ -1.0f };
		std::atomic<float> g_shownTime{ 0.0f };
		std::atomic<float> g_shownDuration{ 0.0f };

		SKSEMenuFramework::Model::WindowInterface* g_roomWindow = nullptr;  // the viewing-room control panel
		SKSEMenuFramework::Model::WindowInterface* g_camWindow = nullptr;   // 1.8.9: the free camera / orbit controls

		using Frames_t = std::int32_t (*)(const void*);
		std::int32_t FramesOf(const void* a_anim)
		{
			const auto fn = reinterpret_cast<Frames_t>(Q(P(Q(a_anim, 0)), 8 * 8));
			std::int32_t n = 0;
			if (!fn) {
				return 0;
			}
			__try {
				n = fn(a_anim);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				n = 0;
			}
			return n;
		}

		void SetStatus(std::string a_text)
		{
			logger::info("Animation player: {}", a_text);
			std::scoped_lock l{ g_lock };
			g_status = std::move(a_text);
		}

		void Notify(const std::string& a_text)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false);
		}

		RE::NiMatrix3 Matrix(const float* a_q, bool a_transpose)
		{
			const float x = a_q[0], y = a_q[1], z = a_q[2], w = a_q[3];
			RE::NiMatrix3 m;
			float r[3][3] = {
				{ 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
				{ 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
				{ 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
			};
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					m.entry[i][j] = a_transpose ? r[j][i] : r[i][j];
				}
			}
			return m;
		}

		float Diff(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
		{
			float d = 0;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					d += std::abs(a.entry[i][j] - b.entry[i][j]);
				}
			}
			return d;
		}

		// the player's animation character (behavior graph 0)
		void* PlayerCharacterInstance()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!player || !player->GetAnimationGraphManager(manager) || !manager || manager->graphs.size() == 0 || !manager->graphs[0]) {
				return nullptr;
			}
			return &manager->graphs[0]->characterInstance;
		}

		// map the animation's tracks to the player's skeleton nodes, and measure units / rotation storage
		bool Prepare(void* a_binding, std::string& a_error)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* playerRoot = player ? player->Get3D(false) : nullptr;
			auto* clone = Clone::Get();
			auto* root = clone ? clone->Get3D(false) : nullptr;
			void* character = PlayerCharacterInstance();
			if (!root) {
				a_error = "the clone is not there (or its 3D is not loaded)";
				return false;
			}
			if (!playerRoot || !character) {
				a_error = "the player has no 3D / behavior graph right now";
				return false;
			}
			void* setup = P(Q(character, 0x50));
			void* skeleton = P(Q(setup, 0x20));
			const auto boneArray = Q(skeleton, 0x28);
			const auto boneCount = I(skeleton, 0x30);
			g_anim = P(Q(a_binding, 0x18));
			g_duration = F(g_anim, 0x14);
			const auto tracks = I(g_anim, 0x18);
			const auto floats = I(g_anim, 0x1C);
			if (!g_anim || tracks <= 0 || tracks > 512 || !(g_duration > 0.0f) || g_duration > 600.0f) {
				a_error = std::format("unusable animation ({} bone tracks, {:.2f} s)", tracks, g_duration);
				return false;
			}
			const auto mapData = Q(a_binding, 0x20);
			const auto mapCount = I(a_binding, 0x28);
			g_tracks.assign(tracks, Track{});
			int mapped = 0;
			for (std::int32_t t = 0; t < tracks; ++t) {
				std::int32_t bone = t;
				if (mapCount > 0) {
					std::int16_t b = -1;
					if (t < mapCount) {
						Read(reinterpret_cast<const std::int16_t*>(mapData) + t, &b, 2);
					}
					bone = b;
				}
				if (bone < 0 || bone >= boneCount) {
					continue;
				}
				const auto name = Str(Q(P(boneArray + static_cast<std::uintptr_t>(bone) * 0x10), 0));
				if (name.empty()) {
					continue;
				}
				if (auto* node = root->GetObjectByName(RE::BSFixedString(name.c_str()))) {
					g_tracks[t] = { node, bone };
					++mapped;
				}
			}
			if (mapped < tracks / 2) {
				a_error = std::format("only {} of its {} bone tracks match the player's skeleton (made for another creature or skeleton?)", mapped, tracks);
				return false;
			}
			// units and rotation storage, from the game's own current pose of the same bones
			const auto poseLocal = Q(character, 0x90);
			const auto poseCount = I(character, 0x98);
			std::vector<float> ratios;
			float              straight = 0, transposed = 0;
			int                compared = 0;
			for (const auto& tr : g_tracks) {
				if (!tr.node || !poseLocal || tr.bone >= poseCount) {
					continue;
				}
				// the player's own node of that bone: the player's graph pose is what the player's nodes show
				auto* own = playerRoot->GetObjectByName(tr.node->name);
				if (!own) {
					continue;
				}
				float hk[12]{};
				if (!Read(P(poseLocal + static_cast<std::uintptr_t>(tr.bone) * 0x30), hk, sizeof(hk))) {
					continue;
				}
				const auto& local = own->local;
				const float hkLen = std::sqrt(hk[0] * hk[0] + hk[1] * hk[1] + hk[2] * hk[2]);
				const float niLen = local.translate.Length();
				if (hkLen > 1e-4f && niLen > 1e-3f) {
					ratios.push_back(niLen / hkLen);
				}
				straight += Diff(Matrix(&hk[4], false), local.rotate);
				transposed += Diff(Matrix(&hk[4], true), local.rotate);
				++compared;
			}
			if (!ratios.empty()) {
				std::ranges::sort(ratios);
				g_scale = ratios[ratios.size() / 2];
			} else {
				g_scale = 1.0f;
			}
			g_transpose = compared > 0 && transposed < straight;
			logger::info("Animation player: {} of {} tracks mapped; measured on {} bones: unit scale {:.4f}, rotation {} (differences {:.2f} / {:.2f})", mapped,
				tracks, compared, g_scale, g_transpose ? "transposed" : "as is", straight, transposed);
			g_pose.assign(static_cast<std::size_t>(tracks) * 12 + 4, 0.0f);
			g_floats.assign(static_cast<std::size_t>(std::max(floats, 1)) + 4, 0.0f);
			return true;
		}

		void Apply()
		{
			auto* clone = Clone::Get();
			if (!clone || !clone->Get3D(false)) {
				SetStatus("the clone is gone; stopped");
				g_phase = Phase::kIdle;
				return;
			}
			if (!Sample(g_anim, g_time, g_pose.data(), g_floats.data())) {
				SetStatus("the animation could not be sampled; stopped");
				g_phase = Phase::kIdle;
				return;
			}
			for (std::size_t t = 0; t < g_tracks.size(); ++t) {
				auto* node = g_tracks[t].node;
				if (!node) {
					continue;
				}
				const float* x = &g_pose[t * 12];
				if (!std::isfinite(x[0]) || !std::isfinite(x[1]) || !std::isfinite(x[2]) || !std::isfinite(x[4]) || !std::isfinite(x[7])) {
					continue;
				}
				node->local.translate = RE::NiPoint3{ x[0] * g_scale, x[1] * g_scale, x[2] * g_scale };
				node->local.rotate = Matrix(&x[4], g_transpose);
			}
			if (auto* root = clone->Get3D(false)) {
				if (root->GetAppCulled()) {
					root->SetAppCulled(false);  // copied from the hidden player
				}
				// a new reference fades in over game time, and game time is almost stopped in the room: show it fully
				float fadeBefore = -1.0f;
				if (auto* fade = root->AsFadeNode()) {
					fadeBefore = fade->GetRuntimeData().currentFade;
					fade->GetRuntimeData().currentFade = 1.0f;
				}
				RE::NiUpdateData data{};
				root->Update(data);
				static int frames = 0;
				if (++frames == 1 || frames == 30) {
					auto*      head = root->GetObjectByName(RE::BSFixedString("NPC Head [Head]"));
					const auto w = root->world.translate;
					logger::info("Animation player: clone frame {}: root at {:.0f} {:.0f} {:.0f}, fade was {:.2f}, hidden {}, head at {}", frames, w.x, w.y, w.z,
						fadeBefore, root->GetAppCulled(),
						head ? std::format("{:.0f} {:.0f} {:.0f}", head->world.translate.x, head->world.translate.y, head->world.translate.z) : std::string("(none)"));
					// every mesh of the clone: whether it is hidden and where its bounds are
					int count = 0;
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geo) {
						if (++count <= 40) {
							const auto& b = a_geo->worldBound;
							std::string hiddenParents;
							for (auto* p = a_geo->parent; p; p = p->parent) {
								if (p->GetAppCulled()) {
									hiddenParents += std::format(" '{}'", p->name.c_str());
								}
							}
							logger::info("  mesh '{}': hidden {}, hidden parents{}, bound centre {:.0f} {:.0f} {:.0f} radius {:.0f}", a_geo->name.c_str(),
								a_geo->GetAppCulled(), hiddenParents.empty() ? std::string(" none") : hiddenParents, b.center.x, b.center.y, b.center.z, b.radius);
						}
						return RE::BSVisit::BSVisitControl::kContinue;
					});
					logger::info("  {} meshes in the clone's 3D", count);
					std::string chain;
					for (auto* p = root->parent; p; p = p->parent) {
						chain += std::format(" <- '{}'{}", p->name.c_str(), p->GetAppCulled() ? " (hidden)" : "");
					}
					logger::info("  clone root '{}' is attached{}", root->name.c_str(), chain.empty() ? std::string(": NOT attached to the scene") : chain);
				}
			}
		}

		void StartRequest(const std::string& a_path, const std::string& a_label)
		{
			g_phase = Phase::kIdle;
			g_path = a_path;
			g_label = a_label;
			g_id = ID{};
			if (!MakeId(a_path.c_str(), g_id)) {
				SetStatus(std::format("{}: could not make the file ID", a_label));
				return;
			}
			void* entry = nullptr;
			if (!Find(g_id, entry)) {
				std::int32_t result = 0;
				if (!Queue(g_id, result)) {
					SetStatus(std::format("{}: queuing the file for loading failed", a_label));
					return;
				}
				logger::info("Animation player: {} queued for loading (result {})", a_path, result);
			}
			g_phase = Phase::kLoading;
			g_frames = 0;
			SetStatus(std::format("loading {}", a_label));
		}

		void Begin(void* a_entry)
		{
			std::string error;
			void*       binding = BindingOf(a_entry, error);
			if (!binding || !Prepare(binding, error)) {
				SetStatus(std::format("{}: {}", g_label, error));
				Notify(std::format("AnimationWatch: {}: {}", g_label, error));
				g_phase = Phase::kIdle;
				return;
			}
			g_time = 0;
			g_loops = 0;
			g_played = 0;
			g_frameCount = FramesOf(g_anim);
			g_seek = -1.0f;
			g_paused = false;
			g_shownDuration = g_duration;
			g_phase = Phase::kPlaying;
			SetStatus(std::format("playing {} ({:.2f} s, {} bone tracks)", g_label, g_duration, g_tracks.size()));
			Notify(std::format("AnimationWatch: the clone plays {}", g_label));
		}

		// ------------------------------------------------------------------ the world stands still while the clone exists
		constexpr float kWorldPaused = 0.0001f;
		bool            g_worldPaused = false;
		float           g_savedMultiplier = 1.0f;

		void PauseWorld()
		{
			auto* timer = RE::BSTimer::GetSingleton();
			if (g_worldPaused || !timer) {
				return;
			}
			g_savedMultiplier = RE::BSTimer::QGlobalTimeMultiplier();
			if (!(g_savedMultiplier > kWorldPaused * 10.0f)) {
				g_savedMultiplier = 1.0f;
			}
			timer->SetGlobalTimeMultiplier(kWorldPaused, true);
			g_worldPaused = true;
			logger::info("Animation player: world paused (time multiplier {} -> {})", g_savedMultiplier, kWorldPaused);
		}

		void ResumeWorld()
		{
			auto* timer = RE::BSTimer::GetSingleton();
			if (!g_worldPaused || !timer) {
				return;
			}
			timer->SetGlobalTimeMultiplier(g_savedMultiplier, true);
			g_worldPaused = false;
			logger::info("Animation player: world resumed (time multiplier {})", g_savedMultiplier);
		}

		// real seconds since the last frame the game ran (the F1 menu pauses everything; a long gap counts as one frame)
		float RealDelta()
		{
			static auto last = std::chrono::steady_clock::now();
			const auto  now = std::chrono::steady_clock::now();
			const float dt = std::chrono::duration<float>(now - last).count();
			last = now;
			return dt > 0.25f ? 1.0f / 60.0f : std::max(dt, 0.0f);
		}

		// ------------------------------------------------------------------ per frame, after the player's animation update
		using UpdateAnimation_t = void (*)(RE::PlayerCharacter*, float);
		UpdateAnimation_t g_updateAnimation = nullptr;

		// the viewing-room panel is open in the room, except while the F1 menu itself is open
		void UpdatePanel()
		{
			if (!g_roomWindow) {
				return;
			}
			auto*      main = SKSEMenuFramework::GetMainWindow();
			// 1.8.8: closed while the free camera is on, so the mouse turns the camera instead of moving a cursor
			const bool want = Clone::InViewingRoom() && !(main && main->IsOpen) && !FreeCam::IsOn();
			if (g_roomWindow->IsOpen != want) {
				g_roomWindow->IsOpen = want;
				logger::info("Viewing room: control panel {}", want ? "opened" : "closed");
			}
			// 1.8.9: the camera controls (speed buttons and sliders) while the free camera or orbit view is on
			if (g_camWindow) {
				const bool cam = Clone::InViewingRoom() && !(main && main->IsOpen) && FreeCam::ControlsShown();
				if (g_camWindow->IsOpen != cam) {
					g_camWindow->IsOpen = cam;
					logger::info("Viewing room: camera controls {}", cam ? "shown" : "hidden");
				}
			}
		}

		std::atomic<std::int64_t> g_lastHookTick{ 0 };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// one step of everything: called after the player's update while the game runs, and from the panel while
		// the panel (a menu) has paused the game; both on the main thread
		void Tick(float realDelta)
		{
			// 1.8.7: not as soon as the clone exists: its fade-in, gear and the camera switch all need game time
			if (Clone::Get() && Clone::ViewReady()) {
				PauseWorld();
			} else {
				ResumeWorld();
			}
			std::string request, label;
			bool        stop = false;
			{
				std::scoped_lock l{ g_lock };
				request.swap(g_request);
				label.swap(g_requestLabel);
				stop = g_stopRequest;
				g_stopRequest = false;
			}
			if (stop && g_phase != Phase::kIdle) {
				g_phase = Phase::kIdle;
				SetStatus("stopped");
			}
			Clone::OnFrame();
			FreeCam::Update(realDelta);
			if (!request.empty()) {
				if (Clone::Ready()) {
					StartRequest(request, label);
				} else {
					if (!Clone::Get()) {
						Clone::RequestCreate();
					}
					g_path = request;
					g_label = label;
					g_frames = 0;
					g_phase = Phase::kWaitClone;
					SetStatus(std::format("making the clone, then playing {}", label));
				}
			}
			if (g_phase == Phase::kWaitClone) {
				if (Clone::Ready()) {
					const auto path = g_path, lbl = g_label;
					StartRequest(path, lbl);
				} else if (++g_frames > 900) {
					SetStatus(std::format("the clone was not ready in time: {}", Clone::Status()));
					g_phase = Phase::kIdle;
				}
			}
			if (g_phase == Phase::kLoading) {
				void* entry = nullptr;
				if (Find(g_id, entry)) {
					Begin(entry);
				} else if (++g_frames > 600) {
					SetStatus(std::format("{}: the game did not load the file (is the path right? {})", g_label, g_path));
					Notify(std::format("AnimationWatch: could not load {}", g_label));
					g_phase = Phase::kIdle;
				}
			}
			if (g_phase == Phase::kPlaying) {
				const float seek = g_seek.exchange(-1.0f);
				if (seek >= 0.0f) {
					g_time = std::clamp(seek, 0.0f, g_duration);
				} else if (!g_paused) {
					g_time += std::clamp(realDelta, 0.0f, 0.1f) * std::clamp(g_speed.load(), 0.05f, 3.0f);
				}
				if (g_time >= g_duration) {
					if (g_loop) {
						g_time = std::fmod(g_time, g_duration);
						++g_loops;
					} else {
						g_phase = Phase::kIdle;
						SetStatus(std::format("played {}", g_label));
						return;
					}
				}
				g_shownTime = g_time;
				Apply();
			}
		}

		void UpdateAnimationHook(RE::PlayerCharacter* a_this, float a_delta)
		{
			g_updateAnimation(a_this, a_delta);
			g_lastHookTick = NowMs();
			UpdatePanel();
			Tick(RealDelta());
		}
	}

	// ------------------------------------------------------------------ viewing-room HUD and keys
	namespace
	{
		constexpr float kSpeeds[] = { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };

		void StepSpeed(int a_dir)
		{
			const float now = g_speed;
			int         best = 5;
			for (int i = 0; i < static_cast<int>(std::size(kSpeeds)); ++i) {
				if (std::abs(kSpeeds[i] - now) < std::abs(kSpeeds[best] - now)) {
					best = i;
				}
			}
			best = std::clamp(best + a_dir, 0, static_cast<int>(std::size(kSpeeds)) - 1);
			g_speed = kSpeeds[best];
		}

		bool KeysActive()
		{
			if (!Clone::InViewingRoom()) {
				return false;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::Console::MENU_NAME)) {
				return false;
			}
			auto* window = SKSEMenuFramework::GetMainWindow();
			return !(window && window->IsOpen);
		}

		class KeySink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events || !KeysActive()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* e = *a_events; e; e = e->next) {
					auto* button = e->AsButtonEvent();
					if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
						continue;
					}
					switch (button->GetIDCode()) {
					case 57:  // Space
						g_paused = !g_paused;
						break;
					case 203:  // Left
						StepFrames(-1);
						break;
					case 205:  // Right
						StepFrames(1);
						break;
					case 200:  // Up
						StepSpeed(1);
						break;
					case 208:  // Down
						StepSpeed(-1);
						break;
					case 38:  // L
						g_loop = !g_loop;
						break;
					case 14:  // Backspace
						Restart();
						break;
					case 1:  // Esc: leave the viewing room (the Journal menu it also opens is closed by Clone)
						logger::info("Viewing room: Esc, leaving");
						Clone::RequestRemove();
						break;
					default:
						break;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		KeySink g_keySink;

		bool g_panelLogged = false;

		// the viewing-room control panel: a window of SKSE Menu Framework with buttons and sliders (it takes the mouse;
		// while it pauses the game, it also drives the playback itself)
		void __stdcall RenderRoomPanel()
		{
			using namespace ImGuiMCP;
			if (!g_panelLogged) {
				g_panelLogged = true;
				logger::info("Viewing room: control panel shown");
			}
			if (auto* main = SKSEMenuFramework::GetMainWindow(); (main && main->IsOpen) || !Clone::InViewingRoom()) {
				if (g_roomWindow) {
					g_roomWindow->IsOpen = false;
				}
				return;
			}
			if (NowMs() - g_lastHookTick.load() > 60) {  // the game is paused: play from here
				Tick(RealDelta());
			}
			SetNextWindowPos(ImVec2{ 30.0f, 30.0f }, ImGuiCond_FirstUseEver);
			SetNextWindowBgAlpha(0.85f);
			if (ImGuiMCP::Begin("AnimationWatch viewing room", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
				std::string title;
				{
					std::scoped_lock l{ g_lock };
					title = g_status;
				}
				TextUnformatted(title.c_str());
				const bool playing = g_phase == Phase::kPlaying;
				if (Button(g_paused ? "  Play  " : " Pause  ")) {
					g_paused = !g_paused;
				}
				SameLine();
				if (Button("Restart")) {
					Restart();
				}
				SameLine();
				if (Button("< Frame")) {
					StepFrames(-1);
				}
				SameLine();
				if (Button("Frame >")) {
					StepFrames(1);
				}
				SameLine();
				bool loop = g_loop;
				if (Checkbox("Loop", &loop)) {
					g_loop = loop;
				}
				float speed = g_speed;
				SetNextItemWidth(260.0f);
				if (SliderFloat("Speed", &speed, 0.05f, 3.0f, "%.2fx")) {
					g_speed = std::clamp(speed, 0.05f, 3.0f);
				}
				for (const float preset : { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f }) {
					SameLine();
					if (SmallButton(std::format("{}x", preset).c_str())) {
						g_speed = preset;
					}
				}
				const float dur = g_shownDuration;
				if (playing && dur > 0.0f) {
					float t = g_shownTime;
					SetNextItemWidth(420.0f);
					if (SliderFloat("Time", &t, 0.0f, dur, "%.2f s")) {
						g_paused = true;
						Seek(t);
					}
				}
				Separator();
				if (Button("Pick another animation")) {
					if (auto* main = SKSEMenuFramework::GetMainWindow()) {
						main->IsOpen = true;
					}
					if (g_roomWindow) {
						g_roomWindow->IsOpen = false;
					}
				}
				Spacing();
				PushStyleColor(ImGuiCol_Button, ImVec4{ 0.10f, 0.45f, 0.85f, 1.0f });
				PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.20f, 0.60f, 1.00f, 1.0f });
				PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.05f, 0.30f, 0.65f, 1.0f });
				PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
				const bool freeCam = Button(FreeCam::IsOn() ? "Free camera: ON  (C)" : "Free camera  (C)", ImVec2{ 300.0f, 44.0f });
				SameLine();
				const bool orbitView = Button("Orbit view  (O)", ImVec2{ 220.0f, 44.0f });
				PopStyleColor(4);
				if (freeCam || orbitView) {
					if (freeCam) {
						FreeCam::RequestToggle();
					} else {
						FreeCam::RequestOrbit();
					}
					if (g_roomWindow) {
						g_roomWindow->IsOpen = false;  // the camera controls take its place; C brings this panel back
					}
				}
				SameLine();
				if (Button("Reset view", ImVec2{ 140.0f, 44.0f })) {
					FreeCam::RequestReset();
				}
				SameLine();
				bool invert = FreeCam::InvertY();
				if (Checkbox("Invert mouse Y", &invert)) {
					FreeCam::SetInvertY(invert);
				}
				TextDisabled("%s", "Free camera: hold the right mouse button to look, W/A/S/D move, E up, Q down; speed buttons and slider on screen. Orbit view: the camera circles the clone until you press a key.");
				Spacing();
				PushStyleColor(ImGuiCol_Button, ImVec4{ 0.85f, 0.30f, 0.05f, 1.0f });
				PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 1.00f, 0.45f, 0.10f, 1.0f });
				PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.70f, 0.20f, 0.00f, 1.0f });
				PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
				const bool leave = Button("<<  Leave the viewing room", ImVec2{ 300.0f, 44.0f });
				PopStyleColor(4);
				if (leave) {
					Stop();
					Clone::RequestRemove();
					if (g_roomWindow) {
						g_roomWindow->IsOpen = false;
					}
				}
			}
			ImGuiMCP::End();
		}

		// 1.8.9: the free camera / orbit controls, a small window at the bottom middle of the screen
		void __stdcall RenderCamControls()
		{
			using namespace ImGuiMCP;
			if (auto* main = SKSEMenuFramework::GetMainWindow(); (main && main->IsOpen) || !Clone::InViewingRoom() || !FreeCam::ControlsShown()) {
				if (g_camWindow) {
					g_camWindow->IsOpen = false;
				}
				return;
			}
			if (NowMs() - g_lastHookTick.load() > 60) {  // the game is paused: play from here
				Tick(RealDelta());
			}
			ImVec2 at{ 640.0f, 700.0f };
			if (auto* io = GetIO()) {
				at = ImVec2{ io->DisplaySize.x * 0.5f, io->DisplaySize.y - 24.0f };
			}
			SetNextWindowPos(at, ImGuiCond_Always, ImVec2{ 0.5f, 1.0f });
			SetNextWindowBgAlpha(0.75f);
			if (ImGuiMCP::Begin("AnimationWatch camera", nullptr,
					ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
				FreeCam::RenderControls();
			}
			ImGuiMCP::End();
		}

		void __stdcall RenderHud()
		{
			using namespace ImGuiMCP;
			if (!Clone::InViewingRoom()) {
				return;
			}
			auto* dl = GetForegroundDrawList();
			if (!dl) {
				return;
			}
			std::string title;
			{
				std::scoped_lock l{ g_lock };
				title = g_status;
			}
			const float dur = g_shownDuration;
			std::string line2 = std::format("{:.2f} / {:.2f} s    speed {:.2f}x    loop {}    {}", g_shownTime.load(), dur, g_speed.load(), g_loop ? "on" : "off",
				g_phase == Phase::kPlaying ? (g_paused ? "PAUSED" : "playing") : "stopped");
			const char* keys = "Space pause/play   Left/Right frame   Up/Down speed   L loop   Backspace restart   F1 pick another   Esc leave";
			const std::string camKeys = FreeCam::KeyHelp();
			const ImVec2 at{ 30.0f, 30.0f };
			const float  lh = GetTextLineHeight() + 4.0f;
			const auto   w = std::max({ CalcTextSize(title.c_str()).x, CalcTextSize(line2.c_str()).x, CalcTextSize(keys).x, CalcTextSize(camKeys.c_str()).x }) + 24.0f;
			ImDrawListManager::AddRectFilled(dl, ImVec2{ at.x - 12.0f, at.y - 10.0f }, ImVec2{ at.x - 12.0f + w, at.y + lh * 4.0f + 6.0f }, IM_COL32(15, 12, 10, 200), 6.0f, 0);
			ImDrawListManager::AddText(dl, at, IM_COL32(243, 219, 190, 255), title.c_str());
			ImDrawListManager::AddText(dl, ImVec2{ at.x, at.y + lh }, IM_COL32(120, 205, 255, 255), line2.c_str());
			ImDrawListManager::AddText(dl, ImVec2{ at.x, at.y + lh * 2.0f }, IM_COL32(200, 200, 200, 255), keys);
			ImDrawListManager::AddText(dl, ImVec2{ at.x, at.y + lh * 3.0f }, FreeCam::IsOn() ? IM_COL32(255, 170, 60, 255) : IM_COL32(200, 200, 200, 255), camKeys.c_str());
		}
	}

	void Install()
	{
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink<RE::InputEvent*>(&g_keySink);
		}
		if (SKSEMenuFramework::IsInstalled()) {
			g_roomWindow = SKSEMenuFramework::AddWindow(RenderRoomPanel, true);
			g_camWindow = SKSEMenuFramework::AddWindow(RenderCamControls, true);
			logger::info("Camera controls window {}", g_camWindow ? "registered" : "NOT registered");
			logger::info("Viewing room control panel {}", g_roomWindow ? "registered" : "NOT registered");
		}
		if (REL::Module::get().version() != REL::Version{ 1, 6, 1170, 0 }) {
			SetStatus("the animation player is written for Skyrim 1.6.1170 and is off on this version");
			return;
		}
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		g_updateAnimation = reinterpret_cast<UpdateAnimation_t>(vtbl.write_vfunc(0x7D, &UpdateAnimationHook));
		logger::info("Animation player installed (PlayerCharacter::UpdateAnimation)");
	}

	void Play(const std::string& a_dataPath, const std::string& a_label)
	{
		std::scoped_lock l{ g_lock };
		if (!g_updateAnimation) {
			return;
		}
		g_request = a_dataPath;
		g_requestLabel = a_label;
		g_status = std::format("{}: waiting for the game to run (close the menu)", a_label);
	}

	void OnGameLoad()
	{
		g_phase = Phase::kIdle;
		ResumeWorld();
	}

	void Stop()
	{
		std::scoped_lock l{ g_lock };
		g_stopRequest = true;
		g_request.clear();
	}

	bool IsBusy() { return g_phase != Phase::kIdle; }

	bool  IsPaused() { return g_paused; }
	void  SetPaused(bool a_paused) { g_paused = a_paused; }
	float Speed() { return g_speed; }
	void  SetSpeed(float a_speed) { g_speed = std::clamp(a_speed, 0.05f, 3.0f); }
	bool  Loop() { return g_loop; }
	void  SetLoop(bool a_loop) { g_loop = a_loop; }
	float Time() { return g_shownTime; }
	float Duration() { return g_shownDuration; }

	void Seek(float a_time)
	{
		g_seek = std::max(0.0f, a_time);
		g_shownTime = a_time;
	}

	void Restart()
	{
		Seek(0.0f);
		g_paused = false;
	}

	void StepFrames(int a_frames)
	{
		const float duration = g_shownDuration;
		const int   frames = g_frameCount > 1 ? g_frameCount - 1 : static_cast<int>(duration * 30.0f);
		const float step = frames > 0 ? duration / static_cast<float>(frames) : 1.0f / 30.0f;
		g_paused = true;
		Seek(std::clamp(g_shownTime.load() + step * static_cast<float>(a_frames), 0.0f, std::max(0.0f, duration - 0.0001f)));
	}

	std::string Status()
	{
		std::scoped_lock l{ g_lock };
		return g_status;
	}
}
