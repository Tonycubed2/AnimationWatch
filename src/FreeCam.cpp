// Free camera and orbit view in the viewing room - AnimationWatch 1.8.9
//
// The game's own free camera state (PlayerCamera kFree, the console's "tfc") is entered and left with
// PlayerCamera::ToggleFreeCameraMode(false), the way po3's Photo Mode does it. While the world stands still (global
// time multiplier 0.0001) the game's own free-camera movement runs on game time and would barely move, so this file
// drives the camera itself: it reads the mouse and keys from the input events, keeps its own position / pitch / yaw,
// moves with REAL time (the frame time the animation player measures with steady_clock) and writes them every frame
// into FreeCameraState::translation and ::rotation (x = pitch, y = yaw, the same fields and order Photo Mode's IGCS
// bridge reads and writes). Forward / right come from the game's own convention for that state (Photo Mode:
// EulerAnglesToAxesZXY(pitch, 0, yaw): right = (cos y, -sin y, 0), forward = (sin y cos p, cos y cos p, -sin p)).
// Whatever the game's own free-camera code adds in between is overwritten the next frame.
//
// 1.8.9: twice the speed (400 units/s to start, 40 to 6000); on-screen camera controls at the bottom of the screen
// (Slower / Faster buttons and a speed slider, orbit sliders) instead of keys for the speed. With the controls shown
// the mouse is a cursor for them and the camera turns while the RIGHT mouse button is held; "Hide controls" gives
// the 1.8.8 behaviour (the mouse always turns the camera) and a right click brings the controls back.
// Orbit view: the camera circles the clone by itself, looking at its chest, on real time. Any keyboard key or a right
// click stops it (left clicks are for the on-screen controls, and the wheel / + / - only change the speed). Started
// from the free camera it goes back to the free camera where it stopped; started from the normal view it goes back
// to the normal view.

#include "FreeCam.h"

#include "Clone.h"
#include "SKSEMenuFramework.h"

namespace FreeCam
{
	namespace
	{
		enum class Mode
		{
			kOff,
			kEntering,  // asked the game for the free camera, waiting for the state
			kOn,
			kOrbit
		};

		constexpr std::uint32_t kKeyW = 17, kKeyA = 30, kKeyS = 31, kKeyD = 32, kKeyE = 18, kKeyQ = 16, kKeyC = 46, kKeyR = 19, kKeyO = 24;
		constexpr std::uint32_t kKeyLShift = 42, kKeyRShift = 54, kKeyMinus = 12, kKeyEquals = 13, kKeyNumMinus = 74, kKeyNumPlus = 78;
		constexpr float         kMinSpeed = 40.0f, kMaxSpeed = 6000.0f, kSensitivity = 0.0035f;
		constexpr float         kChest = 85.0f;  // the point the orbit looks at, above the clone's feet

		Mode               g_mode = Mode::kOff;
		std::int64_t       g_enterAt = 0;
		std::atomic<bool>  g_toggleRequest{ false };
		std::atomic<bool>  g_orbitRequest{ false };
		std::atomic<bool>  g_stopOrbitRequest{ false };
		std::atomic<bool>  g_resetRequest{ false };
		std::atomic<bool>  g_wantOn{ false };  // for the buttons (render thread)
		std::atomic<bool>  g_orbiting{ false };
		std::atomic<bool>  g_controls{ true };  // on-screen controls shown (mouse = cursor, right button = look)
		std::atomic<float> g_speed{ 400.0f };
		std::atomic<float> g_orbitPeriod{ 20.0f };   // seconds for one circle
		std::atomic<float> g_orbitRadius{ 250.0f };
		std::atomic<float> g_orbitHeight{ 40.0f };   // above the chest
		std::atomic<bool>  g_invertY{ false };
		bool               g_orbitPending = false;   // orbit as soon as the free camera state exists
		bool               g_orbitFromFree = false;
		float              g_orbitAngle = 0.0f;
		RE::NiPoint3       g_pos;
		float              g_pitch = 0.0f, g_yaw = 0.0f;
		int                g_restoreFrames = 0;   // after leaving: make sure the room view is first person again
		int                g_noAutoMove = 0;      // frames to cancel the game's auto-move (C)
		std::int64_t       g_lastPoseLog = 0;
		std::int64_t       g_speedShownAt = 0;    // when the speed last changed (shown on screen for 2 s)

