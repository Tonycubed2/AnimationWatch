// Animation dolls - AnimationWatch 1.4.0
//
// F1 > Animation Watch > Animation Dolls: a page of pictures, one per activity (Running, Walking, Jumping,
// One-handed ...), each with its name on a cream label below. Clicking a picture shows it large in the middle of the
// page with every animation of that activity around it, a line from each label to the picture:
//   left-side animations (turn left, left hand, backward ...) on the left, right-side ones on the right,
//   weapon animations (attack, block, draw, bow, cast ...) pointing at the weapon, the rest at the body.
// Each label names the animation and the pack (mod / submod) it comes from, coloured by state:
//   green  - one pack provides it for the gear shown
//   yellow - two or more packs compete for it (the one with the higher OAR priority wins when its conditions pass)
//   red    - it flip-flopped between packs this session (switching mid-step)
//   grey   - no pack replaces it (vanilla)
// The pictures are Data\Interface\AnimationWatch\*.png. "Back to all pictures" returns to the page of pictures.
// "Gear" picks the view: all gear, light armor, heavy armor, clothing. Packs whose conditions name a gear type
// (the game's ArmorLight / ArmorHeavy / ArmorClothing keywords, or the words in their folder names) only show
// on that gear's view; packs without such a condition show on every view.
//
// Clicking a label lists the competing packs. "Make winner" raises that pack's OAR priority just above the others
// for that slot, by writing OAR's own user.json for the submod (a copy of its settings with the new priority, the
// original priority kept as "animationWatchOriginalPriority"). "Make whole mod win" does that for every submod of
// the mod, keeping their order. "Undo" puts the original priority back. OAR reads user.json when the game starts.
// Old DAR folders: "Rename it for me" renames the folder to the first free number above the others (after a
// "Yes, rename it"); under Mod Organizer the real folder inside the MO2 mod is renamed.
//
// Where the data comes from: every OAR pack under Data\meshes\actors\character\animations\OpenAnimationReplacer
// (third person) and ...\_1stperson\animations\OpenAnimationReplacer (first person) is read once at startup
// (config.json, or user.json when present), plus the DAR folders under ...\DynamicAnimationReplacer\_CustomConditions.
// What really played is taken from AnimationWatch's own tracking.

#include "Dolls.h"

#include <Windows.h>

#include <climits>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>

