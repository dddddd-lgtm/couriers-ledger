// The Courier's Ledger: a list in the SKSE Menu Framework of the people the player has talked to.
//
// The list rests on the game's own memory: every person carries a flag saying whether they have talked
// to the player (the condition function GetTalkedToPC), so the list also works backwards in an old save.
//
// Everything here only reads. Nobody is moved and nothing is taken: inventories and merchant stock are
// shown, never more. The one change to the game is tracking: the quest CL_TrackQuest in
// CouriersLedger.esp gets the chosen person in its alias and shows its objective, so the person gets a
// quest marker.
//
// Names are read straight from the base record (TESFullName::fullName), never through
// GetDisplayFullName. NPCs Names Distributor hooks GetDisplayFullName, and another NPC finder crashed
// there on an NPC without a base record.
//
// Game data is read on the game's main thread (SKSE tasks). The menu draws on another thread and only
// reads finished snapshots.

#include "SKSEMenuFramework.h"

namespace
{
	namespace ui = ImGuiMCP;

	// ------------------------------------------------------------------ text

	bool IsUtf8(std::string_view a_text)
	{
		std::size_t i = 0;
		while (i < a_text.size()) {
			const auto c = static_cast<unsigned char>(a_text[i]);
			std::size_t more = 0;
			if (c < 0x80) {
				more = 0;
			} else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
				more = 1;
			} else if ((c & 0xF0) == 0xE0) {
				more = 2;
			} else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
				more = 3;
			} else {
				return false;
			}
			if (i + more >= a_text.size() && more > 0) {
				return false;
			}
			for (std::size_t k = 1; k <= more; ++k) {
				if ((static_cast<unsigned char>(a_text[i + k]) & 0xC0) != 0x80) {
					return false;
				}
			}
			i += more + 1;
		}
		return true;
	}

	void AppendUtf8(std::string& a_out, char32_t a_cp)
	{
		if (a_cp < 0x80) {
			a_out += static_cast<char>(a_cp);
		} else if (a_cp < 0x800) {
			a_out += static_cast<char>(0xC0 | (a_cp >> 6));
			a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
		} else {
			a_out += static_cast<char>(0xE0 | (a_cp >> 12));
			a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
			a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
		}
	}

	// The game's strings are Windows-1252. The menu wants UTF-8.
	std::string ToUtf8(const char* a_text)
	{
		if (!a_text) {
			return {};
		}
		const std::string_view text{ a_text };
		if (IsUtf8(text)) {
			return std::string{ text };
		}
		static constexpr char16_t cp1252[32] = {
			0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
			0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
		};
		std::string out;
		out.reserve(text.size() + 8);
		for (const auto ch : text) {
			const auto c = static_cast<unsigned char>(ch);
			char32_t cp = c;
			if (c >= 0x80 && c < 0xA0) {
				cp = cp1252[c - 0x80] ? cp1252[c - 0x80] : U'?';
			}
			AppendUtf8(out, cp);
		}
		return out;
	}

	// Lower case for search and sorting, including Latin-1 capitals in UTF-8.
	std::string Lower(const std::string& a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const auto c = static_cast<unsigned char>(a_text[i]);
			if (c < 0x80) {
				out += static_cast<char>(std::tolower(c));
			} else if (c == 0xC3 && i + 1 < a_text.size()) {
				auto d = static_cast<unsigned char>(a_text[i + 1]);
				if (d >= 0x80 && d <= 0x9E && d != 0x97) {
					d = static_cast<unsigned char>(d + 0x20);
				}
				out += static_cast<char>(c);
				out += static_cast<char>(d);
				++i;
			} else {
				out += static_cast<char>(c);
			}
		}
		return out;
	}

	// The tabs: 1 = A-F, 2 = G-L, 3 = M-R, 4 = S-Z and everything else.
	int LetterGroup(const std::string& a_key)
	{
		if (a_key.empty()) {
			return 4;
		}
		const auto c = static_cast<unsigned char>(a_key[0]);
		if (c >= 'a' && c <= 'f') {
			return 1;
		}
		if (c >= 'g' && c <= 'l') {
			return 2;
		}
		if (c >= 'm' && c <= 'r') {
			return 3;
		}
		return 4;
	}

	std::string FormName(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		return ToUtf8(a_form->GetName());
	}

	// ------------------------------------------------------------------ data

	enum Role : std::uint32_t
	{
		kTrainer = 1 << 0,
		kMerchant = 1 << 1,
		kFence = 1 << 2,
		kInnkeeper = 1 << 3,
		kFollower = 1 << 4,
		kCanFollow = 1 << 5,
		kCanMarry = 1 << 6,
		kDead = 1 << 7,
		kQuest = 1 << 8,      // target of a running quest
		kQuestPart = 1 << 9,  // takes part in a quest in the journal (one of its aliases), but is not the target now
	};

	struct Person
	{
		RE::FormID              ref = 0;
		std::string             name;
		std::string             key;
		int                     group = 4;
		std::uint32_t           roles = 0;
		std::string             where;
		std::string             trains;
		int                     trainMax = 0;
		std::string             sells;
		int                     openFrom = 0;
		int                     openTo = 0;
		std::vector<RE::FormID> chests;
		int                     relation = 0;
		std::string             tag;
		std::string             quests;          // quests this person is a target of right now
		std::string             partOf;          // quests in the journal this person takes part in
		bool                    nearby = false;  // same cell as the player (inside) or same world (outside)
		bool                    loaded = false;  // the person's 3D is loaded
		float                   meters = -1.0f;  // distance when nearby
	};

	struct Item
	{
		std::string name;
		std::string key;
		std::string effects;  // for example "Fortify Alteration 12" (enchantments, potions, scrolls)
		std::string effectsKey;
		int         count = 0;
		int         value = 0;
	};

	struct Detail
	{
		RE::FormID        ref = 0;
		bool              wares = false;
		bool              known = true;
		int               gold = 0;
		std::vector<Item> items;
	};

	struct Hit
	{
		RE::FormID    ref = 0;
		std::string   person;
		std::uint32_t roles = 0;
		bool          stock = true;  // the merchant's chest, otherwise the person's inventory
		std::string   item;
		std::string   effects;
		int           count = 0;
		int           value = 0;
	};

	using People = std::vector<Person>;
	using Hits = std::vector<Hit>;

	std::mutex                    g_lock;
	std::shared_ptr<const People> g_people;
	std::shared_ptr<const Detail> g_detail;
	std::shared_ptr<const Hits>   g_hits;
	std::string                   g_status = "Open the menu in a loaded save.";
	std::string                   g_hitsFor;
	std::string                   g_trackedName;
	std::atomic<bool>             g_busy{ false };
	std::atomic<RE::FormID>       g_tracked{ 0 };
	std::atomic<std::int64_t>     g_builtAt{ 0 };

	// Focus: while someone is tracked, the other quests' markers are switched off (the same as unticking
	// them in the journal), so the tracking marker does not drown among the other markers. They come back
	// when tracking stops. The list is kept in the SKSE co-save, so it survives a load.
	std::vector<RE::FormID> g_hidden;  // main thread only
	std::atomic<int>        g_hiddenCount{ 0 };
	std::atomic<bool>       g_focus{ true };

	std::int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	template <class T>
	std::shared_ptr<const T> Read(const std::shared_ptr<const T>& a_ptr)
	{
		std::scoped_lock l{ g_lock };
		return a_ptr;
	}

	std::string ReadString(const std::string& a_text)
	{
		std::scoped_lock l{ g_lock };
		return a_text;
	}

	void SetStatus(std::string a_text)
	{
		logs::info("{}", a_text);
		std::scoped_lock l{ g_lock };
		g_status = std::move(a_text);
	}

	// ------------------------------------------------------------------ conditions and factions

	RE::TESConditionItem* MakeCondition(RE::FUNCTION_DATA::FunctionID a_function)
	{
		auto cond = new RE::TESConditionItem();
		cond->next = nullptr;
		cond->data.functionData.function = a_function;
		cond->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
		cond->data.comparisonValue.f = 1.0f;
		cond->data.object = RE::CONDITIONITEMOBJECT::kSelf;
		return cond;
	}

	bool TalkedToPlayer(RE::Actor* a_actor)
	{
		static auto cond = MakeCondition(RE::FUNCTION_DATA::FunctionID::kGetTalkedToPC);
		RE::ConditionCheckParams params(a_actor, nullptr);
		return cond->IsTrue(params);
	}

	int Relation(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
	{
		static auto cond = MakeCondition(RE::FUNCTION_DATA::FunctionID::kGetRelationshipRank);
		cond->data.functionData.params[0] = a_player;
		RE::ConditionCheckParams params(a_actor, nullptr);
		for (const int rank : { 1, 2, 3, 4, -1, -2, -3, -4 }) {
			cond->data.comparisonValue.f = static_cast<float>(rank);
			if (cond->IsTrue(params)) {
				return rank;
			}
		}
		return 0;
	}

	const char* RelationName(int a_rank)
	{
		switch (a_rank) {
		case 4:
			return "lover";
		case 3:
			return "ally";
		case 2:
			return "confidant";
		case 1:
			return "friend";
		case -1:
			return "rival";
		case -2:
			return "foe";
		case -3:
			return "enemy";
		case -4:
			return "archnemesis";
		default:
			return "";
		}
	}

	const char* SkillName(int a_skill)
	{
		static constexpr const char* names[18] = {
			"One-Handed", "Two-Handed", "Archery", "Block", "Smithing", "Heavy Armor",
			"Light Armor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech",
			"Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"
		};
		return a_skill >= 0 && a_skill < 18 ? names[a_skill] : "?";
	}

	const char* TrainerLevel(int a_max)
	{
		if (a_max >= 90) {
			return "Master";
		}
		if (a_max >= 75) {
			return "Expert";
		}
		if (a_max >= 50) {
			return "Journeyman";
		}
		return "Apprentice";
	}

	struct KnownFactions
	{
		RE::TESFaction* follower = nullptr;   // CurrentFollowerFaction
		RE::TESFaction* canFollow = nullptr;  // PotentialFollowerFaction
		RE::TESFaction* canMarry = nullptr;   // PotentialMarriageFaction
		RE::TESFaction* innkeeper = nullptr;  // JobInnkeeperFaction
	};

	KnownFactions LoadFactions()
	{
		KnownFactions f;
		if (auto dh = RE::TESDataHandler::GetSingleton()) {
			f.follower = dh->LookupForm<RE::TESFaction>(0x05C84E, "Skyrim.esm");
			f.canFollow = dh->LookupForm<RE::TESFaction>(0x05C84D, "Skyrim.esm");
			f.canMarry = dh->LookupForm<RE::TESFaction>(0x019809, "Skyrim.esm");
			f.innkeeper = dh->LookupForm<RE::TESFaction>(0x05091B, "Skyrim.esm");
		}
		return f;
	}

	// The factions the person is in right now: the base record's factions, changed by the save.
	std::vector<std::pair<RE::TESFaction*, int>> FactionRanks(RE::Actor* a_actor)
	{
		std::vector<std::pair<RE::TESFaction*, int>> ranks;
		auto set = [&](RE::TESFaction* a_faction, int a_rank) {
			if (!a_faction) {
				return;
			}
			for (auto& [faction, rank] : ranks) {
				if (faction == a_faction) {
					rank = a_rank;
					return;
				}
			}
			ranks.emplace_back(a_faction, a_rank);
		};
		if (auto base = a_actor->GetActorBase()) {
			for (auto& entry : base->factions) {
				set(entry.faction, entry.rank);
			}
		}
		if (auto changes = a_actor->extraList.GetByType<RE::ExtraFactionChanges>()) {
			for (auto& entry : changes->factionChanges) {
				set(entry.faction, entry.rank);
			}
		}
		return ranks;
	}

	// What a merchant trades in, from the keywords in the faction's buy and sell list.
	std::string Sells(const RE::TESFaction* a_faction)
	{
		const auto& vendor = a_faction->vendorData;
		if (vendor.vendorValues.notBuySell) {
			return "everything";
		}
		std::vector<std::string> kinds;
		auto add = [&](const char* a_kind) {
			if (std::find(kinds.begin(), kinds.end(), a_kind) == kinds.end()) {
				kinds.emplace_back(a_kind);
			}
		};
		if (auto list = vendor.vendorSellBuyList) {
			for (auto form : list->forms) {
				auto keyword = form ? form->As<RE::BGSKeyword>() : nullptr;
				if (!keyword) {
					continue;
				}
				const auto id = Lower(keyword->GetFormEditorID() ? keyword->GetFormEditorID() : "");
				auto has = [&](std::string_view a_part) { return id.find(a_part) != std::string::npos; };
				if (has("weapon") || has("arrow") || has("ammo")) {
					add("weapons");
				} else if (has("armor") || has("ore") || has("ingot")) {
					add("armor and smithing");
				} else if (has("potion") || has("poison") || has("ingredient")) {
					add("alchemy");
				} else if (has("spelltome") || has("scroll") || has("staff") || has("soulgem")) {
					add("magic");
				} else if (has("food") || has("drink")) {
					add("food and drink");
				} else if (has("jewel") || has("gem")) {
					add("jewelry");
				} else if (has("cloth")) {
					add("clothing");
				} else if (has("hide") || has("pelt") || has("animalpart")) {
					add("hides");
				} else if (has("book") || has("recipe")) {
					add("books");
				} else if (has("tool") || has("clutter") || has("firewood")) {
					add("misc");
				}
			}
		}
		if (kinds.empty()) {
			return "unknown";
		}
		if (kinds.size() > 4) {
			return "everything";
		}
		std::string out;
		for (const auto& kind : kinds) {
			out += out.empty() ? kind : ", " + kind;
		}
		return out;
	}

	std::string Where(RE::Actor* a_actor)
	{
		std::string out;
		if (auto location = a_actor->GetCurrentLocation()) {
			out = FormName(location);
		}
		RE::TESObjectCELL* cell = a_actor->GetParentCell();
		if (!cell || !cell->IsInteriorCell()) {
			if (auto saved = a_actor->GetSaveParentCell()) {
				cell = saved;
			}
		}
		if (cell && cell->IsInteriorCell()) {
			const auto name = FormName(cell);
			if (!name.empty() && name != out) {
				out = out.empty() ? name : out + " - " + name;
			}
		}
		if (out.empty()) {
			if (auto world = a_actor->GetWorldspace()) {
				out = FormName(world);
			}
		}
		if (out.empty()) {
			if (auto home = a_actor->GetEditorLocation()) {
				const auto name = FormName(home);
				if (!name.empty()) {
					out = name + " (home)";
				}
			}
		}
		return out.empty() ? "unknown" : out;
	}

	RE::TESQuest* TrackQuest();
	bool          Transitional(const RE::TESQuest* a_quest);

	struct QuestPeople
	{
		std::unordered_map<RE::FormID, std::string> targets;  // who the shown objectives point at
		std::unordered_map<RE::FormID, std::string> parts;    // everyone else the quest holds in its aliases
	};

	void AddQuestName(std::string& a_list, const std::string& a_name)
	{
		if (a_list.find(a_name) == std::string::npos) {
			a_list += a_list.empty() ? a_name : ", " + a_name;
		}
	}

	// The quests in the journal (running, with at least one objective shown) and the people in them: who
	// the objectives point at, and the other people the quest holds, such as the one who gave it.
	QuestPeople QuestTargets()
	{
		QuestPeople out;
		auto dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return out;
		}
		const auto ours = TrackQuest();
		const auto player = RE::PlayerCharacter::GetSingleton();
		for (auto quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest || quest == ours || Transitional(quest) || !quest->IsEnabled() || quest->IsCompleted()) {
				continue;
			}
			std::string name;
			auto questName = [&]() -> const std::string& {
				if (name.empty()) {
					name = ToUtf8(quest->GetFullName());
					if (name.empty()) {
						name = "unnamed quest";
					}
				}
				return name;
			};
			bool shown = false;
			for (auto objective : quest->objectives) {
				if (!objective || objective->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
					continue;
				}
				shown = true;
				if (!objective->targets) {
					continue;
				}
				for (std::uint32_t i = 0; i < objective->numTargets; ++i) {
					auto target = objective->targets[i];
					if (!target) {
						continue;
					}
					auto ref = quest->GetAliasedRef(target->alias).get();
					if (!ref || !ref->Is(RE::FormType::ActorCharacter)) {
						continue;
					}
					AddQuestName(out.targets[ref->GetFormID()], questName());
				}
			}
			if (!shown) {
				continue;
			}
			for (auto alias : quest->aliases) {
				if (!alias) {
					continue;
				}
				auto ref = quest->GetAliasedRef(alias->aliasID).get();
				if (!ref || ref.get() == player || !ref->Is(RE::FormType::ActorCharacter)) {
					continue;
				}
				AddQuestName(out.parts[ref->GetFormID()], questName());
			}
		}
		return out;
	}

	// ------------------------------------------------------------------ the list (main thread)

	void BuildPeople()
	{
		const auto started = std::chrono::steady_clock::now();
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			SetStatus("Load a save first.");
			return;
		}

		std::vector<RE::Actor*> actors;
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSReadLockGuard guard{ lock.get() };
			if (map) {
				for (auto& entry : *map) {
					auto form = entry.second;
					if (form && form->Is(RE::FormType::ActorCharacter)) {
						if (auto actor = form->As<RE::Actor>()) {
							actors.push_back(actor);
						}
					}
				}
			}
		}

		const auto factions = LoadFactions();
		const auto questTargets = QuestTargets();
		auto people = std::make_shared<People>();
		std::unordered_map<RE::FormID, std::size_t> byBase;

		for (auto actor : actors) {
			if (actor == player || actor->IsDeleted() || actor->IsDisabled()) {
				continue;
			}
			auto base = actor->GetActorBase();
			if (!base) {
				continue;
			}
			const char* raw = base->fullName.c_str();
			if (!raw || !*raw) {
				continue;
			}
			// In the list: people the player has talked to, and everyone in a quest in the journal.
			const auto quest = questTargets.targets.find(actor->GetFormID());
			const bool isTarget = quest != questTargets.targets.end();
			const auto part = questTargets.parts.find(actor->GetFormID());
			const bool isPart = part != questTargets.parts.end();
			if (!isTarget && !isPart && (!base->IsUnique() || !TalkedToPlayer(actor))) {
				continue;
			}

			Person p;
			p.ref = actor->GetFormID();
			p.name = ToUtf8(raw);
			p.key = Lower(p.name);
			p.group = LetterGroup(p.key);
			if (actor->IsDead()) {
				p.roles |= kDead;
			}
			if (isTarget) {
				p.roles |= kQuest;
				p.quests = quest->second;
			}
			if (isPart) {
				// Only the quests where the person is not already the target.
				for (std::size_t start = 0; start < part->second.size();) {
					auto end = part->second.find(", ", start);
					if (end == std::string::npos) {
						end = part->second.size();
					}
					const auto one = part->second.substr(start, end - start);
					if (p.quests.find(one) == std::string::npos) {
						AddQuestName(p.partOf, one);
					}
					start = end + 2;
				}
				if (!p.partOf.empty()) {
					p.roles |= kQuestPart;
				}
			}
			if (auto cls = base->npcClass; cls && cls->data.maximumTrainingLevel > 0) {
				p.roles |= kTrainer;
				p.trainMax = cls->data.maximumTrainingLevel;
				p.trains = SkillName(static_cast<int>(cls->data.teaches.get()));
			}
			for (auto& [faction, rank] : FactionRanks(actor)) {
				if (rank < 0) {
					continue;
				}
				if (faction == factions.follower) {
					p.roles |= kFollower;
				} else if (faction == factions.canFollow) {
					p.roles |= kCanFollow;
				} else if (faction == factions.canMarry) {
					p.roles |= kCanMarry;
				} else if (faction == factions.innkeeper) {
					p.roles |= kInnkeeper;
				}
				if (faction->IsVendor()) {
					p.roles |= kMerchant;
					const auto& vendor = faction->vendorData;
					if (vendor.vendorValues.buysStolen) {
						p.roles |= kFence;
					}
					if (p.sells.empty() || p.sells == "unknown") {
						p.sells = Sells(faction);
						p.openFrom = vendor.vendorValues.startHour;
						p.openTo = vendor.vendorValues.endHour;
					}
					if (auto chest = vendor.merchantContainer) {
						if (std::find(p.chests.begin(), p.chests.end(), chest->GetFormID()) == p.chests.end()) {
							p.chests.push_back(chest->GetFormID());
						}
					}
				}
			}
			p.where = Where(actor);
			p.relation = Relation(actor, player);
			{
				auto here = player->GetParentCell();
				auto there = actor->GetParentCell();
				const bool inside = here && here->IsInteriorCell();
				p.nearby = inside ? (there == here) : (there && !there->IsInteriorCell() && actor->GetWorldspace() == player->GetWorldspace());
				p.loaded = actor->Is3DLoaded();
				if (p.nearby) {
					p.meters = player->GetPosition().GetDistance(actor->GetPosition()) / 70.0f;
				}
			}

			if (p.roles & kQuest) {
				p.tag = "quest: " + p.quests;
			} else if (p.roles & kQuestPart) {
				p.tag = "in quest: " + p.partOf;
			} else if (p.roles & kTrainer) {
				p.tag = std::string(p.trains) + " " + std::to_string(p.trainMax);
			} else if (p.roles & kInnkeeper) {
				p.tag = "inn";
			} else if (p.roles & kFence) {
				p.tag = "fence";
			} else if (p.roles & kMerchant) {
				p.tag = p.sells;
			} else if (p.roles & kFollower) {
				p.tag = "following you";
			}

			const auto baseID = base->IsUnique() ? base->GetFormID() : actor->GetFormID();
			if (auto it = byBase.find(baseID); it != byBase.end()) {
				auto& old = (*people)[it->second];
				if ((old.roles & kDead) && !(p.roles & kDead)) {
					old = std::move(p);
				}
				continue;
			}
			byBase.emplace(baseID, people->size());
			people->push_back(std::move(p));
		}

		std::sort(people->begin(), people->end(), [](const Person& a, const Person& b) {
			return a.key != b.key ? a.key < b.key : a.ref < b.ref;
		});

		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		const auto count = people->size();
		std::size_t targets = 0;
		std::size_t parts = 0;
		for (const auto& p : *people) {
			targets += (p.roles & kQuest) ? 1 : 0;
			parts += (p.roles & kQuestPart) ? 1 : 0;
		}
		{
			std::scoped_lock l{ g_lock };
			g_people = std::move(people);
		}
		g_builtAt = NowMs();
		logs::info("List: {} people out of {} actors ({} quest targets, {} more in quests), {} ms", count, actors.size(), targets, parts, ms);
		SetStatus(std::to_string(count) + " people: the ones you have talked to and the ones in your quests");
	}

	// ------------------------------------------------------------------ item effects

	std::string Number(float a_value)
	{
		const float rounded = std::round(a_value);
		char        buffer[32];
		if (std::fabs(a_value - rounded) < 0.05f) {
			std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(rounded));
		} else {
			std::snprintf(buffer, sizeof(buffer), "%.1f", a_value);
		}
		return buffer;
	}

	void AppendEffects(std::string& a_out, const RE::MagicItem* a_magic)
	{
		if (!a_magic) {
			return;
		}
		using Flag = RE::EffectSetting::EffectSettingData::Flag;
		for (auto effect : a_magic->effects) {
			if (!effect || !effect->baseEffect) {
				continue;
			}
			const auto base = effect->baseEffect;
			if (base->data.flags.any(Flag::kHideInUI)) {
				continue;
			}
			auto text = ToUtf8(base->GetFullName());
			if (text.empty()) {
				continue;
			}
			const float magnitude = effect->effectItem.magnitude;
			if (magnitude > 0.0f && base->data.flags.none(Flag::kNoMagnitude)) {
				text += " " + Number(magnitude);
			}
			const auto duration = effect->effectItem.duration;
			if (duration > 0 && base->data.flags.none(Flag::kNoDuration)) {
				text += " (" + std::to_string(duration) + " s)";
			}
			if (a_out.find(text) != std::string::npos) {
				continue;
			}
			a_out += a_out.empty() ? text : ", " + text;
		}
	}

	// Enchantments, potions and scrolls. Ingredients are skipped: their effects are for the player to
	// discover in the game, and the list must not give them away.
	std::string ItemEffects(RE::TESBoundObject* a_object, const RE::InventoryEntryData* a_entry)
	{
		std::string out;
		if (!a_object || a_object->Is(RE::FormType::Ingredient)) {
			return out;
		}
		if (auto enchantable = a_object->As<RE::TESEnchantableForm>(); enchantable && enchantable->formEnchanting) {
			AppendEffects(out, enchantable->formEnchanting);
		}
		if (a_object->Is(RE::FormType::AlchemyItem) || a_object->Is(RE::FormType::Scroll)) {
			AppendEffects(out, a_object->As<RE::MagicItem>());
		}
		if (a_entry && a_entry->extraLists) {
			for (auto list : *a_entry->extraLists) {
				if (!list) {
					continue;
				}
				if (auto extra = list->GetByType<RE::ExtraEnchantment>(); extra && extra->enchantment) {
					AppendEffects(out, extra->enchantment);
				}
			}
		}
		return out;
	}

	// What a list shows: armour and weapons that cannot be worn are hidden, and so are levelled lists.
	bool ShowItem(RE::TESBoundObject& a_object)
	{
		if (a_object.Is(RE::FormType::LeveledItem)) {
			return false;
		}
		if (a_object.Is(RE::FormType::Armor)) {
			return (a_object.GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
		}
		if (a_object.Is(RE::FormType::Weapon) || a_object.Is(RE::FormType::Ammo)) {
			return a_object.GetPlayable();
		}
		return true;
	}

	// Reads an inventory without filling it: a chest that has never been opened is left untouched.
	std::vector<Item> ReadItems(RE::TESObjectREFR* a_ref, int& a_gold, bool& a_known)
	{
		std::vector<Item> items;
		if (!a_ref) {
			a_known = false;
			return items;
		}
		a_known = a_known && a_ref->GetInventoryChanges(true) != nullptr;
		auto inventory = a_ref->GetInventory([](RE::TESBoundObject& a_object) { return ShowItem(a_object); }, true);
		for (auto& [object, data] : inventory) {
			const auto count = data.first;
			if (!object || count <= 0) {
				continue;
			}
			if (object->IsGold()) {
				a_gold += count;
				continue;
			}
			const char* name = object->GetName();
			if (!name || !*name) {
				continue;
			}
			Item item;
			item.name = ToUtf8(name);
			item.key = Lower(item.name);
			item.count = count;
			item.value = object->GetGoldValue();
			item.effects = ItemEffects(object, data.second.get());
			item.effectsKey = Lower(item.effects);
			items.push_back(std::move(item));
		}
		return items;
	}

	void BuildDetail(RE::FormID a_ref, bool a_wares)
	{
		auto detail = std::make_shared<Detail>();
		detail->ref = a_ref;
		detail->wares = a_wares;
		auto actor = RE::TESForm::LookupByID<RE::Actor>(a_ref);
		if (!actor) {
			detail->known = false;
		} else if (!a_wares) {
			detail->items = ReadItems(actor, detail->gold, detail->known);
		} else {
			auto people = Read(g_people);
			const Person* person = nullptr;
			if (people) {
				for (const auto& p : *people) {
					if (p.ref == a_ref) {
						person = &p;
						break;
					}
				}
			}
			if (!person || person->chests.empty()) {
				detail->known = false;
			} else {
				for (const auto chestID : person->chests) {
					auto chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(chestID);
					auto items = ReadItems(chest, detail->gold, detail->known);
					for (auto& item : items) {
						auto same = std::find_if(detail->items.begin(), detail->items.end(), [&](const Item& a) { return a.key == item.key && a.effects == item.effects; });
						if (same != detail->items.end()) {
							same->count += item.count;
						} else {
							detail->items.push_back(std::move(item));
						}
					}
				}
			}
		}
		std::sort(detail->items.begin(), detail->items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
		std::scoped_lock l{ g_lock };
		g_detail = std::move(detail);
	}

	// Searches by name and by effect (for example "alteration") in the merchants' chests and, if wanted,
	// in the inventories of people the player has talked to. Read only.
	void SearchItems(std::string a_query, bool a_stock, bool a_inventories)
	{
		auto       hits = std::make_shared<Hits>();
		const auto query = Lower(a_query);
		auto       people = Read(g_people);
		auto       add = [&](const Person& a_person, bool a_isStock, RE::TESObjectREFR* a_ref) {
			int  gold = 0;
			bool known = true;
			for (auto& item : ReadItems(a_ref, gold, known)) {
				if (hits->size() >= 1000) {
					return;
				}
				if (item.key.find(query) == std::string::npos && item.effectsKey.find(query) == std::string::npos) {
					continue;
				}
				Hit hit;
				hit.ref = a_person.ref;
				hit.person = a_person.name;
				hit.roles = a_person.roles;
				hit.stock = a_isStock;
				hit.item = std::move(item.name);
				hit.effects = std::move(item.effects);
				hit.count = item.count;
				hit.value = item.value;
				hits->push_back(std::move(hit));
			}
		};
		if (people && !query.empty()) {
			for (const auto& p : *people) {
				if (a_stock) {
					for (const auto chestID : p.chests) {
						add(p, true, RE::TESForm::LookupByID<RE::TESObjectREFR>(chestID));
					}
				}
				if (a_inventories) {
					add(p, false, RE::TESForm::LookupByID<RE::TESObjectREFR>(p.ref));
				}
			}
		}
		std::sort(hits->begin(), hits->end(), [](const Hit& a, const Hit& b) {
			return a.item != b.item ? a.item < b.item : a.person < b.person;
		});
		logs::info("Item search '{}': {} results", a_query, hits->size());
		std::scoped_lock l{ g_lock };
		g_hits = std::move(hits);
		g_hitsFor = std::move(a_query);
	}

	// ------------------------------------------------------------------ tracking (main thread)

	RE::TESQuest* TrackQuest()
	{
		static RE::TESQuest* quest = nullptr;
		if (!quest) {
			if (auto dh = RE::TESDataHandler::GetSingleton()) {
				quest = dh->LookupForm<RE::TESQuest>(0x800, "CouriersLedger.esp");
			}
			if (!quest) {
				quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("CL_TrackQuest");
			}
		}
		return quest;
	}

	// Calls one of the quest's Papyrus functions (Quest.SetObjectiveDisplayed and Quest.SetActive).
	template <class... Args>
	bool CallQuest(RE::TESQuest* a_quest, const char* a_function, Args... a_args)
	{
		auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
		if (!policy) {
			return false;
		}
		const auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, a_quest);
		if (handle == policy->EmptyHandle()) {
			logs::warn("No handle for the quest");
			return false;
		}
		RE::BSTSmartPointer<RE::BSScript::Object> object;
		if (!vm->FindBoundObject(handle, "Quest", object) || !object) {
			if (!vm->CreateObject("Quest", object) || !object) {
				logs::warn("Could not create a Quest object");
				return false;
			}
			auto bind = vm->GetObjectBindPolicy();
			if (!bind) {
				return false;
			}
			bind->BindObject(object, handle);
			logs::info("Quest bound to the Quest script");
		}
		auto args = RE::MakeFunctionArguments(std::move(a_args)...);
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
		const bool sent = vm->DispatchMethodCall(handle, "Quest", a_function, args, callback);
		logs::info("Quest.{}: {}", a_function, sent ? "sent" : "FAILED");
		return sent;
	}

	bool Transitional(const RE::TESQuest* a_quest)
	{
		return a_quest->data.flags.underlying() == 0xFFFF;
	}

	// Main thread. Switches off the markers of every other active quest and remembers which.
	void HideOtherMarkers(RE::TESQuest* a_ours)
	{
		auto dh = RE::TESDataHandler::GetSingleton();
		if (!dh || !g_focus.load()) {
			return;
		}
		int count = 0;
		for (auto quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest || quest == a_ours || Transitional(quest) || !quest->IsActive()) {
				continue;
			}
			quest->data.flags.reset(RE::QuestFlag::kActive);
			g_hidden.push_back(quest->GetFormID());
			++count;
		}
		g_hiddenCount = static_cast<int>(g_hidden.size());
		logs::info("Hid the markers of {} other quests ({} in total)", count, g_hidden.size());
	}

	// Main thread. Switches the markers back on for the quests that are still running.
	void RestoreOtherMarkers()
	{
		int count = 0;
		for (const auto id : g_hidden) {
			auto quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
			if (quest && !Transitional(quest) && quest->IsEnabled() && !quest->IsCompleted()) {
				quest->data.flags.set(RE::QuestFlag::kActive);
				++count;
			}
		}
		if (!g_hidden.empty()) {
			logs::info("Showed the markers of {} other quests again", count);
		}
		g_hidden.clear();
		g_hiddenCount = 0;
	}

	// CommonLib's IsRunning() is also true for a quest that has never started, so the quest's own flags
	// are read here.
	bool QuestUp(const RE::TESQuest* a_quest)
	{
		return a_quest->IsEnabled() && !a_quest->IsStarting() && !a_quest->IsStopping();
	}

	void LogQuest(RE::TESQuest* a_quest, const char* a_when)
	{
		const auto target = a_quest->GetAliasedRef(0).get();
		int        state = -1;
		for (auto objective : a_quest->objectives) {
			if (objective && objective->index == 10) {
				state = static_cast<int>(objective->state.underlying());
			}
		}
		logs::info("{}: enabled {} starting {} stopping {} active {} stage {} alias {:08X} objective state {}",
			a_when, a_quest->IsEnabled(), a_quest->IsStarting(), a_quest->IsStopping(), a_quest->IsActive(),
			a_quest->GetCurrentStageID(), target ? target->GetFormID() : 0, state);
	}

	// Main thread. Returns true when tracking is done or has given up.
	bool TryTrack(RE::FormID a_ref)
	{
		auto quest = TrackQuest();
		if (!quest) {
			SetStatus("CouriersLedger.esp is missing or not enabled.");
			return true;
		}
		auto actor = RE::TESForm::LookupByID<RE::Actor>(a_ref);
		if (!actor) {
			SetStatus("That person no longer exists.");
			return true;
		}
		if (quest->IsStopping() || quest->IsStarting()) {
			return false;
		}
		if (!quest->IsEnabled()) {
			const bool started = quest->Start();
			logs::info("Starting the tracking quest: {}", started);
			if (!QuestUp(quest)) {
				return false;
			}
		}
		quest->ForceRefIntoAlias(0, actor);
		HideOtherMarkers(quest);
		CallQuest(quest, "SetObjectiveDisplayed", static_cast<std::int32_t>(10), false, false);
		CallQuest(quest, "SetObjectiveDisplayed", static_cast<std::int32_t>(10), true, true);
		CallQuest(quest, "SetActive", true);
		LogQuest(quest, "Tracking");
		g_tracked = a_ref;
		{
			std::scoped_lock l{ g_lock };
			g_trackedName = ToUtf8(actor->GetActorBase() ? actor->GetActorBase()->fullName.c_str() : "");
		}
		SetStatus("Tracking " + ReadString(g_trackedName) + ". The marker shows on the map and the compass.");
		return true;
	}

	void RequestTrack(RE::FormID a_ref)
	{
		auto done = std::make_shared<std::atomic<bool>>(false);
		auto attempt = [a_ref, done]() {
			if (!done->load() && TryTrack(a_ref)) {
				done->store(true);
			}
		};
		SKSE::GetTaskInterface()->AddTask(attempt);
		// A quest often starts a few frames later. Try again for a while, then write the quest's state to
		// the log once the Papyrus calls have run.
		std::thread([attempt, done]() {
			for (int i = 0; i < 50 && !done->load(); ++i) {
				std::this_thread::sleep_for(100ms);
				if (!done->load()) {
					SKSE::GetTaskInterface()->AddTask(attempt);
				}
			}
			std::this_thread::sleep_for(200ms);
			if (!done->load()) {
				done->store(true);
				SetStatus("The tracking quest did not start. See CouriersLedger.log.");
				return;
			}
			std::this_thread::sleep_for(1500ms);
			SKSE::GetTaskInterface()->AddTask([]() {
				if (auto quest = TrackQuest()) {
					LogQuest(quest, "A moment later");
				}
			});
		}).detach();
	}

	void Untrack()
	{
		auto quest = TrackQuest();
		RestoreOtherMarkers();
		if (quest && quest->IsEnabled()) {
			CallQuest(quest, "SetObjectiveDisplayed", static_cast<std::int32_t>(10), false, false);
			CallQuest(quest, "SetActive", false);
			quest->Stop();
		}
		g_tracked = 0;
		{
			std::scoped_lock l{ g_lock };
			g_trackedName.clear();
		}
		SetStatus("Nobody is tracked.");
	}

	// After a load: who the quest tracks in this save.
	void ReadTrackedFromSave()
	{
		g_tracked = 0;
		std::string name;
		auto quest = TrackQuest();
		if (quest && quest->IsEnabled()) {
			auto handle = quest->GetAliasedRef(0);
			if (auto ref = handle.get()) {
				g_tracked = ref->GetFormID();
				if (auto actor = ref->As<RE::Actor>(); actor && actor->GetActorBase()) {
					name = ToUtf8(actor->GetActorBase()->fullName.c_str());
				}
			}
		}
		std::scoped_lock l{ g_lock };
		g_trackedName = std::move(name);
		g_people.reset();
		g_detail.reset();
		g_hits.reset();
		g_status = "Refreshes when you open the list.";
	}

	// ------------------------------------------------------------------ requests from the menu

	void RequestPeople()
	{
		if (g_busy.exchange(true)) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([]() {
			BuildPeople();
			g_busy = false;
		});
	}

	void RequestDetail(RE::FormID a_ref, bool a_wares)
	{
		SKSE::GetTaskInterface()->AddTask([a_ref, a_wares]() { BuildDetail(a_ref, a_wares); });
	}

	void RequestItems(std::string a_query, bool a_stock, bool a_inventories)
	{
		SKSE::GetTaskInterface()->AddTask([query = std::move(a_query), a_stock, a_inventories]() { SearchItems(query, a_stock, a_inventories); });
	}

	void RequestUntrack()
	{
		SKSE::GetTaskInterface()->AddTask([]() { Untrack(); });
	}

	// Focus is switched on or off while someone is tracked.
	void RequestFocus(bool a_on)
	{
		g_focus = a_on;
		SKSE::GetTaskInterface()->AddTask([a_on]() {
			auto quest = TrackQuest();
			if (!quest || !quest->IsEnabled() || g_tracked.load() == 0) {
				return;
			}
			if (a_on) {
				HideOtherMarkers(quest);
			} else {
				RestoreOtherMarkers();
			}
			CallQuest(quest, "SetActive", true);
		});
	}

	// ------------------------------------------------------------------ SKSE co-save

	constexpr std::uint32_t kSaveID = 0x434C4447;    // CLDG
	constexpr std::uint32_t kHiddenRecord = 0x48494445;  // HIDE

	void OnGameSave(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kHiddenRecord, 1)) {
			return;
		}
		const auto count = static_cast<std::uint32_t>(g_hidden.size());
		a_intfc->WriteRecordData(count);
		for (const auto id : g_hidden) {
			a_intfc->WriteRecordData(id);
		}
	}

	void OnGameLoad(SKSE::SerializationInterface* a_intfc)
	{
		g_hidden.clear();
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kHiddenRecord) {
				continue;
			}
			std::uint32_t count = 0;
			a_intfc->ReadRecordData(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID id = 0;
				RE::FormID resolved = 0;
				a_intfc->ReadRecordData(id);
				if (a_intfc->ResolveFormID(id, resolved)) {
					g_hidden.push_back(resolved);
				}
			}
		}
		g_hiddenCount = static_cast<int>(g_hidden.size());
		logs::info("Loaded: {} quests with hidden markers", g_hidden.size());
	}

	void OnGameRevert(SKSE::SerializationInterface*)
	{
		g_hidden.clear();
		g_hiddenCount = 0;
	}

	// ------------------------------------------------------------------ the menu

	constexpr ui::ImVec4 kWhite{ 0.92f, 0.92f, 0.92f, 1.0f };
	constexpr ui::ImVec4 kGrey{ 0.55f, 0.55f, 0.55f, 1.0f };
	constexpr ui::ImVec4 kBlue{ 0.45f, 0.75f, 1.0f, 1.0f };
	constexpr ui::ImVec4 kGold{ 1.0f, 0.82f, 0.35f, 1.0f };
	constexpr ui::ImVec4 kPurple{ 0.82f, 0.6f, 1.0f, 1.0f };
	constexpr ui::ImVec4 kOrange{ 1.0f, 0.6f, 0.3f, 1.0f };
	constexpr ui::ImVec4 kGreen{ 0.5f, 0.9f, 0.5f, 1.0f };
	constexpr ui::ImVec4 kPink{ 1.0f, 0.55f, 0.75f, 1.0f };
	constexpr ui::ImVec4 kYellow{ 1.0f, 0.95f, 0.4f, 1.0f };

	ui::ImVec4 ColorFor(std::uint32_t a_roles)
	{
		if (a_roles & kDead) {
			return kGrey;
		}
		if (a_roles & (kQuest | kQuestPart)) {
			return kYellow;
		}
		if (a_roles & kTrainer) {
			return kBlue;
		}
		if (a_roles & kInnkeeper) {
			return kOrange;
		}
		if (a_roles & kFence) {
			return kPurple;
		}
		if (a_roles & kMerchant) {
			return kGold;
		}
		if (a_roles & (kFollower | kCanFollow)) {
			return kGreen;
		}
		if (a_roles & kCanMarry) {
			return kPink;
		}
		return kWhite;
	}

	std::string RoleText(const Person& a_person)
	{
		std::vector<std::string> parts;
		if (a_person.roles & kQuest) {
			parts.emplace_back("quest target");
		} else if (a_person.roles & kQuestPart) {
			parts.emplace_back("in a quest");
		}
		if (a_person.roles & kTrainer) {
			parts.emplace_back("trainer");
		}
		if (a_person.roles & kInnkeeper) {
			parts.emplace_back("innkeeper");
		}
		if (a_person.roles & kFence) {
			parts.emplace_back("fence (buys stolen goods)");
		} else if (a_person.roles & kMerchant) {
			parts.emplace_back("merchant");
		}
		if (a_person.roles & kFollower) {
			parts.emplace_back("following you");
		} else if (a_person.roles & kCanFollow) {
			parts.emplace_back("can follow you");
		}
		if (a_person.roles & kCanMarry) {
			parts.emplace_back("can marry");
		}
		if (a_person.roles & kDead) {
			parts.emplace_back("dead");
		}
		std::string out;
		for (const auto& part : parts) {
			out += out.empty() ? part : " · " + part;
		}
		return out;
	}

	struct Chip
	{
		const char*   label;
		std::uint32_t mask;
		ui::ImVec4    color;
		bool          on;
	};

	std::array<Chip, 7> g_chips{ {
		{ "Quests", kQuest | kQuestPart, kYellow, true },
		{ "Trainers", kTrainer, kBlue, true },
		{ "Merchants", kMerchant | kFence, kGold, true },
		{ "Inns", kInnkeeper, kOrange, true },
		{ "Followers", kFollower | kCanFollow, kGreen, true },
		{ "Marriage", kCanMarry, kPink, true },
		{ "Others", 0, kWhite, true },
	} };
	bool       g_showDead = false;
	int        g_tab = 0;
	char       g_nameBuf[96] = {};
	char       g_waresBuf[96] = {};
	bool       g_searchStock = true;
	bool       g_searchInventories = true;
	RE::FormID g_selected = 0;

	bool Visible(const Person& a_person, const std::string& a_query)
	{
		if ((a_person.roles & kDead) && !g_showDead) {
			return false;
		}
		if (g_tab != 0 && a_person.group != g_tab) {
			return false;
		}
		if (!a_query.empty() && a_person.key.find(a_query) == std::string::npos) {
			return false;
		}
		const auto roles = a_person.roles & ~kDead;
		for (const auto& chip : g_chips) {
			if (chip.mask == 0) {
				if (roles == 0 && chip.on) {
					return true;
				}
			} else if ((roles & chip.mask) && chip.on) {
				return true;
			}
		}
		return false;
	}

	void TrackedLine()
	{
		const auto tracked = g_tracked.load();
		if (!tracked) {
			return;
		}
		ui::TextColored(kGreen, "Tracking: %s", ReadString(g_trackedName).c_str());
		ui::SameLine();
		if (ui::SmallButton("Stop tracking##line")) {
			RequestUntrack();
		}
		if (const int hidden = g_hiddenCount.load(); hidden > 0) {
			ui::SameLine();
			ui::TextDisabled("(%d other quest markers hidden until you stop tracking)", hidden);
		}
	}

	void ItemTable(const Detail& a_detail)
	{
		if (a_detail.wares) {
			ui::Text("Gold for trading: %d", a_detail.gold);
			if (!a_detail.known) {
				ui::TextWrapped("The merchant chest has not been stocked yet, so this may be incomplete. The full stock shows once you have traded here.");
			}
		} else if (a_detail.gold > 0) {
			ui::Text("Gold: %d", a_detail.gold);
		}
		if (a_detail.items.empty()) {
			ui::TextDisabled(a_detail.wares ? "No stock found." : "Nothing to show.");
			return;
		}
		const auto avail = ui::GetContentRegionAvail();
		const int  flags = ui::ImGuiTableFlags_RowBg | ui::ImGuiTableFlags_ScrollY | ui::ImGuiTableFlags_BordersOuter | ui::ImGuiTableFlags_BordersInnerV;
		if (ui::BeginTable("##saker", 4, flags, ui::ImVec2{ 0.0f, (std::max)(120.0f, avail.y) })) {
			ui::TableSetupColumn("Item", ui::ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ui::TableSetupColumn("Effects", ui::ImGuiTableColumnFlags_WidthStretch, 1.3f);
			ui::TableSetupColumn("Count", ui::ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ui::TableSetupColumn("Value", ui::ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ui::TableHeadersRow();
			for (const auto& item : a_detail.items) {
				ui::TableNextRow();
				ui::TableSetColumnIndex(0);
				ui::TextUnformatted(item.name.c_str());
				ui::TableSetColumnIndex(1);
				ui::TextWrapped("%s", item.effects.c_str());
				ui::TableSetColumnIndex(2);
				ui::Text("%d", item.count);
				ui::TableSetColumnIndex(3);
				ui::Text("%d", item.value);
			}
			ui::EndTable();
		}
	}

	void Details(const Person& a_person)
	{
		ui::TextColored(ColorFor(a_person.roles), "%s", a_person.name.c_str());
		const auto roles = RoleText(a_person);
		if (!roles.empty()) {
			ui::TextDisabled("%s", roles.c_str());
		}
		ui::Spacing();
		if (a_person.roles & kQuest) {
			ui::TextWrapped("Quest target: %s", a_person.quests.c_str());
		}
		if (a_person.roles & kQuestPart) {
			ui::TextWrapped("In quest: %s", a_person.partOf.c_str());
		}
		ui::TextWrapped("Where: %s", a_person.where.c_str());
		if (a_person.nearby) {
			ui::Text("Near you: about %d m away%s", static_cast<int>(a_person.meters), a_person.loaded ? "" : " (not drawn in the world right now)");
		}
		if (a_person.roles & kTrainer) {
			ui::Text("Trains %s up to %d (%s)", a_person.trains.c_str(), a_person.trainMax, TrainerLevel(a_person.trainMax));
		}
		if (a_person.roles & kMerchant) {
			ui::TextWrapped("Trades in: %s", a_person.sells.c_str());
			if (a_person.openFrom != a_person.openTo) {
				ui::Text("Trades %02d:00-%02d:00", a_person.openFrom, a_person.openTo);
			}
		}
		if (a_person.relation != 0) {
			ui::Text("Relationship with you: %s", RelationName(a_person.relation));
		}
		ui::Spacing();

		const bool tracked = g_tracked.load() == a_person.ref;
		if (tracked) {
			if (ui::Button("Stop tracking")) {
				RequestUntrack();
			}
		} else if (ui::Button("Track on map")) {
			RequestTrack(a_person.ref);
		}
		ui::SameLine();
		static bool focus = true;
		focus = g_focus.load();
		if (ui::Checkbox("Hide other quest markers", &focus)) {
			RequestFocus(focus);
		}
		if (ui::Button("Inventory")) {
			RequestDetail(a_person.ref, false);
		}
		if (a_person.roles & kMerchant) {
			ui::SameLine();
			if (ui::Button("Stock")) {
				RequestDetail(a_person.ref, true);
			}
		}
		ui::Separator();
		if (auto detail = Read(g_detail); detail && detail->ref == a_person.ref) {
			ui::TextDisabled(detail->wares ? "Stock (look only)" : "Inventory (look only)");
			ItemTable(*detail);
		}
	}

	void __stdcall RenderPeople()
	{
		auto people = Read(g_people);
		if ((!people || NowMs() - g_builtAt.load() > 30000) && !g_busy.load()) {
			RequestPeople();
		}

		ui::SetNextItemWidth(260.0f);
		ui::InputTextWithHint("##name", "Search name", g_nameBuf, sizeof(g_nameBuf));
		ui::SameLine();
		if (ui::Button(g_busy.load() ? "Refreshing..." : "Refresh")) {
			RequestPeople();
		}
		ui::SameLine();
		ui::TextDisabled("%s", ReadString(g_status).c_str());
		TrackedLine();

		for (auto& chip : g_chips) {
			ui::PushStyleColor(ui::ImGuiCol_Text, chip.color);
			ui::Checkbox(chip.label, &chip.on);
			ui::PopStyleColor();
			ui::SameLine();
		}
		ui::PushStyleColor(ui::ImGuiCol_Text, kGrey);
		ui::Checkbox("Dead", &g_showDead);
		ui::PopStyleColor();

		if (ui::BeginTabBar("##bokstaver")) {
			static constexpr const char* tabs[5] = { "All", "A-F", "G-L", "M-R", "S-Z" };
			for (int i = 0; i < 5; ++i) {
				if (ui::BeginTabItem(tabs[i])) {
					g_tab = i;
					ui::EndTabItem();
				}
			}
			ui::EndTabBar();
		}

		const auto query = Lower(g_nameBuf);
		const auto avail = ui::GetContentRegionAvail();
		const float listWidth = (std::max)(240.0f, avail.x * 0.36f);
		const Person* selected = nullptr;

		ui::BeginChild("##lista", ui::ImVec2{ listWidth, 0.0f }, ui::ImGuiChildFlags_Border);
		if (people) {
			int shown = 0;
			for (const auto& p : *people) {
				if (p.ref == g_selected) {
					selected = &p;
				}
				if (!Visible(p, query)) {
					continue;
				}
				++shown;
				ui::PushID(static_cast<int>(p.ref));
				ui::PushStyleColor(ui::ImGuiCol_Text, ColorFor(p.roles));
				if (ui::Selectable(p.name.c_str(), p.ref == g_selected)) {
					g_selected = p.ref;
					selected = &p;
				}
				ui::PopStyleColor();
				if (!p.tag.empty()) {
					ui::SameLine();
					ui::TextDisabled("%s", p.tag.c_str());
				}
				ui::PopID();
			}
			if (shown == 0) {
				ui::TextDisabled("Nobody matches the filter.");
			}
		} else {
			ui::TextDisabled("Loading...");
		}
		ui::EndChild();

		ui::SameLine();
		ui::BeginChild("##detaljer", ui::ImVec2{ 0.0f, 0.0f }, ui::ImGuiChildFlags_Border);
		if (selected) {
			Details(*selected);
		} else {
			ui::TextWrapped("Pick a person in the list. The colour shows the role: yellow in a quest, blue trainer, gold merchant, purple fence, orange innkeeper, green follower, pink can marry, grey dead.");
		}
		ui::EndChild();
	}

	void __stdcall RenderWares()
	{
		auto people = Read(g_people);
		if (!people && !g_busy.load()) {
			RequestPeople();
		}
		ui::TextWrapped("Search by item name or by effect, for example \"robe\", \"alteration\" or \"fortify smithing\". Only people in the ledger: the ones you have talked to and the ones in your quests. A merchant's stock is what was in their chest when the game last restocked it.");
		ui::SetNextItemWidth(300.0f);
		const bool enter = ui::InputTextWithHint("##item", "Item or effect", g_waresBuf, sizeof(g_waresBuf), ui::ImGuiInputTextFlags_EnterReturnsTrue);
		ui::SameLine();
		const bool clicked = ui::Button("Search");
		ui::SameLine();
		ui::Checkbox("Merchants' stock", &g_searchStock);
		ui::SameLine();
		ui::Checkbox("People's inventories", &g_searchInventories);
		if ((clicked || enter) && g_waresBuf[0] && (g_searchStock || g_searchInventories)) {
			RequestItems(g_waresBuf, g_searchStock, g_searchInventories);
		}
		TrackedLine();
		ui::Separator();

		auto hits = Read(g_hits);
		if (!hits) {
			return;
		}
		const auto query = ReadString(g_hitsFor);
		ui::Text("%d results for \"%s\"%s", static_cast<int>(hits->size()), query.c_str(), hits->size() >= 1000 ? " (the first 1000)" : "");
		if (hits->empty()) {
			return;
		}
		const auto avail = ui::GetContentRegionAvail();
		const int  flags = ui::ImGuiTableFlags_RowBg | ui::ImGuiTableFlags_ScrollY | ui::ImGuiTableFlags_BordersOuter | ui::ImGuiTableFlags_BordersInnerV;
		if (ui::BeginTable("##traffar", 7, flags, ui::ImVec2{ 0.0f, (std::max)(150.0f, avail.y) })) {
			ui::TableSetupColumn("Item", ui::ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ui::TableSetupColumn("Effects", ui::ImGuiTableColumnFlags_WidthStretch, 1.3f);
			ui::TableSetupColumn("Who", ui::ImGuiTableColumnFlags_WidthStretch, 0.8f);
			ui::TableSetupColumn("Where", ui::ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ui::TableSetupColumn("Count", ui::ImGuiTableColumnFlags_WidthFixed, 50.0f);
			ui::TableSetupColumn("Value", ui::ImGuiTableColumnFlags_WidthFixed, 55.0f);
			ui::TableSetupColumn("", ui::ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ui::TableHeadersRow();
			int row = 0;
			for (const auto& hit : *hits) {
				ui::PushID(row++);
				ui::TableNextRow();
				ui::TableSetColumnIndex(0);
				ui::TextUnformatted(hit.item.c_str());
				ui::TableSetColumnIndex(1);
				ui::TextWrapped("%s", hit.effects.c_str());
				ui::TableSetColumnIndex(2);
				ui::TextColored(ColorFor(hit.roles), "%s", hit.person.c_str());
				ui::TableSetColumnIndex(3);
				ui::TextDisabled("%s", hit.stock ? "for sale" : "carried");
				ui::TableSetColumnIndex(4);
				ui::Text("%d", hit.count);
				ui::TableSetColumnIndex(5);
				ui::Text("%d", hit.value);
				ui::TableSetColumnIndex(6);
				if (g_tracked.load() == hit.ref) {
					ui::TextColored(kGreen, "tracked");
				} else if (ui::SmallButton("Track")) {
					RequestTrack(hit.ref);
				}
				ui::PopID();
			}
			ui::EndTable();
		}
	}

	void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_type)
	{
		if (a_type == SKSEMenuFramework::Model::EventType::kOpenMenu) {
			RequestPeople();
		}
	}

	void RegisterMenu()
	{
		if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule()) {
			logs::error("SKSE Menu Framework is missing, the list cannot be shown");
			return;
		}
		logs::info("SKSE Menu Framework {}, API {}", SKSEMenuFramework::GetMenuFrameworkVersion(), SKSEMenuFramework::GetMenuFrameworkAPIVersion());
		SKSEMenuFramework::SetSection("The Courier's Ledger");
		SKSEMenuFramework::AddSectionItem("People", RenderPeople);
		SKSEMenuFramework::AddSectionItem("Find Item", RenderWares);
		SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
		logs::info("Menu registered");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// The menu framework may load after us (plugins load in alphabetical order), so registration waits until here.
			RegisterMenu();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			ReadTrackedFromSave();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logs::info("The Courier's Ledger 1.0.0");
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	if (auto serialization = SKSE::GetSerializationInterface()) {
		serialization->SetUniqueID(kSaveID);
		serialization->SetSaveCallback(OnGameSave);
		serialization->SetLoadCallback(OnGameLoad);
		serialization->SetRevertCallback(OnGameRevert);
	}
	return true;
}