		// input, written by the input sink (main thread), read by Update (main thread)
		std::mutex g_inputLock;
		float      g_mouseX = 0.0f, g_mouseY = 0.0f;
		bool       g_held[6]{};  // W S A D E Q
		bool       g_shift = false;
		bool       g_rightHeld = false;
		int        g_speedSteps = 0;

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		RE::FreeCameraState* State()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->IsInFreeCameraMode() || !camera->currentState) {
				return nullptr;
			}
			return static_cast<RE::FreeCameraState*>(camera->currentState.get());
		}

		void SetSpeed(float a_speed, const char* a_how)
		{
			const float s = std::clamp(a_speed, kMinSpeed, kMaxSpeed);
			if (std::abs(s - g_speed.load()) > 0.01f) {
				g_speed = s;
				g_speedShownAt = NowMs();
				logger::info("Free camera: speed {:.0f} units/s ({})", s, a_how);
			}
		}

		void ClearInput()
		{
			std::scoped_lock l{ g_inputLock };
			g_mouseX = g_mouseY = 0.0f;
			for (auto& h : g_held) {
				h = false;
			}
			g_shift = false;
			g_rightHeld = false;
			g_speedSteps = 0;
		}

		// the game's own menus take the keys; the on-screen camera controls (a Menu Framework window) do not
		bool MenusAllowInput()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			for (const auto& name : { RE::LoadingMenu::MENU_NAME, RE::Console::MENU_NAME, RE::JournalMenu::MENU_NAME, RE::InventoryMenu::MENU_NAME,
					 RE::MagicMenu::MENU_NAME, RE::MapMenu::MENU_NAME, RE::TweenMenu::MENU_NAME, RE::FavoritesMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
					 RE::MessageBoxMenu::MENU_NAME }) {
				if (ui->IsMenuOpen(name)) {
					return false;
				}
			}
			auto* window = SKSEMenuFramework::GetMainWindow();
			return !(window && window->IsOpen);
		}

		void FaceClone()
		{
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				player->data.angle.x = 0.0f;  // level
				player->data.angle.z = 0.0f;  // facing +y, where the clone stands
			}
		}

		void LeaveState(const char* a_why)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			const bool wasFree = camera && camera->IsInFreeCameraMode();
			if (wasFree) {
				camera->ToggleFreeCameraMode(false);
			}
			const bool stillFree = camera && camera->IsInFreeCameraMode();
			logger::info("Free camera: off ({}){}", a_why, stillFree ? "; ERROR: the game is still in its free camera" : "");
			g_mode = Mode::kOff;
			g_wantOn = false;
			g_orbiting = false;
			g_orbitPending = false;
			g_restoreFrames = 30;
			ClearInput();
		}

		void Enter(bool a_orbit)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				logger::error("Free camera: no player camera");
				return;
			}
			if (!Clone::InViewingRoom()) {
				logger::info("Free camera: only in the viewing room");
				g_wantOn = false;
				return;
			}
			ClearInput();
			if (!camera->IsInFreeCameraMode()) {
				camera->ToggleFreeCameraMode(false);
			}
			g_mode = Mode::kEntering;
			g_enterAt = NowMs();
			g_wantOn = true;
			g_orbitPending = a_orbit;
			g_orbitFromFree = false;
			g_orbiting = a_orbit;
			g_controls = true;
			logger::info("Free camera: turning on{} (game free camera asked: {})", a_orbit ? " for the orbit view" : "",
				camera->IsInFreeCameraMode() ? "on at once" : "pending");
		}

		// the game's free camera state exists: start from where it begins (the current view)
		void Adopt(RE::FreeCameraState* a_state)
		{
			g_pos = a_state->translation;
			g_pitch = a_state->rotation.x;
			g_yaw = a_state->rotation.y;
			if (!std::isfinite(g_pos.x) || !std::isfinite(g_pos.y) || !std::isfinite(g_pos.z) || !std::isfinite(g_pitch) || !std::isfinite(g_yaw)) {
				g_pos = RE::PlayerCamera::GetActiveCameraPosition();
				g_pitch = 0.0f;
				g_yaw = 0.0f;
				logger::warn("Free camera: the game's start values were not numbers; starting at the camera position");
			}
			g_mode = Mode::kOn;
			logger::info("Free camera: on after {} ms, at {:.0f} {:.0f} {:.0f}, pitch {:.2f}, yaw {:.2f}", NowMs() - g_enterAt, g_pos.x, g_pos.y, g_pos.z, g_pitch, g_yaw);
		}

		void BeginOrbit()
		{
			auto* clone = Clone::Get();
			if (!clone) {
				logger::warn("Free camera: orbit view needs the clone; staying in the free camera");
				g_orbiting = false;
				if (!g_orbitFromFree) {
					LeaveState("orbit view without a clone");
				}
				return;
			}
			const auto   at = clone->GetPosition();
			const RE::NiPoint3 centre{ at.x, at.y, at.z + kChest };
			const float  dx = g_pos.x - centre.x, dy = g_pos.y - centre.y;
			const float  horiz = std::sqrt(dx * dx + dy * dy);
			g_orbitRadius = horiz > 60.0f ? std::clamp(horiz, 100.0f, 800.0f) : 250.0f;
			g_orbitAngle = horiz > 1.0f ? std::atan2(dx, dy) : RE::NI_PI;  // where the camera is now (default: in front of the clone)
			g_mode = Mode::kOrbit;
			g_orbiting = true;
			ClearInput();
			logger::info("Free camera: orbit view on (from {}), distance {:.0f}, {:.0f} s a circle", g_orbitFromFree ? "the free camera" : "the normal view",
				g_orbitRadius.load(), g_orbitPeriod.load());
		}

		void StopOrbit(const char* a_why)
		{
			if (g_mode != Mode::kOrbit) {
				return;
			}
			if (g_orbitFromFree) {
				g_mode = Mode::kOn;  // carry on flying from where the orbit was
				g_orbiting = false;
				ClearInput();
				logger::info("Free camera: orbit view off ({}); back to the free camera", a_why);
			} else {
				logger::info("Free camera: orbit view off ({}); back to the normal view", a_why);
				LeaveState("orbit view stopped");
			}
		}

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events || !Clone::InViewingRoom() || !MenusAllowInput()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ g_inputLock };
				const bool on = g_mode != Mode::kOff;
				const bool orbit = g_mode == Mode::kOrbit;
				for (auto* e = *a_events; e; e = e->next) {
					if (auto* move = e->AsMouseMoveEvent()) {
						// with the on-screen controls shown the mouse is a cursor: it turns the camera only with the right button held
						if (on && !orbit && (!g_controls || g_rightHeld)) {
							g_mouseX += static_cast<float>(move->mouseInputX);
							g_mouseY += static_cast<float>(move->mouseInputY);
						}
						continue;
					}
					auto* button = e->AsButtonEvent();
					if (!button) {
						continue;
					}
					const auto id = button->GetIDCode();
					if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
						if (!on) {
							continue;
						}
						if (id == RE::BSWin32MouseDevice::Key::kRightButton) {
							g_rightHeld = button->IsPressed();
							if (button->IsDown()) {
								if (orbit) {
									g_stopOrbitRequest = true;
								} else if (!g_controls) {
									g_controls = true;  // a right click brings the on-screen controls back
								}
							}
						} else if (button->IsDown()) {
							if (id == RE::BSWin32MouseDevice::Key::kWheelUp) {
								++g_speedSteps;
							} else if (id == RE::BSWin32MouseDevice::Key::kWheelDown) {
								--g_speedSteps;
							}
						}
						continue;
					}
					if (button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}
					const bool speedKey = id == kKeyEquals || id == kKeyNumPlus || id == kKeyMinus || id == kKeyNumMinus;
					if (speedKey) {
						if (on && button->IsDown()) {
							g_speedSteps += (id == kKeyEquals || id == kKeyNumPlus) ? 1 : -1;
						}
						continue;
					}
					if (orbit) {
						if (button->IsDown()) {  // any other key stops the orbit (and does nothing else here)
							g_stopOrbitRequest = true;
							if (id == kKeyC) {
								g_noAutoMove = 10;
							}
						}
						continue;
					}
					if (id == kKeyC && button->IsDown()) {
						g_toggleRequest = true;
						g_noAutoMove = 10;
						continue;
					}
					if (id == kKeyO && button->IsDown()) {
						g_orbitRequest = true;
						continue;
					}
					if (!on) {
						continue;
					}
					const bool pressed = button->IsPressed();
					switch (id) {
					case kKeyW: g_held[0] = pressed; break;
					case kKeyS: g_held[1] = pressed; break;
					case kKeyA: g_held[2] = pressed; break;
					case kKeyD: g_held[3] = pressed; break;
					case kKeyE: g_held[4] = pressed; break;
					case kKeyQ: g_held[5] = pressed; break;
					case kKeyLShift:
					case kKeyRShift: g_shift = pressed; break;
					case kKeyR:
						if (button->IsDown()) {
							g_resetRequest = true;
						}
						break;
					default: break;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		InputSink g_sink;

		void WritePose(RE::FreeCameraState* a_state)
		{
			a_state->translation = g_pos;
			a_state->rotation.x = g_pitch;
			a_state->rotation.y = g_yaw;
		}
	}

	void Install()
	{
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink<RE::InputEvent*>(&g_sink);
			logger::info("Free camera: input sink installed");
		} else {
			logger::error("Free camera: no input device manager; the free camera keys will not work");
		}
	}

	void RequestToggle() { g_toggleRequest = true; }
	void RequestOrbit() { g_orbitRequest = true; }
	void RequestReset() { g_resetRequest = true; }
	bool IsOn() { return g_wantOn; }
	bool IsOrbiting() { return g_wantOn && g_orbiting; }
	bool ControlsShown() { return g_wantOn && g_controls; }
	float Speed() { return g_speed; }
	bool InvertY() { return g_invertY; }
	void SetInvertY(bool a_invert) { g_invertY = a_invert; }

	void Off(const char* a_why)
	{
		g_toggleRequest = false;
		g_orbitRequest = false;
		g_stopOrbitRequest = false;
		if (g_mode != Mode::kOff) {
			LeaveState(a_why);
		}
		g_restoreFrames = 0;  // the caller sets the camera itself (leaving the room / a new clone)
	}

	void OnGameLoad()
	{
		if (g_mode != Mode::kOff) {
			logger::info("Free camera: off (game loaded)");
		}
		g_mode = Mode::kOff;
		g_wantOn = false;
		g_orbiting = false;
		g_orbitPending = false;
		g_toggleRequest = false;
		g_orbitRequest = false;
		g_stopOrbitRequest = false;
		g_resetRequest = false;
		g_restoreFrames = 0;
		ClearInput();
	}

	void Update(float a_realDelta)
	{
		// cancel the auto-move the game starts on C (it would walk the player after leaving the room)
		if (g_noAutoMove > 0) {
			--g_noAutoMove;
			if (auto* controls = RE::PlayerControls::GetSingleton(); controls && controls->data.autoMove) {
				controls->data.autoMove = false;
			}
		}
		const bool inRoom = Clone::InViewingRoom();
		if (g_mode != Mode::kOff && !inRoom) {
			LeaveState("not in the viewing room");
			g_restoreFrames = 0;
		}
		if (g_stopOrbitRequest.exchange(false)) {
			StopOrbit("a key or right click");
		}
		if (g_toggleRequest.exchange(false)) {
			if (g_mode == Mode::kOff) {
				Enter(false);
			} else if (g_mode == Mode::kOrbit && !g_orbitFromFree) {
				g_orbitFromFree = true;  // the Free camera button while orbiting: fly from here
				StopOrbit("free camera button");
			} else {
				LeaveState("toggled");
			}
		}
		if (g_orbitRequest.exchange(false)) {
			if (g_mode == Mode::kOff) {
				Enter(true);
			} else if (g_mode == Mode::kOrbit) {
				StopOrbit("orbit button");
			} else if (g_mode == Mode::kOn) {
				g_orbitFromFree = true;
				BeginOrbit();
			} else {
				g_orbitPending = true;  // still entering
				g_orbiting = true;
			}
		}
		if (g_resetRequest.exchange(false)) {
			if (g_mode != Mode::kOff) {
				LeaveState("reset view");
			} else {
				g_restoreFrames = 30;
			}
			FaceClone();
			logger::info("Free camera: view reset (first person, facing the clone)");
		}
		// after leaving: the game goes back to the state before the free camera; in the room that must be first person
		if (g_mode == Mode::kOff && g_restoreFrames > 0 && inRoom) {
			--g_restoreFrames;
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && !camera->IsInFreeCameraMode() && !camera->IsInFirstPerson() &&
																 (g_restoreFrames % 10) == 0) {
				camera->ForceFirstPerson();
				logger::info("Free camera: the room view was not first person; ForceFirstPerson asked");
			}
			if (g_restoreFrames == 0) {
				FaceClone();
			}
		}
		if (g_mode == Mode::kEntering) {
			if (auto* state = State()) {
				Adopt(state);
				if (g_orbitPending) {
					g_orbitPending = false;
					BeginOrbit();
				}
			} else if (NowMs() - g_enterAt > 1500) {
				logger::error("Free camera: ERROR: the game did not enter its free camera within 1.5 s; giving up");
				g_mode = Mode::kOff;
				g_wantOn = false;
				g_orbiting = false;
				g_orbitPending = false;
				return;
			} else {
				return;
			}
		}
		if (g_mode != Mode::kOn && g_mode != Mode::kOrbit) {
			return;
		}
		auto* state = State();
		if (!state) {
			logger::warn("Free camera: the game left its free camera by itself; off");
			g_mode = Mode::kOff;
			g_wantOn = false;
			g_orbiting = false;
			g_restoreFrames = 30;
			ClearInput();
			return;
		}
		float mx = 0, my = 0;
		bool  held[6]{};
		bool  shift = false;
		int   steps = 0;
		if (MenusAllowInput()) {
			std::scoped_lock l{ g_inputLock };
			mx = g_mouseX;
			my = g_mouseY;
			std::copy(std::begin(g_held), std::end(g_held), std::begin(held));
			shift = g_shift;
			steps = g_speedSteps;
			g_mouseX = g_mouseY = 0.0f;
			g_speedSteps = 0;
		} else {
			ClearInput();  // a menu took the keys: nothing stays held
		}
		if (steps != 0) {
			SetSpeed(g_speed.load() * std::pow(1.25f, static_cast<float>(steps)), "wheel / + -");
		}
		const float dt = std::clamp(a_realDelta, 0.0f, 0.1f);

		if (g_mode == Mode::kOrbit) {
			auto* clone = Clone::Get();
			if (!clone) {
				g_orbitFromFree = true;  // keep the camera where it is
				StopOrbit("the clone is gone");
				return;
			}
			const auto at = clone->GetPosition();
			const RE::NiPoint3 centre{ at.x, at.y, at.z + kChest };
			g_orbitAngle += (2.0f * RE::NI_PI / std::max(g_orbitPeriod.load(), 2.0f)) * dt;
			if (g_orbitAngle > 2.0f * RE::NI_PI) {
				g_orbitAngle -= 2.0f * RE::NI_PI;
			}
			const float r = g_orbitRadius;
			g_pos = RE::NiPoint3{ centre.x + r * std::sin(g_orbitAngle), centre.y + r * std::cos(g_orbitAngle), centre.z + g_orbitHeight.load() };
			const float dx = centre.x - g_pos.x, dy = centre.y - g_pos.y, dz = centre.z - g_pos.z;
			g_yaw = std::atan2(dx, dy);
			g_pitch = std::atan2(-dz, std::sqrt(dx * dx + dy * dy));  // forward z = -sin(pitch)
			WritePose(state);
			return;
		}

		g_yaw += mx * kSensitivity;
		g_pitch += (g_invertY ? -my : my) * kSensitivity;
		g_pitch = std::clamp(g_pitch, -1.55f, 1.55f);
		if (g_yaw > RE::NI_PI * 2.0f || g_yaw < -RE::NI_PI * 2.0f) {
			g_yaw = std::fmod(g_yaw, RE::NI_PI * 2.0f);
		}
		const float sy = std::sin(g_yaw), cy = std::cos(g_yaw), sp = std::sin(g_pitch), cp = std::cos(g_pitch);
		const RE::NiPoint3 forward{ sy * cp, cy * cp, -sp };
		const RE::NiPoint3 right{ cy, -sy, 0.0f };
		const float        step = g_speed * (shift ? 4.0f : 1.0f) * dt;
		RE::NiPoint3       move{};
		if (held[0]) move += forward;
		if (held[1]) move -= forward;
		if (held[3]) move += right;
		if (held[2]) move -= right;
		if (held[4]) move.z += 1.0f;
		if (held[5]) move.z -= 1.0f;
		g_pos += move * step;
		WritePose(state);
		if (const auto now = NowMs(); (mx != 0 || my != 0 || move.x != 0 || move.y != 0 || move.z != 0) && now - g_lastPoseLog > 2000) {
			g_lastPoseLog = now;
			logger::info("Free camera: at {:.0f} {:.0f} {:.0f}, pitch {:.2f}, yaw {:.2f}", g_pos.x, g_pos.y, g_pos.z, g_pitch, g_yaw);
		}
	}

	// the on-screen camera controls (drawn by Player.cpp's camera-controls window, bottom middle of the screen)
	void RenderControls()
	{
		using namespace ImGuiMCP;
		const bool orbit = IsOrbiting();
		auto blue = [] {
			PushStyleColor(ImGuiCol_Button, ImVec4{ 0.10f, 0.45f, 0.85f, 1.0f });
			PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.20f, 0.60f, 1.00f, 1.0f });
			PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.05f, 0.30f, 0.65f, 1.0f });
			PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
		};
		const ImVec2 big{ 130.0f, 40.0f };

		TextUnformatted(orbit ? "ORBIT VIEW: the camera circles the clone. Any key or a right click stops it." :
								"FREE CAMERA: hold the RIGHT mouse button to look around. W/A/S/D move, E up, Q down, Shift faster.");
		// movement speed
		blue();
		const bool slower = Button("<< Slower", big);
		PopStyleColor(4);
		if (slower) {
			SetSpeed(g_speed.load() / 1.5f, "Slower button");
		}
		SameLine();
		float speed = g_speed;
		SetNextItemWidth(320.0f);
		if (SliderFloat("##camspeed", &speed, kMinSpeed, kMaxSpeed, "camera speed %.0f", ImGuiSliderFlags_Logarithmic)) {
			SetSpeed(speed, "slider");
		}
		SameLine();
		blue();
		const bool faster = Button("Faster >>", big);
		PopStyleColor(4);
		if (faster) {
			SetSpeed(g_speed.load() * 1.5f, "Faster button");
		}
		// orbit
		if (orbit) {
			blue();
			const bool oSlower = Button("<< Orbit slower", big);
			PopStyleColor(4);
			if (oSlower) {
				g_orbitPeriod = std::clamp(g_orbitPeriod.load() * 1.5f, 4.0f, 120.0f);
				logger::info("Free camera: orbit {:.0f} s a circle", g_orbitPeriod.load());
			}
			SameLine();
			float period = g_orbitPeriod;
			SetNextItemWidth(320.0f);
			if (SliderFloat("##orbitperiod", &period, 4.0f, 120.0f, "%.0f s a circle", ImGuiSliderFlags_Logarithmic)) {
				g_orbitPeriod = std::clamp(period, 4.0f, 120.0f);
			}
			SameLine();
			blue();
			const bool oFaster = Button("Orbit faster >>", big);
			PopStyleColor(4);
			if (oFaster) {
				g_orbitPeriod = std::clamp(g_orbitPeriod.load() / 1.5f, 4.0f, 120.0f);
				logger::info("Free camera: orbit {:.0f} s a circle", g_orbitPeriod.load());
			}
			float radius = g_orbitRadius;
			SetNextItemWidth(250.0f);
			if (SliderFloat("Distance##orbit", &radius, 100.0f, 800.0f, "%.0f")) {
				g_orbitRadius = radius;
			}
			SameLine();
			float height = g_orbitHeight;
			SetNextItemWidth(250.0f);
			if (SliderFloat("Height##orbit", &height, -80.0f, 300.0f, "%.0f")) {
				g_orbitHeight = height;
			}
		}
		// mode buttons
		blue();
		const bool orbitButton = Button(orbit ? "Stop orbit" : "Orbit view (O)", ImVec2{ 170.0f, 40.0f });
		PopStyleColor(4);
		if (orbitButton) {
			RequestOrbit();
		}
		SameLine();
		if (Button(orbit ? "Fly from here" : "Free camera off (C)", ImVec2{ 170.0f, 40.0f })) {
			RequestToggle();
		}
		SameLine();
		if (Button("Reset view (R)", ImVec2{ 140.0f, 40.0f })) {
			RequestReset();
		}
		if (!orbit) {
			SameLine();
			if (Button("Hide controls", ImVec2{ 140.0f, 40.0f })) {
				g_controls = false;  // the mouse always turns the camera now; a right click brings the controls back
				logger::info("Free camera: on-screen controls hidden (right click brings them back)");
			}
			SameLine();
			bool invert = g_invertY;
			if (Checkbox("Invert mouse Y", &invert)) {
				g_invertY = invert;
			}
		}
	}

	std::string KeyHelp()
	{
		const bool speedFlash = NowMs() - g_speedShownAt < 2000;
		const std::string speed = speedFlash ? std::format(">> CAMERA SPEED {:.0f} <<", g_speed.load()) : std::format("speed {:.0f}", g_speed.load());
		if (IsOrbiting()) {
			return std::format("ORBIT VIEW: any key or right click stops it   ({:.0f} s a circle, distance {:.0f})", g_orbitPeriod.load(), g_orbitRadius.load());
		}
		if (g_wantOn) {
			return std::format("FREE CAMERA: {}   W/A/S/D move   E up / Q down   Shift faster   {}   C off   R reset   O orbit",
				g_controls ? "hold right mouse to look" : "mouse look (right click: controls)", speed);
		}
		return "C free camera   O orbit view (the camera circles the clone)";
	}
}