#include "SKSEMenuFramework.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Dolls
{
	namespace
	{
		// ---------------------------------------------------------------- data
		enum Gear : std::uint8_t
		{
			kGearLight = 1 << 0,
			kGearHeavy = 1 << 1,
			kGearCloth = 1 << 2,
		};

		struct SubMod
		{
			std::string mod;        // OAR mod folder name (display name if the config has one)
			std::string source;     // Mod Organizer mod (or "game Data folder") the files really come from
			std::string modFolder;  // folder name, used to group "whole mod"
			std::string name;       // submod display name
			fs::path    dir;        // submod folder
			int         priority = 0;
			int         original = 0;  // priority before an AnimationWatch change
			bool        changed = false;
			bool        disabled = false;
			bool        legacy = false;  // DAR folder: priority is the folder number
			std::uint8_t gear = 0;       // gear it is limited to (0 = any)
			bool        otherConditions = false;
		};

		struct Slot
		{
			std::string           key;       // "3|male\\mt_runforward.hkx"
			std::string           file;      // "mt_runforward"
			bool                  firstPerson = false;
			int                   activity = 0;
			std::vector<std::size_t> candidates;  // indexes into g_subMods
		};

		struct Live
		{
			std::string source;
			double      time = 0;
		};

		std::mutex               g_lock;
		std::vector<SubMod>      g_subMods;
		std::map<std::string, Slot> g_slots;
		std::map<std::string, Live> g_live;
		std::set<std::string>    g_flipFlopped;
		std::atomic<bool>        g_scanned{ false };
		std::string              g_scanInfo = "reading animation packs...";

		const char* const kActivities[] = { "Running", "Sprinting", "Walking", "Sneaking", "Jumping", "Swimming", "One-handed", "Two-handed", "Bow",
			"Spellcasting", "Unarmed", "Horse", "Idle", "Other" };
		constexpr int kActivityCount = static_cast<int>(std::size(kActivities));

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		int ActivityOf(const std::string& a_file)  // a_file lowercase, no extension, no folder
		{
			auto has = [&](std::string_view s) { return a_file.find(s) != std::string::npos; };
			auto starts = [&](std::string_view s) { return a_file.rfind(s, 0) == 0; };
			if (has("horse")) return 11;
			if (has("sprint")) return 1;
			if (has("run")) return 0;
			if (has("walk")) return 2;
			if (has("sneak")) return 3;
			if (has("jump") || has("fall") || has("land")) return 4;
			if (has("swim")) return 5;
			if (starts("mlh_") || starts("mrh_") || starts("mag_") || has("spell") || has("magic") || has("cast") || has("ritual")) return 9;
			if (starts("bow") || has("crossbow")) return 8;
			if (starts("1hm_") || starts("1hd_") || starts("dw_") || starts("shd_") || has("block")) return 6;
			if (starts("2hm_") || starts("2hw_")) return 7;
			if (starts("h2h") || has("unarmed")) return 10;
			if (has("idle")) return 12;
			return 13;
		}

		std::string ReadText(const fs::path& a_path)
		{
			std::ifstream in(a_path, std::ios::binary);
			return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string();
		}

		bool WriteText(const fs::path& a_path, const std::string& a_text)
		{
			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			if (!out) {
				return false;
			}
			out << a_text;
			return static_cast<bool>(out);
		}

		json ParseJson(const std::string& a_text)
		{
			try {
				return json::parse(a_text, nullptr, true, true);  // OAR files may hold comments
			} catch (...) {
				return json();
			}
		}

		std::uint8_t GearOf(const std::string& a_conditionsLower, const std::string& a_namesLower, bool& a_other)
		{
			std::uint8_t gear = 0;
			// Skyrim.esm keywords: ArmorLight 06BBD3, ArmorHeavy 06BBD2, ArmorClothing 06BBE8
			if (a_conditionsLower.find("6bbd3") != std::string::npos || a_namesLower.find("light armor") != std::string::npos || a_namesLower.find("lightarmor") != std::string::npos) {
				gear |= kGearLight;
			}
			if (a_conditionsLower.find("6bbd2") != std::string::npos || a_namesLower.find("heavy armor") != std::string::npos || a_namesLower.find("heavyarmor") != std::string::npos) {
				gear |= kGearHeavy;
			}
			if (a_conditionsLower.find("6bbe8") != std::string::npos || a_namesLower.find("cloth") != std::string::npos || a_namesLower.find("unarmored") != std::string::npos ||
				a_namesLower.find("naked") != std::string::npos) {
				gear |= kGearCloth;
			}
			a_other = gear == 0 && a_conditionsLower.find("\"condition\"") != std::string::npos;
			return gear;
		}

		// every .hkx under a submod folder, as the path OAR maps it to (relative, lowercase); _variants_X folders count as X.hkx
		void CollectFiles(const fs::path& a_root, std::set<std::string>& a_out)
		{
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(a_root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
				const auto& p = it->path();
				const auto name = Lower(p.filename().string());
				if (it->is_directory(ec)) {
					if (name.rfind("_variants_", 0) == 0) {
						auto rel = Lower(fs::relative(p.parent_path(), a_root, ec).string());
						rel = (rel == "." || rel.empty()) ? std::string() : rel + "\\";
						a_out.insert(rel + name.substr(10) + ".hkx");
						it.disable_recursion_pending();
					}
					continue;
				}
				if (p.extension().string().size() == 4 && Lower(p.extension().string()) == ".hkx") {
					a_out.insert(Lower(fs::relative(p, a_root, ec).string()));
				}
			}
		}

		void AddSlotFiles(std::size_t a_index, const std::set<std::string>& a_files, bool a_firstPerson, std::map<std::string, Slot>& a_slots)
		{
			for (const auto& rel : a_files) {
				const auto key = std::string(a_firstPerson ? "1|" : "3|") + rel;
				auto&      slot = a_slots[key];
				if (slot.key.empty()) {
					slot.key = key;
					slot.firstPerson = a_firstPerson;
					auto file = fs::path(rel).stem().string();
					slot.file = file;
					slot.activity = ActivityOf(file);
				}
				slot.candidates.push_back(a_index);
			}
		}

		// ---------------------------------------------------------------- renaming an old DAR folder
		// Under Mod Organizer the game sees the folder through MO2's virtual Data folder: the rename is done on the
		// real folder (in the MO2 mod), found from an open handle.
		fs::path RealPathOf(const fs::path& a_path)
		{
			const HANDLE h = CreateFileW(a_path.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return fs::absolute(a_path);
			}
			std::wstring buf(32768, L'\0');
			const DWORD  n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
			CloseHandle(h);
			if (n == 0 || n >= buf.size()) {
				return fs::absolute(a_path);
			}
			buf.resize(n);
			if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0) {
				buf = L"\\\\" + buf.substr(8);
			} else if (buf.rfind(L"\\\\?\\", 0) == 0) {
				buf = buf.substr(4);
			}
			return fs::path(buf);
		}

		// the Mod Organizer mod a folder really comes from: the folder before "meshes" in its real path
		std::string SourceModOf(const fs::path& a_dir)
		{
			const auto real = RealPathOf(a_dir);
			std::vector<std::string> parts;
			for (const auto& part : real) {
				parts.push_back(part.string());
			}
			for (std::size_t i = parts.size(); i-- > 1;) {
				if (Lower(parts[i]) == "meshes") {
					return Lower(parts[i - 1]) == "data" ? std::string("game Data folder") : parts[i - 1];
				}
			}
			return {};
		}

		void ScanRoot(const fs::path& a_animations, bool a_firstPerson, std::vector<SubMod>& a_subs, std::map<std::string, Slot>& a_slots)
		{
			std::error_code ec;
			const auto oar = a_animations / "OpenAnimationReplacer";
			if (fs::is_directory(oar, ec)) {
				for (const auto& modEntry : fs::directory_iterator(oar, ec)) {
					if (!modEntry.is_directory(ec)) {
						continue;
					}
					const auto modFolder = modEntry.path().filename().string();
					std::string modName = modFolder;
					if (auto mj = ParseJson(ReadText(modEntry.path() / "config.json")); mj.is_object() && mj.contains("name") && mj["name"].is_string()) {
						modName = mj["name"].get<std::string>();
					}
					for (const auto& subEntry : fs::directory_iterator(modEntry.path(), ec)) {
						if (!subEntry.is_directory(ec)) {
							continue;
						}
						const auto dir = subEntry.path();
						auto       text = ReadText(dir / "user.json");
						const auto config = ReadText(dir / "config.json");
						if (text.empty()) {
							text = config;
						}
						if (text.empty()) {
							continue;  // not a submod
						}
						SubMod s;
						s.mod = modName;
						s.modFolder = modFolder;
						s.name = dir.filename().string();
						s.dir = dir;
						s.source = SourceModOf(dir);
						const auto j = ParseJson(text);
						if (const auto cj = ParseJson(config); cj.is_object() && cj.contains("name") && cj["name"].is_string()) {
							s.name = cj["name"].get<std::string>();
						}
						if (j.is_object()) {
							s.priority = j.value("priority", 0);
							s.disabled = j.value("disabled", false);
							if (j.contains("animationWatchOriginalPriority") && j["animationWatchOriginalPriority"].is_number_integer()) {
								s.changed = true;
								s.original = j["animationWatchOriginalPriority"].get<int>();
							}
						}
						s.original = s.changed ? s.original : s.priority;
						const auto conditions = j.is_object() && j.contains("conditions") ? Lower(j["conditions"].dump()) : std::string();
						s.gear = GearOf(conditions, Lower(modFolder + " " + modName + " " + s.name + " " + dir.filename().string()), s.otherConditions);
						std::set<std::string> files;
						CollectFiles(dir, files);
						if (files.empty()) {
							continue;
						}
						a_subs.push_back(std::move(s));
						AddSlotFiles(a_subs.size() - 1, files, a_firstPerson, a_slots);
					}
				}
			}
			// old DAR folders: _CustomConditions\<priority>\...
			const auto dar = a_animations / "DynamicAnimationReplacer" / "_CustomConditions";
			if (fs::is_directory(dar, ec)) {
				for (const auto& entry : fs::directory_iterator(dar, ec)) {
					if (!entry.is_directory(ec)) {
						continue;
					}
					const auto folder = entry.path().filename().string();
					char*      end = nullptr;
					const long prio = std::strtol(folder.c_str(), &end, 10);
					if (!end || *end != '\0') {
						continue;
					}
					SubMod s;
					s.mod = "DAR";
					s.modFolder = "DAR " + folder;
					s.name = "folder " + folder;
					s.dir = entry.path();
					s.source = SourceModOf(entry.path());
					s.priority = static_cast<int>(prio);
					s.original = s.priority;
					s.legacy = true;
					s.gear = GearOf(Lower(ReadText(entry.path() / "_conditions.txt")), "", s.otherConditions);
					s.otherConditions = s.gear == 0 && fs::exists(entry.path() / "_conditions.txt", ec);
					std::set<std::string> files;
					CollectFiles(entry.path(), files);
					if (files.empty()) {
						continue;
					}
					a_subs.push_back(std::move(s));
					AddSlotFiles(a_subs.size() - 1, files, a_firstPerson, a_slots);
				}
			}
		}

		void Scan()
		{
			std::vector<SubMod>         subs;
			std::map<std::string, Slot> slots;
			ScanRoot(fs::path("data") / "meshes" / "actors" / "character" / "animations", false, subs, slots);
			ScanRoot(fs::path("data") / "meshes" / "actors" / "character" / "_1stperson" / "animations", true, subs, slots);
			const auto info = std::format("{} animation packs (submods) replace {} animations", subs.size(), slots.size());
			logger::info("Animation dolls: {}", info);
			std::scoped_lock l{ g_lock };
			g_subMods = std::move(subs);
			g_slots = std::move(slots);
			g_scanInfo = info;
			g_scanned = true;
		}

		// "data\meshes\actors\character\animations\OpenAnimationReplacer\CATA\Light Armor\male\mt_runforward.hkx" -> "3|male\mt_runforward.hkx"
		std::string KeyOfPath(const std::string& a_graph, const std::string& a_path)
		{
			auto p = Lower(a_path);
			std::ranges::replace(p, '/', '\\');
			std::string rel;
			auto skip = [&](std::size_t a_pos, int a_parts) -> std::string {
				for (int i = 0; i < a_parts; ++i) {
					const auto s = p.find('\\', a_pos);
					if (s == std::string::npos) {
						return {};
					}
					a_pos = s + 1;
				}
				return p.substr(a_pos);
			};
			if (const auto o = p.find("openanimationreplacer\\"); o != std::string::npos) {
				rel = skip(o + 22, 2);
			} else if (const auto d = p.find("_customconditions\\"); d != std::string::npos) {
				rel = skip(d + 18, 1);
			} else if (const auto a = p.rfind("animations\\"); a != std::string::npos) {
				rel = p.substr(a + 11);
			} else {
				rel = p;
			}
			// a variant file: "...\_variants_mt_runforward\v1.hkx" -> "...\mt_runforward.hkx"
			if (const auto v = rel.find("_variants_"); v != std::string::npos) {
				const auto slash = rel.find('\\', v);
				rel = rel.substr(0, v) + rel.substr(v + 10, slash == std::string::npos ? std::string::npos : slash - v - 10) + ".hkx";
			}
			return std::string(a_graph == "FirstPerson" ? "1|" : "3|") + rel;
		}

		// ---------------------------------------------------------------- picking a winner
		std::string SetPriority(SubMod& a_sub, int a_priority)
		{
			const auto user = a_sub.dir / "user.json";
			auto       text = ReadText(user);
			const bool hadUser = !text.empty();
			if (!hadUser) {
				text = ReadText(a_sub.dir / "config.json");
			}
			auto j = ParseJson(text);
			if (!j.is_object()) {
				return std::format("could not read the settings of {} / {}", a_sub.mod, a_sub.name);
			}
			std::error_code ec;
			if (hadUser && !j.contains("animationWatchOriginalPriority") && !fs::exists(a_sub.dir / "user.json.animationwatch.bak", ec)) {
				fs::copy_file(user, a_sub.dir / "user.json.animationwatch.bak", fs::copy_options::overwrite_existing, ec);
			}
			if (!j.contains("animationWatchOriginalPriority")) {
				j["animationWatchOriginalPriority"] = j.value("priority", 0);
			}
			j["priority"] = a_priority;
			if (!WriteText(user, j.dump(4))) {
				return std::format("could not write {}", user.string());
			}
			a_sub.original = j["animationWatchOriginalPriority"].get<int>();
			a_sub.priority = a_priority;
			a_sub.changed = true;
			logger::info("Dolls: {} / {} priority {} -> {} ({})", a_sub.mod, a_sub.name, a_sub.original, a_priority, user.string());
			return {};
		}

		std::string UndoPriority(SubMod& a_sub)
		{
			const auto user = a_sub.dir / "user.json";
			auto       j = ParseJson(ReadText(user));
			if (!j.is_object() || !j.contains("animationWatchOriginalPriority")) {
				return "nothing to undo";
			}
			const int original = j["animationWatchOriginalPriority"].get<int>();
			j.erase("animationWatchOriginalPriority");
			j["priority"] = original;
			std::error_code ec;
			const auto      bak = a_sub.dir / "user.json.animationwatch.bak";
			if (fs::exists(bak, ec)) {
				// there was a user.json of its own before: put it back, with the original priority
				auto b = ParseJson(ReadText(bak));
				if (b.is_object()) {
					b["priority"] = original;
					WriteText(user, b.dump(4));
				}
				fs::remove(bak, ec);
			} else if (j.contains("animationWatchFixer")) {
				WriteText(user, j.dump(4));  // the flip-flop fixer's change stays
			} else {
				fs::remove(user, ec);  // AnimationWatch made this user.json only for the priority
			}
			a_sub.priority = original;
			a_sub.original = original;
			a_sub.changed = false;
			logger::info("Dolls: {} / {} priority back to {}", a_sub.mod, a_sub.name, original);
			return {};
		}

		// ---------------------------------------------------------------- page state
		int         g_gear = 0;  // 0 all, else Gear bit
		bool        g_firstPerson = false;
		std::string g_popupSlot;
		std::string g_result;

		bool Applies(const SubMod& a_sub)
		{
			return !a_sub.disabled && (g_gear == 0 || a_sub.gear == 0 || (a_sub.gear & g_gear) != 0);
		}

		std::vector<std::size_t> Ranked(const Slot& a_slot)
		{
			std::vector<std::size_t> out;
			for (const auto i : a_slot.candidates) {
				if (Applies(g_subMods[i])) {
					out.push_back(i);
				}
			}
			std::ranges::stable_sort(out, [](std::size_t a, std::size_t b) { return g_subMods[a].priority > g_subMods[b].priority; });
			return out;
		}

		enum class State
		{
			kVanilla,
			kOne,
			kConflict,
			kError
		};

		State StateOf(const Slot& a_slot, const std::vector<std::size_t>& a_ranked)
		{
			if (g_flipFlopped.contains(a_slot.key)) {
				return State::kError;
			}
			if (a_ranked.empty()) {
				return State::kVanilla;
			}
			std::set<std::string> mods;
			for (const auto i : a_ranked) {
				mods.insert(g_subMods[i].modFolder);
			}
			return mods.size() > 1 ? State::kConflict : State::kOne;
		}

		ImGuiMCP::ImU32 ColorOf(State a_state)
		{
			switch (a_state) {
			case State::kError:
				return IM_COL32(230, 60, 60, 255);
			case State::kConflict:
				return IM_COL32(235, 200, 40, 255);
			case State::kOne:
				return IM_COL32(80, 200, 90, 255);
			default:
				return IM_COL32(140, 140, 140, 255);
			}
		}

		ImGuiMCP::ImVec4 Vec4Of(State a_state)
		{
			switch (a_state) {
			case State::kError:
				return ImGuiMCP::ImVec4{ 0.90f, 0.24f, 0.24f, 1.0f };
			case State::kConflict:
				return ImGuiMCP::ImVec4{ 0.92f, 0.78f, 0.16f, 1.0f };
			case State::kOne:
				return ImGuiMCP::ImVec4{ 0.31f, 0.78f, 0.35f, 1.0f };
			default:
				return ImGuiMCP::ImVec4{ 0.55f, 0.55f, 0.55f, 1.0f };
			}
		}

		std::string GearText(const SubMod& a_sub)
		{
			if (a_sub.legacy) {
				return a_sub.otherConditions ? "DAR conditions" : "always";
			}
			std::string out;
			if (a_sub.gear & kGearLight) out += "light armor ";
			if (a_sub.gear & kGearHeavy) out += "heavy armor ";
			if (a_sub.gear & kGearCloth) out += "clothing ";
			if (out.empty()) {
				out = a_sub.otherConditions ? "has conditions" : "always";
			}
			return out;
		}

		// "Mod name: OAR mod / submod" or "Mod name (DAR folder 33001)"
		std::string FromText(const SubMod& a_sub)
		{
			if (a_sub.legacy) {
				const auto folder = a_sub.dir.filename().string();
				return a_sub.source.empty() ? "DAR folder " + folder : std::format("{} (DAR folder {})", a_sub.source, folder);
			}
			if (a_sub.source.empty() || Lower(a_sub.source) == Lower(a_sub.mod)) {
				return std::format("{} / {}", a_sub.mod, a_sub.name);
			}
			return std::format("{}: {} / {}", a_sub.source, a_sub.mod, a_sub.name);
		}

		// ---------------------------------------------------------------- pictures
		// Data\Interface\AnimationWatch\<file>.png, one per activity. Points on each picture, as a fraction of its
		// width / height: where weapon animations point, where left-side and right-side animations point, and the body.
		struct Picture
		{
			const char*      file;
			float            aspect;  // width / height
			ImGuiMCP::ImVec2 weapon;
			ImGuiMCP::ImVec2 left;
			ImGuiMCP::ImVec2 right;
			ImGuiMCP::ImVec2 body;
		};

		const Picture kPictures[] = {
			{ "running", 576.0f / 768.0f, { 0.86f, 0.36f }, { 0.20f, 0.45f }, { 0.86f, 0.36f }, { 0.55f, 0.40f } },       // Running
			{ "sprinting", 577.0f / 768.0f, { 0.86f, 0.30f }, { 0.20f, 0.40f }, { 0.86f, 0.30f }, { 0.55f, 0.40f } },     // Sprinting
			{ "walking", 576.0f / 768.0f, { 0.76f, 0.48f }, { 0.30f, 0.45f }, { 0.76f, 0.48f }, { 0.50f, 0.36f } },       // Walking
			{ "sneaking", 702.0f / 768.0f, { 0.86f, 0.42f }, { 0.16f, 0.50f }, { 0.86f, 0.42f }, { 0.55f, 0.45f } },      // Sneaking
			{ "jumping", 576.0f / 768.0f, { 0.86f, 0.24f }, { 0.25f, 0.35f }, { 0.76f, 0.30f }, { 0.50f, 0.36f } },       // Jumping
			{ "swimming", 768.0f / 384.0f, { 0.88f, 0.45f }, { 0.12f, 0.62f }, { 0.88f, 0.45f }, { 0.55f, 0.50f } },      // Swimming
			{ "one_handed", 577.0f / 768.0f, { 0.30f, 0.56f }, { 0.30f, 0.45f }, { 0.68f, 0.45f }, { 0.50f, 0.36f } },    // One-handed
			{ "two_handed", 576.0f / 768.0f, { 0.84f, 0.42f }, { 0.25f, 0.50f }, { 0.68f, 0.43f }, { 0.48f, 0.38f } },    // Two-handed
			{ "bow", 577.0f / 768.0f, { 0.74f, 0.24f }, { 0.15f, 0.25f }, { 0.74f, 0.24f }, { 0.45f, 0.42f } },           // Bow
			{ "spellcasting", 576.0f / 768.0f, { 0.82f, 0.18f }, { 0.15f, 0.38f }, { 0.82f, 0.18f }, { 0.45f, 0.40f } },  // Spellcasting
			{ "unarmed", 576.0f / 768.0f, { 0.75f, 0.28f }, { 0.30f, 0.40f }, { 0.75f, 0.28f }, { 0.50f, 0.36f } },       // Unarmed
			{ "horse", 691.0f / 768.0f, { 0.27f, 0.31f }, { 0.15f, 0.60f }, { 0.88f, 0.48f }, { 0.45f, 0.35f } },         // Horse
			{ "idle", 577.0f / 768.0f, { 0.62f, 0.55f }, { 0.35f, 0.45f }, { 0.62f, 0.45f }, { 0.50f, 0.35f } },          // Idle
			{ "other", 576.0f / 768.0f, { 0.80f, 0.38f }, { 0.20f, 0.40f }, { 0.80f, 0.38f }, { 0.50f, 0.36f } },         // Other
		};
		static_assert(std::size(kPictures) == std::size(kActivities));

		constexpr ImGuiMCP::ImU32 kCream = IM_COL32(243, 219, 190, 255);  // the pictures' background
		constexpr ImGuiMCP::ImU32 kBrown = IM_COL32(70, 45, 25, 255);     // the outfits' dark brown
		constexpr ImGuiMCP::ImU32 kBrownSoft = IM_COL32(120, 85, 55, 255);

		ImGuiMCP::ImTextureID g_textures[std::size(kPictures)]{};
		bool                  g_textureTried[std::size(kPictures)]{};

		ImGuiMCP::ImTextureID TextureOf(int a_activity)
		{
			if (!g_textureTried[a_activity]) {
				g_textureTried[a_activity] = true;
				const auto path = std::format("Data\\Interface\\AnimationWatch\\{}.png", kPictures[a_activity].file);
				g_textures[a_activity] = SKSEMenuFramework::LoadTexture(path);
				if (!g_textures[a_activity]) {
					logger::warn("Animation dolls: could not load {}", path);
				}
			}
			return g_textures[a_activity];
		}

		// a picture fitted inside a box, centred; returns the picture's rectangle
		void DrawPicture(int a_activity, ImGuiMCP::ImVec2 a_min, ImGuiMCP::ImVec2 a_max, ImGuiMCP::ImVec2& a_picMin, ImGuiMCP::ImVec2& a_picMax)
		{
			using namespace ImGuiMCP;
			auto*       dl = GetWindowDrawList();
			const float boxW = a_max.x - a_min.x, boxH = a_max.y - a_min.y;
			const float aspect = kPictures[a_activity].aspect;
			float       w = boxW, h = boxW / aspect;
			if (h > boxH) {
				h = boxH;
				w = boxH * aspect;
			}
			a_picMin = ImVec2{ a_min.x + (boxW - w) * 0.5f, a_min.y + (boxH - h) * 0.5f };
			a_picMax = ImVec2{ a_picMin.x + w, a_picMin.y + h };
			if (const auto tex = TextureOf(a_activity)) {
				ImDrawListManager::AddImage(dl, tex, a_picMin, a_picMax, ImVec2{ 0.0f, 0.0f }, ImVec2{ 1.0f, 1.0f }, IM_COL32(255, 255, 255, 255));
			} else {
				ImDrawListManager::AddText(dl, ImVec2{ a_picMin.x + 8.0f, a_picMin.y + 8.0f }, kBrown, "(picture missing: Data\\Interface\\AnimationWatch)");
			}
		}

		// which side of the picture an animation's label goes to, and what it points at
		enum class Side
		{
			kLeft,
			kRight,
			kWeapon,
			kBody
		};

		Side SideOf(const std::string& a_fileLower, int a_activity)
		{
			const auto& f = a_fileLower;
			auto has = [&](std::string_view s) { return f.find(s) != std::string::npos; };
			auto starts = [&](std::string_view s) { return f.rfind(s, 0) == 0; };
			auto ends = [&](std::string_view s) { return f.size() >= s.size() && f.compare(f.size() - s.size(), s.size(), s) == 0; };
			// a spell hand says the side on its own
			if (starts("mlh_") || has("lefthand")) {
				return Side::kLeft;
			}
			if (starts("mrh_") || has("righthand")) {
				return Side::kRight;
			}
			auto direction = [&]() -> std::optional<Side> {
				if (has("left") || ends("_l") || has("_l_") || has("backward") || has("_back") || ends("back")) {
					return Side::kLeft;
				}
				if (has("right") || ends("_r") || has("_r_") || has("forward") || has("fwd")) {
					return Side::kRight;
				}
				return std::nullopt;
			};
			const bool weapon = has("attack") || has("power") || has("bash") || has("block") || has("draw") || has("sheath") || has("equip") ||
			                    has("arrow") || has("shoot") || has("aim") || has("cast") || has("release") || has("ready") || has("charge") ||
			                    has("swing") || has("combo") || has("kill") || has("recoil") || has("parry") || has("slash") || has("stab") ||
			                    has("strike") || has("shield") || has("knock") || starts("bow") || has("crossbow");
			const bool moving = a_activity <= 5 || a_activity == 11;  // running .. swimming, horse
			if (moving) {
				if (const auto d = direction()) {
					return *d;
				}
				return weapon ? Side::kWeapon : Side::kBody;
			}
			if (weapon) {
				return Side::kWeapon;
			}
			if (const auto d = direction()) {
				return *d;
			}
			return Side::kBody;
		}

		int FreeDarNumber(const SubMod& a_sub, int a_above)
		{
			std::error_code ec;
			const auto      real = RealPathOf(a_sub.dir).parent_path();
			int             n = a_above + 1;
			while (fs::exists(a_sub.dir.parent_path() / std::to_string(n), ec) || fs::exists(real / std::to_string(n), ec)) {
				++n;
			}
			return n;
		}

		std::string RenameDar(SubMod& a_sub, int a_number)
		{
			const auto real = RealPathOf(a_sub.dir);
			const auto target = real.parent_path() / std::to_string(a_number);
			const auto old = a_sub.dir.filename().string();
			if (!MoveFileExW(real.wstring().c_str(), target.wstring().c_str(), 0)) {
				return std::format("Could not rename {} to {} (Windows error {}). Close any program that has a file in it open (MO2's file view, Explorer) and try again.",
					real.string(), target.filename().string(), GetLastError());
			}
			std::error_code ec;
			if (!fs::is_directory(target, ec)) {
				return std::format("The rename of {} reported success but {} is not there.", real.string(), target.string());
			}
			logger::info("Dolls: DAR folder {} renamed to {} ({})", real.string(), a_number, target.string());
			a_sub.dir = a_sub.dir.parent_path() / std::to_string(a_number);
			a_sub.priority = a_number;
			a_sub.original = a_number;
			a_sub.modFolder = "DAR " + std::to_string(a_number);
			a_sub.name = "folder " + std::to_string(a_number);
			return std::format("DAR folder {} renamed to {} ({}). Restart the game for it to win. To undo, rename it back to {}.", old, a_number, target.string(), old);
		}

		std::size_t g_renameSub = SIZE_MAX;  // DAR submod waiting for "Yes, rename"
		int         g_renameTo = 0;

		void RenderPopup()
		{
			using namespace ImGuiMCP;
			PushStyleColor(ImGuiCol_PopupBg, ImVec4{ 0.07f, 0.07f, 0.09f, 1.0f });  // solid, so the page behind does not show through
			const bool open = BeginPopup("doll_slot");
			PopStyleColor();
			if (!open) {
				return;
			}
			const auto it = g_slots.find(g_popupSlot);
			if (it == g_slots.end()) {
				EndPopup();
				return;
			}
			const auto& slot = it->second;
			const auto  ranked = Ranked(slot);
			TextUnformatted(std::format("{} ({})", slot.file, slot.firstPerson ? "first person" : "third person").c_str());
			TextUnformatted(slot.key.substr(2).c_str());
			Separator();
			if (ranked.empty()) {
				TextUnformatted("No animation pack replaces this for the gear shown: the game's own animation plays.");
			}
			int maxOther = INT_MIN;
			for (const auto i : ranked) {
				maxOther = std::max(maxOther, g_subMods[i].priority);
			}
			for (std::size_t r = 0; r < ranked.size(); ++r) {
				auto& s = g_subMods[ranked[r]];
				PushID(static_cast<int>(ranked[r]));
				if (r == 0) {
					PushStyleColor(ImGuiCol_Text, ImVec4{ 0.47f, 0.80f, 1.0f, 1.0f });  // the winner: light sky blue
				}
				TextUnformatted(std::format("{} {}   priority {}{}   [{}]", r == 0 ? "WINS:" : "     ", FromText(s), s.priority,
									s.changed ? std::format(" (was {}, changed by AnimationWatch)", s.original) : std::string(), GearText(s))
									.c_str());
				if (s.legacy) {
					if (r != 0) {
						TextWrapped("      Old DAR folder: to make it win, folder %s in DynamicAnimationReplacer\\_CustomConditions needs a number above %d.",
							s.dir.filename().string().c_str(), maxOther);
						if (g_renameSub != ranked[r]) {
							if (SmallButton("Rename it for me")) {
								g_renameSub = ranked[r];
								g_renameTo = FreeDarNumber(s, maxOther);
							}
						} else {
							TextWrapped("      Rename folder %s to %d?  (%s)", s.dir.filename().string().c_str(), g_renameTo, RealPathOf(s.dir).string().c_str());
							if (SmallButton("Yes, rename it")) {
								g_result = RenameDar(s, g_renameTo);
								g_renameSub = SIZE_MAX;
							}
							SameLine();
							if (SmallButton("No")) {
								g_renameSub = SIZE_MAX;
							}
						}
					}
				} else {
					// highest priority among the others for this slot and gear
					int top = INT_MIN, topOfOtherMods = INT_MIN;
					for (const auto k : ranked) {
						if (k == ranked[r]) {
							continue;
						}
						top = std::max(top, g_subMods[k].priority);
						if (g_subMods[k].modFolder != s.modFolder) {
							topOfOtherMods = std::max(topOfOtherMods, g_subMods[k].priority);
						}
					}
					if (r != 0 && SmallButton("Make winner")) {
						g_result = SetPriority(s, top + 1);
						if (g_result.empty()) {
							g_result = std::format("{} / {} now wins {} (priority {}). Restart the game for OAR to use it.", s.mod, s.name, slot.file, s.priority);
						}
					}
					if (r != 0 && topOfOtherMods != INT_MIN && s.priority <= topOfOtherMods) {
						SameLine();
						if (SmallButton("Make whole mod win")) {
							const int delta = topOfOtherMods + 1 - s.priority;
							int       count = 0;
							std::string error;
							for (auto& other : g_subMods) {
								if (other.modFolder == s.modFolder && !other.legacy && other.dir.parent_path() == s.dir.parent_path()) {
									if (auto e = SetPriority(other, other.priority + delta); !e.empty()) {
										error = e;
									} else {
										++count;
									}
								}
							}
							g_result = error.empty() ? std::format("{}: {} submods raised by {} (their order kept). Restart the game for OAR to use it.", s.mod, count, delta) : error;
						}
					}
					if (s.changed) {
						if (r != 0) {
							SameLine();
						}
						if (SmallButton("Undo")) {
							g_result = UndoPriority(s);
							if (g_result.empty()) {
								g_result = std::format("{} / {} back to priority {}. Restart the game for OAR to use it.", s.mod, s.name, s.priority);
							}
						}
					}
				}
				if (r == 0) {
					PopStyleColor();
					Separator();
				}
				PopID();
			}
			if (ranked.size() > 1) {
				Separator();
				TextWrapped("The highest priority wins whenever its conditions pass. A pack limited to some gear or situation still only plays then: making it the winner does not make it play everywhere.");
			}
			if (const auto live = g_live.find(slot.key); live != g_live.end()) {
				TextUnformatted(std::format("Last seen playing: {}", live->second.source).c_str());
			}
			if (g_flipFlopped.contains(slot.key)) {
				TextColored(Vec4Of(State::kError), "%s", "Flip-flopped between packs this session (switching mid-step).");
			}
			Separator();
			if (Button("Close")) {
				CloseCurrentPopup();
			}
			EndPopup();
		}

		// ---------------------------------------------------------------- the two views
		int g_view = -1;  // -1: all pictures, else the activity shown large

		struct Row
		{
			const Slot*              slot;
			std::vector<std::size_t> ranked;
			State                    state;
			Side                     side;
		};

		std::vector<Row> RowsOf(int a_activity)
		{
			std::vector<Row> rows;
			for (const auto& [key, slot] : g_slots) {
				if (slot.activity == a_activity && slot.firstPerson == g_firstPerson) {
					Row r{ &slot, Ranked(slot), State::kVanilla, Side::kBody };
					r.state = StateOf(slot, r.ranked);
					r.side = SideOf(Lower(slot.file), a_activity);
					rows.push_back(std::move(r));
				}
			}
			return rows;
		}

		// all pictures, as large as the page allows, each with its name below
		void RenderGrid()
		{
			using namespace ImGuiMCP;
			const auto avail = GetContentRegionAvail();
			if (!BeginChild("doll_grid", avail)) {
				EndChild();
				return;
			}
			int counts[kActivityCount][4]{};
			for (const auto& [key, slot] : g_slots) {
				if (slot.firstPerson == g_firstPerson) {
					++counts[slot.activity][static_cast<int>(StateOf(slot, Ranked(slot)))];
				}
			}
			const float gap = 12.0f;
			const float labelH = GetTextLineHeight() + 12.0f;
			const float width = GetContentRegionAvail().x - 4.0f;
			// the fewest columns (largest pictures) that still fit the whole set on the page; else scroll
			int cols = std::max(2, static_cast<int>((width + gap) / (200.0f + gap)));
			for (int c = 3; c <= 8; ++c) {
				const float tileW = (width - gap * static_cast<float>(c - 1)) / static_cast<float>(c);
				const float tileH = tileW * 1.25f + labelH;
				const int   rowsNeeded = (kActivityCount + c - 1) / c;
				if (static_cast<float>(rowsNeeded) * (tileH + gap) <= avail.y) {
					cols = c;
					break;
				}
			}
			const float tileW = (width - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
			const float picH = tileW * 1.25f;
			const auto  origin = GetCursorScreenPos();
			auto*       dl = GetWindowDrawList();
			for (int a = 0; a < kActivityCount; ++a) {
				const int   col = a % cols, row = a / cols;
				const ImVec2 p0{ origin.x + static_cast<float>(col) * (tileW + gap), origin.y + static_cast<float>(row) * (picH + labelH + gap) };
				const ImVec2 p1{ p0.x + tileW, p0.y + picH + labelH };
				SetCursorScreenPos(p0);
				PushID(a);
				const bool clicked = InvisibleButton("tile", ImVec2{ tileW, picH + labelH });
				const bool hovered = IsItemHovered();
				PopID();
				ImDrawListManager::AddRectFilled(dl, p0, p1, kCream, 8.0f, 0);
				ImVec2 picMin, picMax;
				DrawPicture(a, ImVec2{ p0.x + 4.0f, p0.y + 4.0f }, ImVec2{ p1.x - 4.0f, p0.y + picH }, picMin, picMax);
				// label
				const ImVec2 l0{ p0.x, p0.y + picH }, l1{ p1.x, p1.y };
				ImDrawListManager::AddLine(dl, ImVec2{ l0.x + 10.0f, l0.y }, ImVec2{ l1.x - 10.0f, l0.y }, kBrownSoft, 1.0f);
				const auto ts = CalcTextSize(kActivities[a]);
				ImDrawListManager::AddText(dl, ImVec2{ l0.x + (tileW - ts.x) * 0.5f, l0.y + (labelH - ts.y) * 0.5f }, kBrown, kActivities[a]);
				ImDrawListManager::AddRect(dl, p0, p1, hovered ? kBrown : kBrownSoft, 8.0f, 0, hovered ? 3.0f : 1.0f);
				// problem badges: red (flip-flopped) and yellow (competing) counts
				float bx = p1.x - 14.0f;
				for (const State s : { State::kError, State::kConflict }) {
					if (const int n = counts[a][static_cast<int>(s)]; n > 0) {
						const auto text = std::to_string(n);
						const auto bs = CalcTextSize(text.c_str());
						const float r = std::max(bs.x, bs.y) * 0.5f + 5.0f;
						ImDrawListManager::AddCircleFilled(dl, ImVec2{ bx - r + 6.0f, p0.y + r + 6.0f }, r, ColorOf(s), 20);
						ImDrawListManager::AddText(dl, ImVec2{ bx - r + 6.0f - bs.x * 0.5f, p0.y + r + 6.0f - bs.y * 0.5f }, IM_COL32(20, 12, 5, 255), text.c_str());
						bx -= 2.0f * r + 4.0f;
					}
				}
				if (hovered) {
					const int total = counts[a][0] + counts[a][1] + counts[a][2] + counts[a][3];
					SetTooltip("%s: %d animations replaced or watched\n%d one pack (green), %d competing (yellow), %d flip-flopped (red)\nClick to open", kActivities[a],
						total, counts[a][1], counts[a][2], counts[a][3]);
				}
				if (clicked) {
					g_view = a;
				}
			}
			const int rowsUsed = (kActivityCount + cols - 1) / cols;
			SetCursorScreenPos(origin);
			Dummy(ImVec2{ width, static_cast<float>(rowsUsed) * (picH + labelH + gap) });
			EndChild();
		}

		// one picture, large in the middle, with its animations around it pointing at it
		void RenderDetail(int a_activity)
		{
			using namespace ImGuiMCP;
			if (Button("<  Back to all pictures")) {
				g_view = -1;
				return;
			}
			auto rows = RowsOf(a_activity);
			int  counts[4]{};
			for (const auto& r : rows) {
				++counts[static_cast<int>(r.state)];
			}
			SameLine();
			TextUnformatted(std::format("{}:  {} animations  -  {} green, {} yellow, {} red, {} game's own", kActivities[a_activity], rows.size(), counts[1], counts[2],
				counts[3], counts[0])
								.c_str());
			const auto avail = GetContentRegionAvail();
			if (!BeginChild("doll_detail", avail)) {
				EndChild();
				return;
			}
			const auto  origin = GetCursorScreenPos();
			const float width = GetContentRegionAvail().x - 4.0f;
			const float viewH = avail.y - 8.0f;
			const float lineH = GetTextLineHeight();
			const float rowH = lineH * 2.0f + 12.0f, rowGap = 6.0f;
			const float colW = std::clamp(width * 0.27f, 220.0f, 430.0f);
			const auto& pic = kPictures[a_activity];

			// left column / right column
			std::vector<Row*> left, right;
			for (auto& r : rows) {
				Side s = r.side;
				if (s == Side::kWeapon) {
					s = pic.weapon.x < 0.5f ? Side::kLeft : Side::kRight;
				}
				if (s == Side::kLeft) {
					left.push_back(&r);
				} else if (s == Side::kRight) {
					right.push_back(&r);
				}
			}
			for (auto& r : rows) {  // body animations fill whichever side is shorter
				if (r.side == Side::kBody) {
					(left.size() <= right.size() ? left : right).push_back(&r);
				}
			}

			// the picture: as large as the space between the columns allows
			const float picBoxW = std::max(160.0f, width - 2.0f * colW - 80.0f);
			const ImVec2 box0{ origin.x + (width - picBoxW) * 0.5f, origin.y };
			const ImVec2 box1{ box0.x + picBoxW, origin.y + viewH };
			auto*        dl = GetWindowDrawList();
			ImVec2       picMin, picMax;
			DrawPicture(a_activity, box0, box1, picMin, picMax);
			auto at = [&](ImVec2 n) { return ImVec2{ picMin.x + n.x * (picMax.x - picMin.x), picMin.y + n.y * (picMax.y - picMin.y) }; };

			if (rows.empty()) {
				ImDrawListManager::AddText(dl, ImVec2{ origin.x, origin.y }, IM_COL32(220, 220, 220, 255), "No animation pack replaces anything here.");
			}

			auto drawColumn = [&](std::vector<Row*>& a_column, bool a_left) {
				const float total = static_cast<float>(a_column.size()) * (rowH + rowGap);
				float       y = origin.y + std::max(0.0f, (viewH - total) * 0.5f);
				const float x = a_left ? origin.x : origin.x + width - colW;
				for (auto* r : a_column) {
					const ImVec2 p0{ x, y }, p1{ x + colW, y + rowH };
					SetCursorScreenPos(p0);
					const bool clicked = InvisibleButton(r->slot->key.c_str(), ImVec2{ colW, rowH });
					const bool hovered = IsItemHovered();
					const auto color = ColorOf(r->state);
					// what it points at
					ImVec2 target;
					switch (r->side) {
					case Side::kWeapon:
						target = at(pic.weapon);
						break;
					case Side::kLeft:
						target = at(pic.left);
						break;
					case Side::kRight:
						target = at(pic.right);
						break;
					default:
						target = at(pic.body);
						break;
					}
					const ImVec2 from{ a_left ? p1.x : p0.x, y + rowH * 0.5f };
					ImDrawListManager::AddLine(dl, from, target, color, hovered ? 3.0f : 1.5f);
					ImDrawListManager::AddCircleFilled(dl, target, 4.0f, color, 12);
					// the label
					ImDrawListManager::AddRectFilled(dl, p0, p1, kCream, 5.0f, 0);
					ImDrawListManager::AddRectFilled(dl, p0, ImVec2{ p0.x + 7.0f, p1.y }, color, 5.0f, 0);
					ImDrawListManager::AddRect(dl, p0, p1, hovered ? kBrown : color, 5.0f, 0, hovered ? 2.5f : 1.5f);
					std::string from1 = r->ranked.empty() ? std::string("game's own animation") :
					                                        "from " + FromText(g_subMods[r->ranked[0]]);
					if (r->ranked.size() > 1) {
						from1 += std::format("  (+{} competing)", r->ranked.size() - 1);
					}
					ImDrawListManager::PushClipRect(dl, p0, ImVec2{ p1.x - 4.0f, p1.y }, true);
					const auto folder = fs::path(r->slot->key.substr(2)).parent_path().string();
					const auto name1 = folder.empty() ? r->slot->file : std::format("{}   ({})", r->slot->file, folder);
					ImDrawListManager::AddText(dl, ImVec2{ p0.x + 13.0f, p0.y + 5.0f }, kBrown, name1.c_str());
					ImDrawListManager::AddText(dl, ImVec2{ p0.x + 13.0f, p0.y + 7.0f + lineH }, kBrownSoft, from1.c_str());
					ImDrawListManager::PopClipRect(dl);
					if (hovered) {
						SetTooltip("%s\n%s\n%s\nClick to see the competing packs and pick the winner", r->slot->file.c_str(), from1.c_str(), r->slot->key.substr(2).c_str());
					}
					if (clicked) {
						g_popupSlot = r->slot->key;
						g_renameSub = SIZE_MAX;
						OpenPopup("doll_slot");
					}
					y += rowH + rowGap;
				}
				return y;
			};
			const float endL = drawColumn(left, true);
			const float endR = drawColumn(right, false);
			SetCursorScreenPos(origin);
			Dummy(ImVec2{ width, std::max(viewH, std::max(endL, endR) - origin.y) });
			RenderPopup();
			EndChild();
		}
	}

	void StartScan()
	{
		std::thread([]() {
			try {
				Scan();
			} catch (const std::exception& e) {
				logger::error("Animation dolls: scan failed: {}", e.what());
				std::scoped_lock l{ g_lock };
				g_scanInfo = std::format("reading the animation packs failed: {}", e.what());
			}
		}).detach();
	}

	void OnPlayed(const std::string& a_graph, const std::string& a_path, const std::string& a_source)
	{
		const auto key = KeyOfPath(a_graph, a_path);
		std::scoped_lock l{ g_lock };
		auto& live = g_live[key];
		live.source = a_source;
	}

	void MarkFlipFlop(const std::string& a_graph, const std::string& a_path)
	{
		const auto key = KeyOfPath(a_graph, a_path);
		std::scoped_lock l{ g_lock };
		g_flipFlopped.insert(key);
	}

	void __stdcall Render()
	{
		using namespace ImGuiMCP;
		std::scoped_lock l{ g_lock };
		TextUnformatted("Which animation pack wins each animation. Green: one pack.  Yellow: packs compete.  Red: flip-flopped this session.  Grey: game's own.");
		TextUnformatted(g_scanInfo.c_str());
		if (!g_scanned) {
			return;
		}
		const char* gears[] = { "All gear", "Light armor", "Heavy armor", "Clothing" };
		const int   gearBits[] = { 0, kGearLight, kGearHeavy, kGearCloth };
		int         gearIndex = 0;
		for (int i = 0; i < 4; ++i) {
			if (gearBits[i] == g_gear) {
				gearIndex = i;
			}
		}
		if (BeginCombo("Gear", gears[gearIndex])) {
			for (int i = 0; i < 4; ++i) {
				if (Selectable(gears[i], i == gearIndex)) {
					g_gear = gearBits[i];
				}
			}
			EndCombo();
		}
		SameLine();
		Checkbox("First person", &g_firstPerson);
		if (!g_result.empty()) {
			TextWrapped("%s", g_result.c_str());
		}
		Separator();
		if (g_view < 0 || g_view >= kActivityCount) {
			g_view = -1;
			RenderGrid();
		} else {
			RenderDetail(g_view);
		}
	}
}
