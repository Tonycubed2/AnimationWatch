// AnimationWatch 1.8.9
// Built against CommonLibSSE-NG for Skyrim AE 1.6.1170 (Address Library).
//
// Watches every animation clip the player's behavior graphs start (third person and first person),
// after Open Animation Replacer / DAR have picked their replacement, and writes them to a log file
// for the play session. Optionally shows each new animation in the notification area (top left).
// The on-screen part can be switched on and off in the F1 menu (SKSE Menu Framework), section
// "Animation Watch". Logging to file stays on unless switched off there too.
//
// How: hkbClipGenerator vtable slots 4 (Activate) and 0x1B (StartEcho = loop restart) are wrapped at
// kDataLoaded, i.e. after OAR has installed its own hooks on the same slots, so this plugin sees the
// final choice. The clip's animationBindingIndex points into hkbCharacterStringData::animationNames,
// which holds the real .hkx path: vanilla/behavior paths and every OAR/DAR replacement path (OAR adds
// its replacements to that list). Synchronized clips (kill moves, paired animations) are logged too
// (BSSynchronizedClipGenerator vtable slot 4).
//
// 1.1.0: flip-flop warning. When the same animation slot (graph + clip, e.g. DefaultMale MT_RunForward)
// switches to a different mod's file more than twice within 2 seconds (e.g. CATA run <-> EVG CLAMBER slope
// run on bumpy ground), a WARNING line is written to the session log and shown on screen. That pattern
// restarts the animation mid-step and looks like a stutter or limp.
//
// 1.8.0: the viewing room is its own cell in AnimationWatch.esp (ESL, made by tools\make_room.py); CTest is the fallback.
// 1.7.0: the clone plays in a private viewing room (cell CTest); the player goes there and comes back (Clone.cpp).
// 1.6.0: a clone of the player (Clone.cpp) plays the animations instead of the player; playback controls (1.5.1).
// 1.5.0: animation player (Player.cpp): on the dolls page, left click an animation and the player plays it (the file
// decoded by the game's own Havok code, 3 times); right click opens the competing packs, which each have a Play button.
// 1.4.0: the dolls page shows pictures (Data\Interface\AnimationWatch\*.png); click one to see its animations around it,
// each pointing at the picture and naming its pack. Old DAR folders can be renamed for the player ("Rename it for me").
// 1.3.0: animation dolls (Dolls.cpp): F1 > Animation Watch > Animation Dolls shows, per activity and gear, which
// animation pack wins each animation, conflicts in yellow, flip-flops in red, and lets the player pick the winner.
// 1.2.0: fixer catalog. When a flip-flop warning fires, every OAR submod involved whose settings say
// "interruptible" is offered as a fix in the F1 menu (Animation Watch > Fixes): Apply writes a user.json next to
// that submod's config.json (OAR's own per-user override file, the same file OAR's User mode writes): a copy of
// the submod's settings with "interruptible" set to false, so the animation can no longer be cut off mid-step.
// Undo removes it again (or restores a user.json that was there before). OAR reads user.json when the game
// starts, so a fix takes effect after a restart. Fixer #1: flip-flop between submods -> interruptible off.
// A new suggestion pops up the game's message box (stays until OK), once per submod per session.
//
// Files:
//   Log:  Documents\My Games\Skyrim Special Edition\SKSE\AnimationWatch\Session_<date>_<time>.log
//   INI:  Data\SKSE\Plugins\AnimationWatch.ini  (bShowOnScreen, bLogToFile; saved from the F1 menu)

#include <Windows.h>

#include <deque>
#include <filesystem>
#include <regex>
#include <map>
#include <set>
#include <shared_mutex>

#include "SKSEMenuFramework.h"
#include "Dolls.h"
#include "Player.h"
#include "Clone.h"
#include "FreeCam.h"

