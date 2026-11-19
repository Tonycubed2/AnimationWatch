// Animation dolls - AnimationWatch 1.3.0
//
// F1 > Animation Watch > Animation Dolls: one page per activity (running, walking, jumping, one-handed ...).
// Each page shows a figure with a label for every animation slot of that activity (run forward, run left ...)
// and which animation pack wins it:
//   green  - one pack provides it for the gear shown
//   yellow - two or more packs compete for it (the one with the higher OAR priority wins when its conditions pass)
//   red    - it flip-flopped between packs this session (switching mid-step)
//   grey   - no pack replaces it (vanilla)
// "Gear" picks the view: all gear, light armor, heavy armor, clothing. Packs whose conditions name a gear type
// (the game's ArmorLight / ArmorHeavy / ArmorClothing keywords, or the words in their folder names) only show
// on that gear's view; packs without such a condition show on every view.
//
// Clicking a label lists the competing packs. "Make winner" raises that pack's OAR priority just above the others
// for that slot, by writing OAR's own user.json for the submod (a copy of its settings with the new priority, the
// original priority kept as "animationWatchOriginalPriority"). "Make whole mod win" does that for every submod of
// the mod, keeping their order. "Undo" puts the original priority back. OAR reads user.json when the game starts.
// Old DAR folders cannot be changed this way; the page says which folder number to rename to.
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

		// the figure, with a line from its body to every label row
		void DrawDoll(const char* a_title, ImGuiMCP::ImVec2 a_origin, const std::vector<std::pair<float, ImGuiMCP::ImU32>>& a_rows, float a_labelX)
		{
			using namespace ImGuiMCP;
			auto*       dl = GetWindowDrawList();
			const ImU32 body = IM_COL32(210, 210, 220, 255);
			const float cx = a_origin.x + 90.0f, top = a_origin.y + 40.0f;
			ImDrawListManager::AddText(dl, ImVec2{ a_origin.x + 20.0f, a_origin.y }, IM_COL32(255, 255, 255, 255), a_title);
			ImDrawListManager::AddCircle(dl, ImVec2{ cx, top + 18.0f }, 16.0f, body, 24, 2.5f);                      // head
			ImDrawListManager::AddLine(dl, ImVec2{ cx, top + 34.0f }, ImVec2{ cx, top + 120.0f }, body, 3.0f);      // spine
			ImDrawListManager::AddLine(dl, ImVec2{ cx, top + 50.0f }, ImVec2{ cx - 40.0f, top + 95.0f }, body, 3.0f);  // arms
			ImDrawListManager::AddLine(dl, ImVec2{ cx, top + 50.0f }, ImVec2{ cx + 40.0f, top + 95.0f }, body, 3.0f);
			ImDrawListManager::AddLine(dl, ImVec2{ cx, top + 120.0f }, ImVec2{ cx - 30.0f, top + 190.0f }, body, 3.0f);  // legs
			ImDrawListManager::AddLine(dl, ImVec2{ cx, top + 120.0f }, ImVec2{ cx + 30.0f, top + 190.0f }, body, 3.0f);
			for (const auto& [y, color] : a_rows) {
				ImDrawListManager::AddLine(dl, ImVec2{ cx + 20.0f, top + 75.0f }, ImVec2{ a_labelX - 6.0f, y }, color, 1.0f);
				ImDrawListManager::AddCircleFilled(dl, ImVec2{ a_labelX - 6.0f, y }, 4.0f, color, 12);
			}
		}

		void RenderPopup()
		{
			using namespace ImGuiMCP;
			if (!BeginPopup("doll_slot")) {
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
				TextUnformatted(std::format("{} {} / {}   priority {}{}   [{}]", r == 0 ? "WINS:" : "     ", s.mod, s.name, s.priority,
									s.changed ? std::format(" (was {}, changed by AnimationWatch)", s.original) : std::string(), GearText(s))
									.c_str());
				if (s.legacy) {
					if (r != 0) {
						TextWrapped("      Old DAR folder: to make it win, rename folder %s in DynamicAnimationReplacer\\_CustomConditions to a number above %d.",
							s.dir.filename().string().c_str(), maxOther);
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

		void RenderActivity(int a_activity)
		{
			using namespace ImGuiMCP;
			std::vector<const Slot*> slots;
			for (const auto& [key, slot] : g_slots) {
				if (slot.activity == a_activity && slot.firstPerson == g_firstPerson) {
					slots.push_back(&slot);
				}
			}
			if (slots.empty()) {
				TextUnformatted("No animation pack replaces anything here.");
				return;
			}
			int counts[4]{};
			const auto origin = GetCursorScreenPos();
			const float labelX = origin.x + 210.0f;
			std::vector<std::pair<float, ImU32>> rows;
			const float childHeight = std::max(260.0f, std::min(560.0f, 22.0f * static_cast<float>(slots.size()) + 10.0f));
			SetCursorScreenPos(ImVec2{ labelX, origin.y });
			BeginChild("labels", ImVec2{ 0.0f, childHeight });
			for (const auto* slot : slots) {
				const auto ranked = Ranked(*slot);
				const auto state = StateOf(*slot, ranked);
				++counts[static_cast<int>(state)];
				std::string winner = ranked.empty() ? std::string("game's own animation") : g_subMods[ranked[0]].mod + " / " + g_subMods[ranked[0]].name;
				if (ranked.size() > 1) {
					winner += std::format("   (+{} competing)", ranked.size() - 1);
				}
				const auto rowPos = GetCursorScreenPos();
				if (rowPos.y >= origin.y && rowPos.y < origin.y + childHeight - 8.0f) {
					rows.emplace_back(rowPos.y + 8.0f, ColorOf(state));
				}
				PushStyleColor(ImGuiCol_Text, Vec4Of(state));
				if (Selectable(std::format("{:<28} {}##{}", slot->file, winner, slot->key).c_str())) {
					g_popupSlot = slot->key;
					OpenPopup("doll_slot");
				}
				PopStyleColor();
				if (IsItemHovered()) {
					SetTooltip("%s", "Click to see the competing packs and pick the winner");
				}
			}
			RenderPopup();
			EndChild();
			const auto after = GetCursorScreenPos();
			DrawDoll(std::format("{}  -  {} green  {} yellow  {} red", kActivities[a_activity], counts[1], counts[2], counts[3]).c_str(), origin, rows, labelX);
			SetCursorScreenPos(ImVec2{ origin.x, std::max(after.y, origin.y + 250.0f) });
			Dummy(ImVec2{ 1.0f, 4.0f });
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
		if (BeginTabBar("doll_tabs")) {
			for (int a = 0; a < kActivityCount; ++a) {
				if (BeginTabItem(kActivities[a])) {
					RenderActivity(a);
					EndTabItem();
				}
			}
			EndTabBar();
		}
	}
}