SKSEPluginInfo(
	.Version = REL::Version{ 1, 8, 6, 0 },
	.Name = "AnimationWatch"sv,
	.Author = "tonycubed2"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace
{
	constexpr auto kIniPath = "Data\\SKSE\\Plugins\\AnimationWatch.ini";

	std::atomic<bool> g_showOnScreen{ true };
	std::atomic<bool> g_logToFile{ true };
	std::atomic<bool> g_showWarnings{ true };
	std::atomic<std::uint32_t> g_flipFlops{ 0 };

	// ---------------------------------------------------------------- settings
	void LoadSettings()
	{
		char buf[16]{};
		GetPrivateProfileStringA("General", "bShowOnScreen", "1", buf, sizeof(buf), kIniPath);
		g_showOnScreen = buf[0] != '0';
		GetPrivateProfileStringA("General", "bLogToFile", "1", buf, sizeof(buf), kIniPath);
		g_logToFile = buf[0] != '0';
		GetPrivateProfileStringA("General", "bShowWarningsOnScreen", "1", buf, sizeof(buf), kIniPath);
		g_showWarnings = buf[0] != '0';
	}

	void SaveSettings()
	{
		WritePrivateProfileStringA("General", "bShowOnScreen", g_showOnScreen ? "1" : "0", kIniPath);
		WritePrivateProfileStringA("General", "bLogToFile", g_logToFile ? "1" : "0", kIniPath);
		WritePrivateProfileStringA("General", "bShowWarningsOnScreen", g_showWarnings ? "1" : "0", kIniPath);
	}

	// ---------------------------------------------------------------- session log
	std::mutex    g_fileLock;
	std::ofstream g_file;
	const auto    g_start = std::chrono::steady_clock::now();

	std::string WallClock()
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);
		return std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	}

	void OpenSessionFile()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		*dir /= "AnimationWatch";
		std::error_code ec;
		std::filesystem::create_directories(*dir, ec);
		SYSTEMTIME t{};
		GetLocalTime(&t);
		const auto name = std::format("Session_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		g_file.open(*dir / name, std::ios::out | std::ios::trunc);
		if (g_file) {
			g_file << "AnimationWatch 1.8.9 - animations started on the player (after OAR/DAR replacement)\n";
			g_file << "time | graph | event | clip (behavior node) | animation file\n";
			g_file.flush();
			logger::info("Session log: {}", (*dir / name).string());
		}
	}

	void WriteLine(const std::string& a_line)
	{
		if (!g_logToFile) {
			return;
		}
		std::scoped_lock l{ g_fileLock };
		if (g_file) {
			g_file << a_line << '\n';
			g_file.flush();  // keep the last lines even if the game crashes
		}
	}

	// ---------------------------------------------------------------- naming
	// "Actors\Character\Animations\OpenAnimationReplacer\EVGCI\Player Injured\male\_variants_mt_runforward\inj2.hkx"
	//   -> "inj2.hkx  [OAR: EVGCI / Player Injured]"
	std::string ShortName(std::string_view a_path)
	{
		std::string lower(a_path);
		std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::string_view file = a_path;
		if (const auto slash = file.find_last_of("\\/"); slash != std::string_view::npos) {
			file.remove_prefix(slash + 1);
		}
		auto parts = [&](std::size_t a_after) {
			std::vector<std::string_view> out;
			std::string_view rest = a_path.substr(a_after);
			while (!rest.empty() && out.size() < 2) {
				const auto s = rest.find_first_of("\\/");
				out.push_back(rest.substr(0, s));
				if (s == std::string_view::npos) {
					break;
				}
				rest.remove_prefix(s + 1);
			}
			return out;
		};
		constexpr std::string_view oar = "openanimationreplacer\\";
		constexpr std::string_view dar = "dynamicanimationreplacer\\";
		if (const auto p = lower.find(oar); p != std::string::npos) {
			const auto pr = parts(p + oar.size());
			if (pr.size() == 2) {
				return std::format("{}  [OAR: {} / {}]", file, pr[0], pr[1]);
			}
			return std::format("{}  [OAR]", file);
		}
		if (const auto p = lower.find(dar); p != std::string::npos) {
			const auto pr = parts(p + dar.size());
			if (pr.size() == 2) {
				return std::format("{}  [DAR: {}]", file, pr[1]);
			}
			return std::format("{}  [DAR]", file);
		}
		return std::string(file);
	}

	// ---------------------------------------------------------------- on-screen notification (main thread only)
	// Debug.Notification (AE id 52933; not in this CommonLibSSE-NG copy)
	void ShowNotification(const char* a_text)
	{
		using func_t = void (*)(const char*, const char*, bool);
		static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };  // SE 52050 / AE 52933
		func(a_text, nullptr, false);
	}

	std::mutex  g_notifyLock;
	std::string g_lastShown;
	std::string g_current;  // shown in the F1 menu
	double      g_lastShownTime = -10.0;

	double Now()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
	}

	void Notify(const std::string& a_text)
	{
		{
			std::scoped_lock l{ g_notifyLock };
			g_current = a_text;
			if (!g_showOnScreen || a_text == g_lastShown) {
				return;
			}
			const double now = Now();
			if (now - g_lastShownTime < 0.25) {
				return;  // blends between run directions can start several clips in a few frames; the log keeps them all
			}
			g_lastShown = a_text;
			g_lastShownTime = now;
		}
		if (const auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([text = "Anim: " + a_text]() { ShowNotification(text.c_str()); });
		}
	}

	// ---------------------------------------------------------------- fixer catalog (1.2.0)
	constexpr auto kFixMarker = "animationWatchFixer";

	struct FixSuggestion
	{
		std::string fixer;    // which fixer made it
		std::string source;   // "OAR: <mod> / <submod>"
		std::string slot;     // clip that flip-flopped
		std::string dir;      // submod folder (relative to the game folder, through MO2's virtual Data)
		std::string status;   // shown in the F1 menu
	};

	std::mutex                 g_fixLock;
	std::vector<FixSuggestion> g_fixes;

	std::string ReadText(const std::filesystem::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		if (!in) {
			return {};
		}
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	bool WriteText(const std::filesystem::path& a_path, const std::string& a_text)
	{
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false;
		}
		out << a_text;
		return static_cast<bool>(out);
	}

	// "data\meshes\...\OpenAnimationReplacer\EVGCI\Player Injured\male\x.hkx" -> "data\meshes\...\OpenAnimationReplacer\EVGCI\Player Injured"
	std::string SubmodDir(std::string_view a_path)
	{
		std::string lower(a_path);
		std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		constexpr std::string_view oar = "openanimationreplacer\\";
		const auto p = lower.find(oar);
		if (p == std::string::npos) {
			return {};
		}
		auto pos = p + oar.size();
		for (int part = 0; part < 2; ++part) {
			const auto s = a_path.find_first_of("\\/", pos);
			if (s == std::string_view::npos) {
				return {};
			}
			pos = s + 1;
		}
		return std::string(a_path.substr(0, pos - 1));
	}

	const std::regex kInterruptibleTrue{ R"rx("interruptible"\s*:\s*true)rx" };
	const std::regex kInterruptibleAny{ R"rx("interruptible"\s*:\s*(true|false))rx" };

	// settings OAR uses for this submod: user.json if present, else config.json
	std::string ActiveSettings(const std::filesystem::path& a_dir)
	{
		auto text = ReadText(a_dir / "user.json");
		return text.empty() ? ReadText(a_dir / "config.json") : text;
	}

	bool IsOurFix(const std::filesystem::path& a_dir)
	{
		return ReadText(a_dir / "user.json").find(kFixMarker) != std::string::npos;
	}

	std::string ApplyInterruptibleFix(const std::string& a_dir)
	{
		const std::filesystem::path dir(a_dir);
		const auto user = dir / "user.json";
		const auto text = ActiveSettings(dir);
		if (text.empty()) {
			return "could not read the submod's settings";
		}
		std::string fixed = std::regex_search(text, kInterruptibleAny) ? std::regex_replace(text, kInterruptibleAny, R"("interruptible": false)") : text;
		const auto brace = fixed.find('{');
		if (brace == std::string::npos) {
			return "settings file has no JSON object";
		}
		std::string insert = std::format("\n    \"{}\": \"AnimationWatch flip-flop fix: interruptible set to false; Undo in the F1 menu removes it\",", kFixMarker);
		if (!std::regex_search(text, kInterruptibleAny)) {
			insert += "\n    \"interruptible\": false,";
		}
		fixed.insert(brace + 1, insert);
		std::error_code ec;
		if (std::filesystem::exists(user, ec) && !IsOurFix(dir)) {
			std::filesystem::copy_file(user, dir / "user.json.animationwatch.bak", std::filesystem::copy_options::overwrite_existing, ec);
		}
		if (!WriteText(user, fixed)) {
			return "could not write user.json";
		}
		logger::info("Fix applied: {}\\user.json (interruptible false)", a_dir);
		WriteLine(std::format("{} | - | FIX APPLIED | interruptible off | {}\\user.json", WallClock(), a_dir));
		return "applied - takes effect after the game restarts";
	}

	std::string UndoInterruptibleFix(const std::string& a_dir)
	{
		const std::filesystem::path dir(a_dir);
		if (!IsOurFix(dir)) {
			return "nothing to undo (no AnimationWatch user.json here)";
		}
		std::error_code ec;
		const auto bak = dir / "user.json.animationwatch.bak";
		if (std::filesystem::exists(bak, ec)) {
			std::filesystem::copy_file(bak, dir / "user.json", std::filesystem::copy_options::overwrite_existing, ec);
			std::filesystem::remove(bak, ec);
		} else {
			std::filesystem::remove(dir / "user.json", ec);
		}
		logger::info("Fix undone: {}", a_dir);
		WriteLine(std::format("{} | - | FIX UNDONE | interruptible restored | {}", WallClock(), a_dir));
		return ec ? std::format("undo failed: {}", ec.message()) : "undone - takes effect after the game restarts";
	}

	// Fixer #1: called when a flip-flop warning fires. a_sources: source -> sample file path.
	void SuggestFlipFlopFixes(const std::string& a_slot, const std::map<std::string, std::string>& a_sources)
	{
		for (const auto& [source, path] : a_sources) {
			if (source.rfind("OAR:", 0) != 0) {
				continue;
			}
			const auto dir = SubmodDir(path);
			if (dir.empty()) {
				continue;
			}
			const bool ours = IsOurFix(dir);
			const bool interruptible = std::regex_search(ActiveSettings(dir), kInterruptibleTrue);
			if (!interruptible && !ours) {
				continue;  // already not interruptible: the switches came from fresh starts, not mid-step cuts
			}
			std::scoped_lock l{ g_fixLock };
			if (std::ranges::any_of(g_fixes, [&](const FixSuggestion& f) { return f.dir == dir; })) {
				continue;
			}
			g_fixes.push_back({ "flip-flop: interruptible off", source, a_slot, dir, ours ? "applied earlier (active)" : "suggested" });
			WriteLine(std::format("{} | - | FIX SUGGESTED | {} is interruptible and flip-flops on {} | {}", WallClock(), source, a_slot, dir));
			if (!ours) {
				if (const auto tasks = SKSE::GetTaskInterface()) {
					auto text = std::format("AnimationWatch - FIX AVAILABLE\n\n{}\ncan be cut off mid-step and keeps swapping with other animations on {}.\n\nOpen F1 > Animation Watch > Fixes and press Apply, then restart the game.", source, a_slot);
					tasks->AddTask([text = std::move(text)]() { RE::DebugMessageBox(text.c_str()); });
				}
			}
		}
	}

	// ---------------------------------------------------------------- flip-flop detection
	// Which mod a file comes from: "OAR: <mod> / <submod>", "DAR: <folder>" or "vanilla/behavior".
	std::string SourceOf(std::string_view a_path)
	{
		const auto name = ShortName(a_path);
		const auto open = name.find("  [");
		if (open == std::string::npos) {
			return "vanilla/behavior";
		}
		auto src = name.substr(open + 3);
		if (!src.empty() && src.back() == ']') {
			src.pop_back();
		}
		return src;
	}

	struct SlotHistory
	{
		std::string        lastSource;
		std::deque<double> switches;   // times the source changed, last 2 s
		std::set<std::string> sources; // sources seen in the current burst
		std::map<std::string, std::string> sourcePath;  // a file path per source (to find its OAR submod folder)
		double             lastWarned = -100.0;
	};

	std::mutex                                   g_slotLock;
	std::unordered_map<std::string, SlotHistory> g_slots;  // key: graph|clip

	void ShowWarning(const std::string& a_text)
	{
		if (!g_showWarnings) {
			return;
		}
		if (const auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([text = a_text]() { ShowNotification(text.c_str()); });
		}
	}

	void CheckFlipFlop(const std::string& a_graph, const char* a_clip, const std::string& a_path)
	{
		constexpr double kWindow = 2.0;      // seconds
		constexpr std::size_t kSwitches = 3; // "more than twice"
		constexpr double kQuiet = 10.0;      // warn a slot at most every 10 s

		const auto source = SourceOf(a_path);
		Dolls::OnPlayed(a_graph, a_path, source);
		const std::string key = a_graph + "|" + (a_clip ? a_clip : "");
		const double now = Now();
		std::string warning;
		std::map<std::string, std::string> burst;
		{
			std::scoped_lock l{ g_slotLock };
			auto& h = g_slots[key];
			h.sourcePath[source] = a_path;
			if (h.lastSource.empty()) {
				h.lastSource = source;
				return;
			}
			if (source == h.lastSource) {
				return;
			}
			h.switches.push_back(now);
			while (!h.switches.empty() && now - h.switches.front() > kWindow) {
				h.switches.pop_front();
			}
			if (h.switches.size() == 1) {
				h.sources.clear();
			}
			h.sources.insert(h.lastSource);
			h.sources.insert(source);
			h.sourcePath[source] = a_path;
			h.lastSource = source;
			if (h.switches.size() >= kSwitches && now - h.lastWarned >= kQuiet) {
				h.lastWarned = now;
				std::string list;
				for (const auto& s : h.sources) {
					list += (list.empty() ? "" : " <-> ") + s;
				}
				warning = std::format("{} switched {} times in {:.1f} s: {}", a_clip ? a_clip : "", h.switches.size(), now - h.switches.front(), list);
				for (const auto& s : h.sources) {
					if (const auto it = h.sourcePath.find(s); it != h.sourcePath.end()) {
						burst[s] = it->second;
					}
				}
			}
		}
		if (!warning.empty()) {
			++g_flipFlops;
			for (const auto& [src, path] : burst) {
				Dolls::MarkFlipFlop(a_graph, path);
			}
			WriteLine(std::format("{} | {} | WARNING flip-flop | {}", WallClock(), a_graph, warning));
			logger::warn("Flip-flop: {} {}", a_graph, warning);
			ShowWarning("Anim flip-flop: " + warning);
			SuggestFlipFlopFixes(a_clip ? a_clip : "", burst);
		}
	}

	// ---------------------------------------------------------------- player clip tracking
	struct ClipInfo
	{
		std::string                  graph;       // project name, e.g. DefaultMale / FirstPerson
		RE::hkbCharacterStringData*  stringData = nullptr;
		std::uint16_t                lastIndex = 0xFFFF;
	};

	std::shared_mutex                                   g_clipLock;
	std::unordered_map<const RE::hkbClipGenerator*, ClipInfo> g_playerClips;

	RE::BShkbAnimationGraph* GraphOf(const RE::hkbCharacter* a_character)
	{
		return a_character ? SKSE::stl::adjust_pointer<RE::BShkbAnimationGraph>(const_cast<RE::hkbCharacter*>(a_character), -0xC0) : nullptr;
	}

	bool IsPlayerGraph(const RE::BShkbAnimationGraph* a_graph)
	{
		static const auto player = RE::PlayerCharacter::GetSingleton();
		return a_graph && player && a_graph->holder == player;
	}

	RE::hkbCharacterStringData* StringDataOf(const RE::hkbCharacter* a_character)
	{
		if (a_character && a_character->setup && a_character->setup->data) {
			return a_character->setup->data->stringData.get();
		}
		return nullptr;
	}

	std::string AnimPath(RE::hkbCharacterStringData* a_stringData, std::uint16_t a_index)
	{
		if (a_stringData && a_index < a_stringData->animationNames.size()) {
			const char* s = a_stringData->animationNames[a_index].data();
			return s ? s : "";
		}
		return std::format("(binding index {} not in the animation list)", a_index);
	}

	void Report(const ClipInfo& a_info, const char* a_event, const RE::hkbClipGenerator* a_clip, std::uint16_t a_index)
	{
		const char* clipName = a_clip->name.data();
		const auto  path = AnimPath(a_info.stringData, a_index);
		WriteLine(std::format("{} | {} | {} | {} | {}", WallClock(), a_info.graph, a_event, clipName ? clipName : "", path));
		Notify(ShortName(path));
		CheckFlipFlop(a_info.graph, clipName, path);
	}

	// ---------------------------------------------------------------- hooks
	using ClipActivate_t = void (*)(RE::hkbClipGenerator*, const RE::hkbContext&);
	using ClipEcho_t = void (*)(RE::hkbClipGenerator*, float);
	using ClipDeactivate_t = void (*)(RE::hkbClipGenerator*, const RE::hkbContext&);
	using SyncActivate_t = void (*)(RE::BSSynchronizedClipGenerator*, const RE::hkbContext&);

	ClipActivate_t   g_clipActivate = nullptr;
	ClipEcho_t       g_clipEcho = nullptr;
	ClipDeactivate_t g_clipDeactivate = nullptr;
	SyncActivate_t   g_syncActivate = nullptr;

	void ClipActivateHook(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context)
	{
		g_clipActivate(a_this, a_context);  // OAR (if installed earlier) picks the replacement inside this call

		const auto graph = GraphOf(a_context.character);
		if (!IsPlayerGraph(graph)) {
			return;
		}
		ClipInfo info;
		const char* project = graph->projectName.c_str();
		info.graph = project ? project : "";
		info.stringData = StringDataOf(a_context.character);
		info.lastIndex = a_this->animationBindingIndex;
		{
			std::unique_lock l{ g_clipLock };
			g_playerClips[a_this] = info;
		}
		Report(info, "start", a_this, a_this->animationBindingIndex);
	}

	void ClipEchoHook(RE::hkbClipGenerator* a_this, float a_echoDuration)
	{
		g_clipEcho(a_this, a_echoDuration);  // a loop restart; OAR may switch to another variant here

		ClipInfo info;
		{
			std::shared_lock l{ g_clipLock };
			const auto it = g_playerClips.find(a_this);
			if (it == g_playerClips.end()) {
				return;
			}
			info = it->second;
		}
		const auto index = a_this->animationBindingIndex;
		const char* event = index == info.lastIndex ? "loop" : "loop, switched file";
		{
			std::unique_lock l{ g_clipLock };
			if (const auto it = g_playerClips.find(a_this); it != g_playerClips.end()) {
				it->second.lastIndex = index;
			}
		}
		const char* clipName = a_this->name.data();
		const auto  path = AnimPath(info.stringData, index);
		WriteLine(std::format("{} | {} | {} | {} | {}", WallClock(), info.graph, event, clipName ? clipName : "", path));
		if (index != info.lastIndex) {
			Notify(ShortName(path));
			CheckFlipFlop(info.graph, clipName, path);
		}
	}

	void ClipDeactivateHook(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context)
	{
		{
			std::shared_lock l{ g_clipLock };
			if (!g_playerClips.contains(a_this)) {
				l.unlock();
				g_clipDeactivate(a_this, a_context);
				return;
			}
		}
		{
			std::unique_lock l{ g_clipLock };
			g_playerClips.erase(a_this);
		}
		g_clipDeactivate(a_this, a_context);
	}

	void SyncActivateHook(RE::BSSynchronizedClipGenerator* a_this, const RE::hkbContext& a_context)
	{
		g_syncActivate(a_this, a_context);

		const auto graph = GraphOf(a_context.character);
		if (!IsPlayerGraph(graph)) {
			return;
		}
		const char* project = graph->projectName.c_str();
		const char* syncName = a_this->name.data();
		std::string inner;
		if (const auto clip = a_this->clipGenerator) {
			const char* clipName = clip->name.data();
			inner = std::format("{} | {}", clipName ? clipName : "", AnimPath(StringDataOf(a_context.character), clip->animationBindingIndex));
		}
		WriteLine(std::format("{} | {} | paired start ({}) | {}", WallClock(), project ? project : "", syncName ? syncName : "", inner));
	}

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> clipVtbl{ RE::VTABLE_hkbClipGenerator[0] };
		g_clipActivate = reinterpret_cast<ClipActivate_t>(clipVtbl.write_vfunc(0x4, &ClipActivateHook));
		g_clipDeactivate = reinterpret_cast<ClipDeactivate_t>(clipVtbl.write_vfunc(0x7, &ClipDeactivateHook));
		g_clipEcho = reinterpret_cast<ClipEcho_t>(clipVtbl.write_vfunc(0x1B, &ClipEchoHook));

		REL::Relocation<std::uintptr_t> syncVtbl{ RE::VTABLE_BSSynchronizedClipGenerator[0] };
		g_syncActivate = reinterpret_cast<SyncActivate_t>(syncVtbl.write_vfunc(0x4, &SyncActivateHook));
		logger::info("Hooks installed (hkbClipGenerator 4/7/0x1B, BSSynchronizedClipGenerator 4)");
	}

	// ---------------------------------------------------------------- F1 menu
	void __stdcall RenderMenu()
	{
		bool show = g_showOnScreen;
		if (ImGuiMCP::Checkbox("Show animation names on screen", &show)) {
			g_showOnScreen = show;
			SaveSettings();
		}
		bool log = g_logToFile;
		if (ImGuiMCP::Checkbox("Write session log file", &log)) {
			g_logToFile = log;
			SaveSettings();
		}
		bool warn = g_showWarnings;
		if (ImGuiMCP::Checkbox("Show flip-flop warnings on screen", &warn)) {
			g_showWarnings = warn;
			SaveSettings();
		}
		ImGuiMCP::TextUnformatted(std::format("Flip-flop warnings this session: {}", g_flipFlops.load()).c_str());
		std::string current;
		{
			std::scoped_lock l{ g_notifyLock };
			current = g_current;
		}
		ImGuiMCP::TextUnformatted(("Last animation: " + (current.empty() ? std::string("-") : current)).c_str());
		ImGuiMCP::TextUnformatted("Log folder: Documents\\My Games\\Skyrim Special Edition\\SKSE\\AnimationWatch");
		ImGuiMCP::Separator();
		ImGuiMCP::TextUnformatted("Fixes (take effect after the game restarts):");
		std::scoped_lock l{ g_fixLock };
		if (g_fixes.empty()) {
			ImGuiMCP::TextUnformatted("  none suggested this session");
		}
		for (int i = 0; i < static_cast<int>(g_fixes.size()); ++i) {
			auto& f = g_fixes[i];
			ImGuiMCP::PushID(i);
			ImGuiMCP::TextUnformatted(std::format("{} - {} (flip-flops on {})", f.fixer, f.source, f.slot).c_str());
			ImGuiMCP::TextUnformatted(std::format("    status: {}", f.status).c_str());
			if (ImGuiMCP::Button("Apply")) {
				f.status = ApplyInterruptibleFix(f.dir);
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Undo")) {
				f.status = UndoInterruptibleFix(f.dir);
			}
			ImGuiMCP::PopID();
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
			AnimPlayer::OnGameLoad();
			FreeCam::OnGameLoad();
			Clone::OnGameLoad();
		}
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			InstallHooks();
			AnimPlayer::Install();
			FreeCam::Install();
			Clone::Install();
			Dolls::StartScan();
			if (SKSEMenuFramework::IsInstalled()) {
				SKSEMenuFramework::SetSection("Animation Watch");
				SKSEMenuFramework::AddSectionItem("Settings", RenderMenu);
				SKSEMenuFramework::AddSectionItem("Animation Dolls", Dolls::Render);
				logger::info("F1 menu section added");
			} else {
				logger::warn("SKSE Menu Framework not found; no F1 menu (INI still works)");
			}
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	if (auto log = spdlog::default_logger()) {
		log->flush_on(spdlog::level::trace);  // 1.8.8: every line is on disk at once, no need to quit the game for the log
	}
	logger::info("AnimationWatch 1.8.9 loading; game runtime {}", REL::Module::get().version().string());
	LoadSettings();
	OpenSessionFile();
	Clone::InstallSerialization();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
