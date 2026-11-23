#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" int random_leveling_probe_byte(const void* address);

namespace {
using Address = std::uintptr_t;
using Key = std::uint64_t;

// RVAs verified against the Steam 1.17.2 Worldwide executable (2.7.2.0).
constexpr Address WorldChrMan = 0x3d6d018;
constexpr Address GameDataMan = 0x3d64fb8;
constexpr Address GameMan = 0x3d709a8;
constexpr Address TaskManager = 0x4592ea8;
constexpr Address TaskGroupManager = 0x4592f28;
constexpr Address RegisterTask = 0xeb5540;
constexpr Address ForceDeath = 0x5eea20;
// ChrIns death rune award (float (ChrIns*)). Every machine in a co-op session
// runs it for its own copy of an enemy that dies and gives the runes to its
// local player; that is how the game shares runes. Bit 2 of +0x1c6 marks a
// death whose runes were already given.
constexpr Address RuneAward = 0x3fe030;
constexpr std::ptrdiff_t DeathFlags = 0x1c6;
constexpr std::array<std::uint8_t, 17> RuneAwardPrologue{
    0x40,0x53, 0x48,0x83,0xec,0x40, 0x48,0x8b,0x01, 0x48,0x8b,0xd9, 0x0f,0x29,0x74,0x24,0x30};
constexpr Address CanTarget = 0x51b8b0;
constexpr Address WorldVtable = 0x2a509d0;
constexpr Address EnemyVtable = 0x2a49090;
constexpr Address PlayerVtable = 0x2a81cb0;
constexpr Address DataVtable = 0x2a3a0c8;
constexpr Address TaskVtable = 0x2c06db0;

constexpr std::ptrdiff_t LocalPlayer = 0x1e508;
// 1.17.2 indexes this vector using the pointers at 0x1f1e8 and 0x1f1f0.
constexpr std::ptrdiff_t ByDistance = 0x1f1e0;
constexpr std::ptrdiff_t CharacterSets = 0x1ded8;
constexpr unsigned CharacterSetCount = 196;
constexpr unsigned MapSetsPerUpdate = 12;
constexpr unsigned MapEntriesPerUpdate = 384;
constexpr std::ptrdiff_t Modules = 0x190;
constexpr std::ptrdiff_t Health = 0x138;
constexpr std::ptrdiff_t MaxHealth = 0x13c;
constexpr std::ptrdiff_t UncappedHealth = 0x140;
constexpr std::ptrdiff_t BaseHealth = 0x144;
constexpr std::ptrdiff_t Flags = 0x1c5;
constexpr std::ptrdiff_t Team = 0x6c;
constexpr std::ptrdiff_t Handle = 0x8;
constexpr std::ptrdiff_t LastHitBy = 0x180;
constexpr std::ptrdiff_t StatStart = 0x3c;
constexpr std::ptrdiff_t Level = 0x68;

constexpr Address EventFlagMan = 0x3d6f4d8; // CSEventFlagMan, from "48 8B 3D ? ? ? ? 48 85 FF 74 ? 48 8B 49".
constexpr std::ptrdiff_t PlayerChrSet = 0x10ee0;
constexpr unsigned PlayerSetLimit = 64;
constexpr unsigned OldItemFlagGroup = 1032502;
constexpr unsigned ItemFlagGroups[] = {
#include "item_flag_groups.inc"
};

constexpr std::ptrdiff_t NpcParamId = 0x60;
constexpr std::ptrdiff_t Runes = 0x6c;
// Level curve: an ordinary or elite enemy is 1 point and a boss is 20. A level
// costs 5 points up to level 10, then 1 more point every 5 levels
// (level 20: 7, 60: 15, 100: 23, 150: 33, 300: 63). Random attributes are
// often not the ones a build needs, so the curve allows high levels.
// Simulated base-game results: story run ~190, thorough run ~275, every enemy ~345.
// Values players can change in random-leveling.ini next to the profile.
// The defaults are the designed rules above.
struct Settings {
    unsigned enemy_points = 1;
    unsigned boss_points = 20;
    unsigned wildlife_points = 0;
    unsigned level_cost = 5;           // Points per level at the start.
    unsigned flat_cost_until = 10;     // The cost starts rising after this level.
    unsigned cost_step_levels = 5;     // +1 point every this many levels (0 = never).
    unsigned max_level_cost = 0;       // Upper limit of the cost (0 = none).
    unsigned death_health_percent = 5; // Enemy health added per death or team wipe.
    bool undo_rune_levels = true;
    bool enemies_stay_dead = true;
    bool team_wipe_only = true;
    bool rune_hook = true;
};
Settings settings;

unsigned level_cost(unsigned level) {
    unsigned cost = settings.level_cost;
    if (settings.cost_step_levels && level > settings.flat_cost_until)
        cost += (level - settings.flat_cost_until) / settings.cost_step_levels;
    if (settings.max_level_cost) cost = std::min(cost, settings.max_level_cost);
    return std::max(cost, 1u);
}

// Reads `key = value` lines; `;` or `#` start a comment and [sections] are
// only for reading. Missing keys use the defaults; bad values are reported.
Settings parse_settings(const std::string& text, std::string& report) {
    Settings out;
    std::size_t start = 0;
    if (text.rfind("\xef\xbb\xbf", 0) == 0) start = 3; // UTF-8 BOM.
    const auto trim = [](const std::string& v) {
        const auto a = v.find_first_not_of(" \t\r");
        if (a == std::string::npos) return std::string{};
        return v.substr(a, v.find_last_not_of(" \t\r") - a + 1);
    };
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        line = line.substr(0, line.find_first_of(";#"));
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        char* stop{};
        const unsigned long number = std::strtoul(value.c_str(), &stop, 10);
        if (value.empty() || *stop || value[0] == '-' || number > 1000000) {
            report += " bad:" + key;
            continue;
        }
        const auto n = static_cast<unsigned>(number);
        if (key == "enemy_points") out.enemy_points = n;
        else if (key == "boss_points") out.boss_points = n;
        else if (key == "wildlife_points") out.wildlife_points = n;
        else if (key == "level_cost") out.level_cost = std::max(n, 1u);
        else if (key == "flat_cost_until") out.flat_cost_until = n;
        else if (key == "cost_step_levels") out.cost_step_levels = n;
        else if (key == "max_level_cost") out.max_level_cost = n;
        else if (key == "death_health_percent") out.death_health_percent = std::min(n, 1000u);
        else if (key == "undo_rune_levels") out.undo_rune_levels = n != 0;
        else if (key == "enemies_stay_dead") out.enemies_stay_dead = n != 0;
        else if (key == "team_wipe_only") out.team_wipe_only = n != 0;
        else if (key == "rune_hook") out.rune_hook = n != 0;
        else report += " unknown:" + key;
    }
    return out;
}

constexpr int BossNpcParams[] = {
#include "boss_npcs.inc"
};

struct Character {
    Address instance;
    Address data;
    int hp;
    int max;
    int uncapped;
    int base;
    int model;
    std::uint8_t team;
    bool dead;
    int npc_param{};
};

struct Observation {
    Address data{};
    Key handle{};
    Key placement{};
    std::uint64_t last_frame{};
    int base{};
    int mod_base{};
    int unscaled_max{};
    int mod_max{};
    int unscaled_uncapped{};
    int mod_uncapped{};
    bool seen_alive{};
    bool player_hit{};
};

struct World {
    std::filesystem::path journal;
    std::unordered_set<Key> defeated;
    std::uint32_t deaths{};
    std::uint32_t kills{};    // Credited ordinary and elite kills.
    std::uint32_t progress{}; // Points toward the next level.
    bool has_progress{};
    std::uint32_t last_game_deaths{};
    std::uint32_t last_playtime{};
    int slot{-1};
    Key player{};
    bool active{};
    bool team_down{};     // Every player in the host world is dead.
    bool checked_flags{};
};

Address image{};
std::filesystem::path state_dir;
std::unordered_map<Address, Observation> observed;
World world;

// Last known level, attributes and runes of the local character. A level
// gained any other way than a kill reward is a rune level-up and is undone.
struct Growth {
    Address game_data{};
    Key player{};
    unsigned level{};
    std::array<unsigned, 8> stats{};
    unsigned runes{};
    bool valid{};
};
Growth growth;
std::mt19937 rng;
std::uint64_t frame{};
bool reported_frame{};
bool failed{};
bool in_remote_world{};
bool seamless_loaded{};

struct MapScanCursor {
    unsigned set{};
    unsigned entry{};
};
MapScanCursor map_scan;

template <typename T> T get(Address p) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(p), sizeof(value));
    return value;
}

template <typename T> void put(Address p, T value) {
    std::memcpy(reinterpret_cast<void*>(p), &value, sizeof(value));
}

bool readable(Address p, std::size_t size) {
    if (p < 0x10000 || size == 0 || size - 1 > 0x00007fffffffffffull ||
        p > 0x00007fffffffffffull - (size - 1)) return false;
    if (!random_leveling_probe_byte(reinterpret_cast<const void*>(p))) return false;
    constexpr Address PageSize = 4096;
    const Address last = p + size - 1;
    for (Address page = (p & ~(PageSize - 1)) + PageSize; page <= last; page += PageSize) {
        if (!random_leveling_probe_byte(reinterpret_cast<const void*>(page))) return false;
    }
    return true;
}

bool matches(Address p, const std::uint8_t* bytes, std::size_t size) {
    return readable(p, size) && std::memcmp(reinterpret_cast<const void*>(p), bytes, size) == 0;
}

bool matches(Address p, std::initializer_list<std::uint8_t> bytes) {
    return matches(p, bytes.begin(), bytes.size());
}

void log(const char* message) {
    if (FILE* file = _wfopen((state_dir / L"random-leveling.log").c_str(), L"ab")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}

void log_event(const char* label, Key id, int value) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s %016llx %d",
                  label, static_cast<unsigned long long>(id), value);
    log(message);
}

Key hash_part(Key hash, std::uint32_t word) {
    return (hash ^ word) * 1099511628211ull;
}

Key hash_text(Key hash, Address text, unsigned limit) {
    for (unsigned i = 0; i < limit; ++i) {
        const auto code = get<std::uint16_t>(text + i * 2);
        if (code == 0) break;
        hash = hash_part(hash, code);
    }
    return hash;
}

bool map_block(std::uint32_t block) {
    return (block >> 24) >= 10 && (block >> 24) <= 99;
}

Key placement_key(const Character& c) {
    auto block = get<std::uint32_t>(c.data + 0x7c);
    if (!map_block(block)) block = get<std::uint32_t>(c.instance + 0x3c);
    if (!map_block(block)) return 0;
    const auto part = get<Address>(c.data + 0x28);
    if (readable(part, sizeof(Address))) {
        const auto name = get<Address>(part);
        if (readable(name, 48 * 2) && get<std::uint16_t>(name) != 0)
            return hash_text(hash_part(14695981039346656037ull, block), name, 48);
    }
    const auto event = get<std::uint32_t>(c.instance + 0x1e8);
    return (event >= 1000000u && event != 0xffffffffu) ? (Key(block) << 32) | event : 0;
}

bool load_character(Address instance, bool player, Character& c) {
    if (!readable(instance, 0x1ec)) return false;
    const auto vtable = get<Address>(instance);
    if (vtable != image + PlayerVtable && (player || vtable != image + EnemyVtable)) return false;
    const auto modules = get<Address>(instance + Modules);
    if (!readable(modules, sizeof(Address))) return false;
    const auto data = get<Address>(modules);
    if (!readable(data, BaseHealth + sizeof(int)) || get<Address>(data) != image + DataVtable ||
        get<Address>(data + 8) != instance) return false;

    const int hp = get<int>(data + Health);
    const int maximum = get<int>(data + MaxHealth);
    const int uncapped = get<int>(data + UncappedHealth);
    const int base = get<int>(data + BaseHealth);
    if (maximum <= 0 || maximum > 100000000 || base <= 0 || base > 100000000) return false;
    const int model = get<int>(instance + 0x64);
    const auto team = get<std::uint8_t>(instance + Team);
    c = {instance, data, hp, maximum, uncapped, base, model, team,
         (get<std::uint8_t>(instance + Flags) & 0x80) != 0, get<int>(instance + NpcParamId)};
    return true;
}

std::filesystem::path journal_path(int slot, Key name, bool seamless) {
    wchar_t filename[64];
    std::swprintf(filename, 64, seamless ? L"coop-slot-%02d-%016llx.journal" : L"slot-%02d-%016llx.journal", slot,
                  static_cast<unsigned long long>(name));
    return state_dir / filename;
}

void open_world(int slot, Key player_id, std::uint32_t game_deaths, std::uint32_t playtime, bool seamless = false) {
    world = {};
    growth.valid = false; // A different save may have a different level.
    world.active = true;
    world.slot = slot;
    world.player = player_id;
    world.last_game_deaths = game_deaths;
    world.last_playtime = playtime;
    world.journal = journal_path(slot, player_id, seamless);
    observed.clear();
    map_scan = {};
    if (FILE* file = _wfopen(world.journal.c_str(), L"rb")) {
        char line[100];
        while (std::fgets(line, sizeof(line), file)) {
            unsigned long long value{};
            char kind{};
            if (!std::strchr(line, '\n') || std::sscanf(line, "%c %llx", &kind, &value) != 2) continue;
            if (kind == 'K' && value) world.defeated.insert(value);
            if (kind == 'D' && value <= 1000000) world.deaths = static_cast<std::uint32_t>(value);
            if (kind == 'C' && value <= 1000000) world.kills = static_cast<std::uint32_t>(value);
            if (kind == 'P' && value <= 1000000) {
                world.progress = static_cast<std::uint32_t>(value);
                world.has_progress = true;
            }
        }
        std::fclose(file);
    }
    // Journals from the five-kills-per-level version have no progress record.
    if (!world.has_progress) world.progress = world.kills % 5;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "Character slot %d: %zu defeats, %u counted kills, %u level points, %u player deaths",
                  slot, world.defeated.size(), world.kills, world.progress, world.deaths);
    log(message);
}

bool append_record(char kind, Key value) {
    const auto h = CreateFileW(world.journal.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log("Cannot open the character journal");
        return false;
    }
    char line[32];
    const auto size = static_cast<DWORD>(
        std::snprintf(line, sizeof(line), "\n%c %016llx\n", kind, static_cast<unsigned long long>(value)));
    DWORD written{};
    LARGE_INTEGER offset{};
    // Windows writes the records to its file cache before WriteFile returns.
    // Avoid waiting for the storage device while the game task is updating.
    const bool ok = SetFilePointerEx(h, offset, nullptr, FILE_END) &&
                    WriteFile(h, line, size, &written, nullptr) && written == size;
    CloseHandle(h);
    if (!ok) log("Cannot write the character journal");
    return ok;
}

void remember_growth(Address game_data) {
    growth.game_data = game_data;
    growth.level = get<unsigned>(game_data + Level);
    for (unsigned i = 0; i < growth.stats.size(); ++i) growth.stats[i] = get<unsigned>(game_data + StatStart + i * 4);
    growth.runes = get<unsigned>(game_data + Runes);
    growth.valid = true;
}

bool add_random_stat(Address game_data, Key key) {
    std::array<unsigned, 8> candidates{};
    unsigned count{};
    for (unsigned i = 0; i < candidates.size(); ++i) {
        const auto value = get<unsigned>(game_data + StatStart + i * 4);
        if (value == 0 || value > 99) return false;
        if (value < 99) candidates[count++] = i;
    }
    const auto level = get<unsigned>(game_data + Level);
    if (count == 0 || level == 0 || level >= 713) return false;
    const unsigned index = candidates[std::uniform_int_distribution<unsigned>(0, count - 1)(rng)];
    const Address stat = game_data + StatStart + index * 4;
    put(stat, get<unsigned>(stat) + 1);
    put(game_data + Level, level + 1);
    if (growth.valid && growth.game_data == game_data) remember_growth(game_data);
    log_event("Attribute increased (0=vigor, 7=arcane)", key, index);
    return true;
}

// Undo a level bought with runes at a site of grace and give the runes back.
void undo_rune_levels(Address game_data, Key player_id) {
    if (!growth.valid || growth.game_data != game_data || growth.player != player_id) {
        remember_growth(game_data);
        growth.player = player_id;
        return;
    }
    const auto level = get<unsigned>(game_data + Level);
    const auto runes = get<unsigned>(game_data + Runes);
    unsigned before{}, after{};
    for (unsigned i = 0; i < growth.stats.size(); ++i) {
        before += growth.stats[i];
        after += get<unsigned>(game_data + StatStart + i * 4);
    }
    // A grace level-up adds exactly one attribute point per level.
    if (growth.level > 0 && level > growth.level && level <= growth.level + 99 &&
        after - before == level - growth.level && after > before) {
        for (unsigned i = 0; i < growth.stats.size(); ++i) put(game_data + StatStart + i * 4, growth.stats[i]);
        put(game_data + Level, growth.level);
        if (runes < growth.runes) put(game_data + Runes, growth.runes);
        log_event("Rune level-up undone; runes refunded", player_id, static_cast<int>(level - growth.level));
    }
    remember_growth(game_data);
}

// Adds level points and spends them on levels along the curve.
void add_level_points(Address game_data, Key key, unsigned points, std::uint32_t& progress, bool persist) {
    progress += points;
    while (progress >= level_cost(get<unsigned>(game_data + Level))) {
        const unsigned cost = level_cost(get<unsigned>(game_data + Level));
        if (!add_random_stat(game_data, key)) {
            progress = std::min(progress, cost - 1); // Maximum level: stop collecting.
            break;
        }
        progress -= cost;
    }
    if (persist) append_record('P', progress);
}

int scaled(int base, unsigned deaths) {
    const auto value = static_cast<std::int64_t>(base) +
                       static_cast<std::int64_t>(base) * deaths * settings.death_health_percent / 100;
    return static_cast<int>(std::min<std::int64_t>(value, 100000000));
}

void scale_enemy(const Character& c, Observation& old) {
    if (c.hp <= 0 || c.dead || world.deaths == 0) return;
    if (c.base != old.mod_base) old.base = c.base;
    const int new_base = scaled(old.base, world.deaths);
    const bool engine_recomputed = old.mod_base && c.base == old.mod_base && c.max != old.mod_max;
    if (c.max != old.mod_max) {
        // If the game derives a new maximum from our base (e.g. a max-HP
        // debuff), remove our existing multiplier before calculating again.
        old.unscaled_max = engine_recomputed
            ? std::max(1, int((std::int64_t(c.max) * old.base + old.mod_base / 2) / old.mod_base))
            : c.max;
    }
    if (c.uncapped != old.mod_uncapped) {
        old.unscaled_uncapped = engine_recomputed && c.uncapped > 0
            ? std::max(1, int((std::int64_t(c.uncapped) * old.base + old.mod_base / 2) / old.mod_base))
            : c.uncapped;
    }
    const int new_max = scaled(old.unscaled_max, world.deaths);
    const int new_uncapped = old.unscaled_uncapped > 0 ? scaled(old.unscaled_uncapped, world.deaths) : c.uncapped;
    if (new_base != c.base) put(c.data + BaseHealth, new_base);
    if (new_uncapped != c.uncapped) put(c.data + UncappedHealth, new_uncapped);
    if (new_max != c.max && new_max > 0) {
        const auto adjusted = static_cast<int>((std::int64_t(c.hp) * new_max + c.max / 2) / c.max);
        put(c.data + MaxHealth, new_max);
        put(c.data + Health, std::clamp(adjusted, 1, new_max));
    }
    old.mod_base = new_base;
    old.mod_max = new_max;
    old.mod_uncapped = new_uncapped;
}

bool is_hostile(const Character& player, const Character& enemy) {
    if (enemy.model == 100 || enemy.model == 1000 || enemy.model == 8000) return false;
    // Player-shaped NPCs can share PlayerIns with co-op and invasion players.
    // Allow only the game's three hostile-capable NPC roles for that class.
    if (get<Address>(enemy.instance) == image + PlayerVtable) {
        const auto type = get<int>(enemy.instance + 0x68);
        if (type != 5 && type != 20 && type != 21) return false;
    }
    if (enemy.team == 6 || enemy.team == 7) return true;
    if (enemy.model == 0) return false; // Do not affect non-hostile humanoid quest NPCs.
    using Function = unsigned (*)(Address, Address);
    return enemy.team != 0 && reinterpret_cast<Function>(image + CanTarget)(player.instance, enemy.instance);
}

bool script_controls_death(const Character& c) {
    const auto entity = get<std::uint32_t>(c.instance + 0x1e8);
    const auto local = entity % 1000;
    // Standard boss entities use the 800-series. The game's boss completion
    // flags already persist those deaths and must decide when phases can reset.
    return entity >= 1000000 && local >= 800 && local < 900;
}

// Wildlife models (c4 digits): birds, deer, goats, rams, boars, murres,
// pigeons, dragonflies, turtles, springhares and scarabs, base game and DLC.
bool is_critter(int model) {
    if (model == 6091) return false; // Miriel shares the turtle model group.
    switch (model / 10) {
    case 540: case 541: case 543: case 544: case 545: case 546: case 547: case 548: case 549:
    case 600: case 601: case 604: case 605: case 606: case 607: case 608: case 609:
    case 610: case 620:
        return true;
    default:
        return false;
    }
}

// Boss rune rewards mark bosses, including field bosses. The 800-series entity
// rule is not used here: boss-arena adds and summons share that range.
bool is_boss(const Character& c) {
    return std::binary_search(std::begin(BossNpcParams), std::end(BossNpcParams), c.npc_param);
}

void reward_kill(const Character& c, Address game_data, Key key, bool /*owns_world*/) {
    if (is_critter(c.model)) {
        if (!settings.wildlife_points) {
            log_event("Wildlife defeated; no level", key, c.model);
            return;
        }
        add_level_points(game_data, key, settings.wildlife_points, world.progress, true);
        log_event("Wildlife defeated; level points", key, static_cast<int>(world.progress));
        return;
    }
    // A guest's rewards go to their own character journal, like the host's.
    if (is_boss(c)) {
        log_event("Boss defeated; level points", key, static_cast<int>(settings.boss_points));
        add_level_points(game_data, key, settings.boss_points, world.progress, true);
        return;
    }
    if (!append_record('C', world.kills + 1)) return;
    ++world.kills;
    add_level_points(game_data, key, settings.enemy_points, world.progress, true);
    log_event("Enemy kill counted; level points", key, static_cast<int>(world.progress));
}

// Handles of the other players in a co-op session. A kill by any of them
// counts for everyone, so the whole party levels together.
std::vector<Key> party_handles;

bool hit_by_party(Key attacker, Key player_handle) {
    return attacker && (attacker == player_handle ||
                        std::find(party_handles.begin(), party_handles.end(), attacker) != party_handles.end());
}

bool death_hook_active{};

void enemy_tick(const Character& c, Key player_handle, Address game_data, bool owns_world) {
    const Key actor_handle = get<Key>(c.instance + Handle);
    const Key key = placement_key(c);
    auto& old = observed[c.instance];
    if (old.data != c.data || old.handle != actor_handle || old.placement != key) {
        old = {};
        old.data = c.data;
        old.handle = actor_handle;
        old.placement = key;
    }
    old.last_frame = frame;
    const bool alive = !c.dead && c.hp > 0;
    // The journal always belongs to the local character. A guest in a
    // Seamless Co-op host world records the team's kills there too, so those
    // enemies stay dead back home. Only the host changes the world it is in.
    if (key && world.defeated.contains(key)) {
        if (alive && owns_world && settings.enemies_stay_dead && !script_controls_death(c)) {
            using Function = void (*)(Address, bool);
            reinterpret_cast<Function>(image + ForceDeath)(c.instance, false);
            old.seen_alive = false;
        } else if (alive && owns_world) {
            scale_enemy(c, old);
        }
        return;
    }
    if (alive) {
        old.seen_alive = true;
        old.player_hit = old.player_hit || hit_by_party(get<Key>(c.instance + LastHitBy), player_handle);
        if (owns_world) scale_enemy(c, old);
        return;
    }
    if (!old.seen_alive) return;
    if (death_hook_active) {
        // Kills come from the game's rune award instead (see on_rune_award).
        old.seen_alive = false;
        return;
    }
    const bool credited = old.player_hit || hit_by_party(get<Key>(c.instance + LastHitBy), player_handle);
    if (key) {
        if (!append_record('K', key)) return;
        world.defeated.insert(key);
        log_event(owns_world ? "Enemy defeat recorded" : "Enemy defeat in host world recorded for own world",
                  key, c.model);
    }
    old.seen_alive = false;
    if (credited) {
        reward_kill(c, game_data, key, owns_world);
    } else if (!key) {
        log_event("Enemy death ignored; no map placement or player hit", key, c.model);
    }
}

// The game drops corpses from the by-distance list and from target checks
// before the death flag is visible to us. Keep checking every enemy that we
// saw alive through its own pointer until it dies or its slot is reused.
bool still_same_enemy(Address instance, const Observation& old, Character& c) {
    return load_character(instance, false, c) && c.data == old.data &&
           get<Key>(instance + Handle) == old.handle;
}

// A death the game awarded runes for, captured by the rune award hook. The
// hook may run on a game worker thread, so it only queues the facts and the
// gameplay task handles them.
struct Death {
    Key placement{};
    int model{};
    int npc_param{};
    int team{};
    int chr_type{};
    bool player_shaped{};
};
SRWLOCK death_lock = SRWLOCK_INIT;
std::vector<Death> pending_deaths;
using RuneAwardFn = float (*)(Address);
RuneAwardFn rune_award_original{};
std::atomic<unsigned> rune_award_calls{};

// The game's own rune award also runs for friendly NPCs. Keep the same
// hostile roles the map scan accepts, without the live target check, which
// a dead character no longer passes.
bool counts_as_enemy(const Death& d) {
    if (d.model == 100 || d.model == 1000 || d.model == 8000) return false;
    if (d.player_shaped && d.chr_type != 5 && d.chr_type != 20 && d.chr_type != 21) return false;
    if (d.team == 6 || d.team == 7) return true;
    return d.model != 0 && d.team != 0;
}

float on_rune_award(Address chr) {
    const bool before = readable(chr + DeathFlags, 1) && (get<std::uint8_t>(chr + DeathFlags) & 2);
    const float runes = rune_award_original(chr);
    ++rune_award_calls;
    if (before || !readable(chr + DeathFlags, 1) || !(get<std::uint8_t>(chr + DeathFlags) & 2)) return runes;
    Character c{};
    if (!load_character(chr, false, c)) return runes;
    const Death d{placement_key(c), c.model, c.npc_param, c.team, get<int>(chr + 0x68),
                  get<Address>(chr) == image + PlayerVtable};
    AcquireSRWLockExclusive(&death_lock);
    if (pending_deaths.size() < 4096) pending_deaths.push_back(d);
    ReleaseSRWLockExclusive(&death_lock);
    return runes;
}

void process_deaths(Address game_data, bool owns_world) {
    std::vector<Death> batch;
    AcquireSRWLockExclusive(&death_lock);
    batch.swap(pending_deaths);
    ReleaseSRWLockExclusive(&death_lock);
    for (const Death& d : batch) {
        if (!counts_as_enemy(d)) continue;
        if (d.placement && world.defeated.contains(d.placement)) continue; // Already counted once.
        if (d.placement) {
            if (!append_record('K', d.placement)) continue;
            world.defeated.insert(d.placement);
            log_event(owns_world ? "Enemy defeat recorded" : "Enemy defeat in host world recorded for own world",
                      d.placement, d.model);
        }
        Character c{};
        c.model = d.model;
        c.npc_param = d.npc_param;
        reward_kill(c, game_data, d.placement, owns_world);
    }
}

// Replace the first instructions of `target` with a jump to `hook` and return
// a trampoline that runs them and continues the original function.
template <std::size_t N>
Address install_jump(Address target, const std::array<std::uint8_t, N>& prologue, Address hook) {
    static_assert(N >= 14);
    if (!matches(target, prologue.data(), N)) return 0;
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return 0;
    const Address resume = target + N;
    std::memcpy(trampoline, prologue.data(), N);
    const std::uint8_t jump[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
    std::memcpy(trampoline + N, jump, 6);
    std::memcpy(trampoline + N + 6, &resume, 8);
    std::array<std::uint8_t, N> patch{};
    patch.fill(0x90);
    std::memcpy(patch.data(), jump, 6);
    std::memcpy(patch.data() + 6, &hook, 8);
    DWORD protection{};
    if (!VirtualProtect(reinterpret_cast<void*>(target), N, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(target), patch.data(), N);
    VirtualProtect(reinterpret_cast<void*>(target), N, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), N);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    return reinterpret_cast<Address>(trampoline);
}

bool jump_in_place(Address target, Address hook) {
    return matches(target, {0xff, 0x25, 0, 0, 0, 0}) && readable(target + 6, 8) && get<Address>(target + 6) == hook;
}

// Finds the descriptor node of an event-flag group in CSEventFlagMan's
// std::map. Returns 0 when the group has none, or -1 (as Address) when the
// manager is not ready or not recognised.
constexpr Address NoFlagManager = ~Address{};
Address flag_group_node(unsigned group) {
    const auto manager = get<Address>(image + EventFlagMan);
    if (!readable(manager, 0x48) || get<unsigned>(manager + 0x1c) != 1000 ||
        get<unsigned>(manager + 0x20) != 125) return NoFlagManager;
    const auto head = get<Address>(manager + 0x38);
    if (!readable(head, 0x20)) return NoFlagManager;
    Address node = get<Address>(head + 8); // MSVC std::map: head->parent is the root.
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (!readable(node, 0x38)) return NoFlagManager;
        if (get<std::uint8_t>(node + 0x19)) return 0; // Reached the sentinel.
        const auto key = get<unsigned>(node + 0x20);
        if (group == key) return node;
        node = get<Address>(node + (group < key ? 0 : 0x10));
    }
    return NoFlagManager;
}

// 1 = storage exists, 0 = no storage (flags in the group are never kept),
// -1 = the event flag manager is not ready or not recognised.
int flag_group_state(unsigned group) {
    const Address node = flag_group_node(group);
    if (node == NoFlagManager) return -1;
    if (!node) return 0;
    const auto mode = get<unsigned>(node + 0x28);
    return mode == 1 || mode == 2 ? 1 : 0;
}

// 1 = set, 0 = clear, -1 = unknown.
int event_flag(unsigned flag) {
    const Address node = flag_group_node(flag / 1000);
    if (node == NoFlagManager || !node) return -1;
    Address block{};
    const auto mode = get<unsigned>(node + 0x28);
    if (mode == 1) {
        const auto manager = get<Address>(image + EventFlagMan);
        block = get<Address>(manager + 0x28) + Address(get<unsigned>(node + 0x30)) * 125;
    } else if (mode == 2) {
        block = get<Address>(node + 0x30);
    } else {
        return -1;
    }
    const unsigned local = flag % 1000;
    if (!readable(block + local / 8, 1)) return -1;
    return (get<std::uint8_t>(block + local / 8) >> (7 - local % 8)) & 1;
}

constexpr unsigned ItemFlags[] = {
#include "item_flags.inc"
};

// Logs how many one-time equipment flags the save has, whenever it changes.
// A pickup that never raises the count means the game did not keep the flag.
void report_item_flags() {
    static int last = -1;
    int set{};
    for (const unsigned flag : ItemFlags) {
        const int state = event_flag(flag);
        if (state < 0) return;
        set += state;
    }
    if (set == last) return;
    last = set;
    char message[96];
    std::snprintf(message, sizeof(message), "Equipment obtained (one-time drop flags set): %d of %zu",
                  set, std::size(ItemFlags));
    log(message);
}

void check_item_flags() {
    if (world.checked_flags) return;
    unsigned missing{};
    for (const unsigned group : ItemFlagGroups) {
        const int state = flag_group_state(group);
        if (state < 0) return; // Try again on a later update.
        if (state == 0) {
            ++missing;
            log_event("Equipment flag group has no save storage", group, 0);
        }
    }
    world.checked_flags = true;
    char message[128];
    std::snprintf(message, sizeof(message), "Equipment flag groups stored: %u of %zu (old group %u stored: %d)",
                  static_cast<unsigned>(std::size(ItemFlagGroups) - missing), std::size(ItemFlagGroups),
                  OldItemFlagGroup, flag_group_state(OldItemFlagGroup));
    log(message);
}

// Seamless Co-op: the death penalty applies only when every player present in
// the host world is dead at the same time. Returns false if the set is unreadable.
bool team_wiped(Address manager, Address main_player, bool& wiped) {
    const Address set = manager + PlayerChrSet;
    if (!readable(set, 0x20)) return false;
    const auto capacity = get<unsigned>(set + 0x10);
    const auto entries = get<Address>(set + 0x18);
    if (capacity == 0 || capacity > PlayerSetLimit || !readable(entries, capacity * 0x10)) return false;
    unsigned present{}, down{};
    bool saw_main{};
    for (unsigned i = 0; i < capacity; ++i) {
        const auto actor = get<Address>(entries + i * 0x10);
        Character p{};
        if (!actor || !load_character(actor, true, p)) continue;
        ++present;
        saw_main = saw_main || actor == main_player;
        if (p.dead || p.hp <= 0) ++down;
    }
    if (!saw_main) return false;
    wiped = down == present;
    return true;
}

bool can_write_world(bool seamless, std::uint8_t event_world, int player_type) {
    // Seamless uses the game's local/remote event world and phantom roles.
    // World changes belong to the host. A guest can still earn their own stats.
    return !seamless || (event_world == 0 && player_type == 0);
}

template <typename Visit>
void scan_map_entries(Address manager, MapScanCursor& cursor, Visit&& visit) {
    unsigned remaining = MapEntriesPerUpdate;
    for (unsigned sets = 0; sets < MapSetsPerUpdate && remaining; ++sets) {
        const auto set = get<Address>(manager + CharacterSets + cursor.set * sizeof(Address));
        if (readable(set, 0x20)) {
            const auto capacity = get<unsigned>(set + 0x10);
            const auto entries = get<Address>(set + 0x18);
            if (capacity <= 4096 && cursor.entry < capacity && entries >= 0x10000) {
                const unsigned count = std::min(remaining, capacity - cursor.entry);
                const unsigned stop = cursor.entry + count;
                if (readable(entries + cursor.entry * 0x10, count * 0x10)) {
                    remaining -= count;
                    for (; cursor.entry < stop; ++cursor.entry) {
                        const Address entry = entries + cursor.entry * 0x10;
                        const auto status = get<std::uint8_t>(entry + 8);
                        if (status == 2 || status == 4) visit(get<Address>(entry));
                    }
                    if (cursor.entry < capacity) return;
                }
            }
        }
        cursor.entry = 0;
        cursor.set = (cursor.set + 1) % CharacterSetCount;
    }
}

struct TickTiming {
    LARGE_INTEGER started{};
    LARGE_INTEGER phase_started{};
    LONGLONG longest{};
    const char* phase = "initialize";
    const char* longest_phase = phase;
    unsigned nearby_candidates{};
    unsigned map_candidates{};

    void next(const char* label) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const auto duration = now.QuadPart - phase_started.QuadPart;
        if (duration > longest) {
            longest = duration;
            longest_phase = phase;
        }
        phase = label;
        phase_started = now;
    }
};

std::filesystem::path settings_path;
FILETIME settings_time{};

// Written as random-leveling.ini when the file is missing.
const char DefaultSettings[] = R"(; 随机升级（Random Leveling）设置。保存后约 2 秒内在游戏里生效，不用重启。
; 只有 rune_hook 需要重启游戏。改坏了就删掉本文件，下次启动会重新生成默认值。
; 每次读取的结果会写进 state\random-leveling.log，填错的项会被忽略并写明。

[升级点数]
; 击杀普通敌人和精英怪获得的点数
enemy_points = 1
; 击杀 Boss（含野外 Boss）获得的点数
boss_points = 20
; 击杀小动物获得的点数，0 = 不给
wildlife_points = 0

[升级曲线]
; 每升一级需要的点数从 level_cost 开始，
; 角色等级超过 flat_cost_until 后，每 cost_step_levels 级多需要 1 点。
; 默认值：20 级 7 点，60 级 15 点，100 级 23 点，150 级 33 点。
level_cost = 5
flat_cost_until = 10
; 0 = 一直不涨
cost_step_levels = 5
; 每级所需点数的上限，0 = 不设上限
max_level_cost = 0

[惩罚]
; 每次死亡（无缝联机为每次团灭）敌人血量增加基础值的百分之几，线性叠加
death_health_percent = 5
; 1 = 无缝联机只有团灭才算；0 = 房主每次死亡都算
team_wipe_only = 1

[规则开关]
; 1 = 撤销用卢恩升级并退还卢恩；0 = 允许用卢恩升级
undo_rune_levels = 1
; 1 = 已击杀的敌人读图后保持死亡；0 = 不强制（坐赐福不复活仍由参数文件控制）
enemies_stay_dead = 1
; 1 = 跟随游戏发放卢恩来计算击杀（联机同步）；0 = 改用扫描敌人。改这项需要重启游戏
rune_hook = 1
)";

void load_settings(bool startup) {
    if (settings_path.empty()) settings_path = state_dir.parent_path() / L"random-leveling.ini";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(settings_path.c_str(), GetFileExInfoStandard, &info)) {
        if (!startup) return;
        if (FILE* file = _wfopen(settings_path.c_str(), L"wb")) {
            std::fwrite(DefaultSettings, 1, sizeof(DefaultSettings) - 1, file);
            std::fclose(file);
            log("Created random-leveling.ini with default values");
        }
        if (!GetFileAttributesExW(settings_path.c_str(), GetFileExInfoStandard, &info)) return;
    }
    if (!startup && CompareFileTime(&info.ftLastWriteTime, &settings_time) == 0) return;
    std::string text;
    FILE* file = _wfopen(settings_path.c_str(), L"rb");
    if (!file) return; // An editor may be saving it; try again on the next check.
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 65536) text.append(buffer, n);
    std::fclose(file);
    settings_time = info.ftLastWriteTime;
    std::string problems;
    const bool hook = settings.rune_hook;
    settings = parse_settings(text, problems);
    if (!startup) settings.rune_hook = hook; // Takes effect only after a restart.
    char message[512];
    std::snprintf(message, sizeof(message),
                  "Settings: enemy %u, boss %u, wildlife %u; cost %u, flat until %u, +1 every %u, max %u; "
                  "health +%u%% per death; team wipe only %d; undo rune levels %d; stay dead %d; rune hook %d%s%s",
                  settings.enemy_points, settings.boss_points, settings.wildlife_points, settings.level_cost,
                  settings.flat_cost_until, settings.cost_step_levels, settings.max_level_cost,
                  settings.death_health_percent, settings.team_wipe_only, settings.undo_rune_levels,
                  settings.enemies_stay_dead, settings.rune_hook, problems.empty() ? "" : "; ignored",
                  problems.c_str());
    log(message);
}

void tick(TickTiming& timing) {
    static ULONGLONG next_settings_check{};
    if (GetTickCount64() >= next_settings_check) {
        next_settings_check = GetTickCount64() + 2000;
        load_settings(false);
    }
    timing.next("world lookup");
    const auto manager = get<Address>(image + WorldChrMan);
    if (!readable(manager, sizeof(Address)) || get<Address>(manager) != image + WorldVtable) return;
    timing.next("player lookup");
    Character player{};
    if (!readable(manager + LocalPlayer, sizeof(Address))) return;
    if (!load_character(get<Address>(manager + LocalPlayer), true, player)) return;
    timing.next("save information");
    const auto data_manager = get<Address>(image + GameDataMan);
    const auto game_manager = get<Address>(image + GameMan);
    if (!readable(data_manager, 0xa4) || !readable(game_manager, 0xbcb)) return;
    const int slot = get<int>(game_manager + 0xac0);
    const auto game_data = get<Address>(data_manager + 8);
    if (slot < 0 || slot > 9 || !readable(game_data, 0xc0) || !get<std::uint16_t>(game_data + 0x9c)) return;
    const Key player_id = hash_text(14695981039346656037ull, game_data + 0x9c, 16);
    const auto game_deaths = get<std::uint32_t>(data_manager + 0x94);
    const auto playtime = get<std::uint32_t>(data_manager + 0xa0);
    const bool seamless = seamless_loaded;
    timing.next("world state");
    const bool owns_world = can_write_world(seamless, get<std::uint8_t>(game_manager + 0xbca),
                                             get<int>(player.instance + 0x68));
    if (in_remote_world == owns_world) {
        in_remote_world = !owns_world;
        growth.valid = false;
        observed.clear();
        map_scan = {};
        world.active = false;
        log(owns_world ? "Host world active" : "Guest world active; kills saved to own world, host world unchanged");
    }
    if (!world.active || world.slot != slot || world.player != player_id ||
        playtime + 30000ull < world.last_playtime) {
        open_world(slot, player_id, game_deaths, playtime, seamless);
    }
    world.last_playtime = playtime;
    if (owns_world) check_item_flags();
    if (frame % 64 == 0) report_item_flags();
    if (death_hook_active && frame % 128 == 0 &&
        !jump_in_place(image + RuneAward, reinterpret_cast<Address>(&on_rune_award))) {
        death_hook_active = false; // Another mod replaced the hook; use the map scan.
        log("Rune award hook was overwritten; falling back to the enemy scan");
    }
    process_deaths(game_data, owns_world);
    if (settings.undo_rune_levels) {
        undo_rune_levels(game_data, hash_part(player_id, static_cast<std::uint32_t>(slot)));
    } else {
        growth.valid = false; // Turning it back on must not undo earlier levels.
    }
    bool wiped{};
    static bool reported_party_error{};
    if (owns_world && seamless && settings.team_wipe_only) {
        // In co-op one player's death is not a penalty; a full team wipe is.
        world.last_game_deaths = game_deaths;
        if (!team_wiped(manager, player.instance, wiped)) {
            if (!reported_party_error) log("Cannot read the co-op player list; team wipe check paused");
            reported_party_error = true;
        } else if (wiped && !world.team_down) {
            world.team_down = true;
            if (append_record('D', world.deaths + 1)) {
                ++world.deaths;
                log_event("Team wiped; enemy health penalty", player_id, static_cast<int>(world.deaths));
            }
        } else if (!wiped) {
            world.team_down = false;
        }
    } else if (owns_world && game_deaths > world.last_game_deaths && game_deaths - world.last_game_deaths <= 20) {
        const auto total = world.deaths + (game_deaths - world.last_game_deaths);
        if (append_record('D', total)) {
            world.deaths = total;
            world.last_game_deaths = game_deaths;
            log_event("Player deaths", player_id, total);
        }
    } else if (owns_world) {
        world.last_game_deaths = game_deaths;
    }

    const Key player_handle = get<Key>(player.instance + Handle);
    party_handles.clear();
    if (seamless) {
        const Address set = manager + PlayerChrSet;
        if (readable(set, 0x20)) {
            const auto capacity = get<unsigned>(set + 0x10);
            const auto entries = get<Address>(set + 0x18);
            if (capacity && capacity <= PlayerSetLimit && readable(entries, capacity * 0x10)) {
                for (unsigned i = 0; i < capacity; ++i) {
                    const auto actor = get<Address>(entries + i * 0x10);
                    Character p{};
                    if (actor && actor != player.instance && load_character(actor, true, p))
                        party_handles.push_back(get<Key>(actor + Handle));
                }
            }
        }
    }
    ++frame;
    unsigned enemies{};
    const auto process_actor = [&](Address actor) {
        if (!actor || actor == player.instance) return;
        const auto seen = observed.find(actor);
        if (seen != observed.end() && seen->second.last_frame == frame) return;
        Character character{};
        if (!load_character(actor, false, character)) return;
        // A dying enemy can stop passing the target check. Keep the earlier
        // decision for an enemy that we are already tracking.
        const bool tracked = seen != observed.end() && seen->second.seen_alive &&
                             seen->second.data == character.data &&
                             seen->second.handle == get<Key>(actor + Handle);
        if (!tracked && !is_hostile(player, character)) return;
        enemy_tick(character, player_handle, game_data, owns_world);
        ++enemies;
    };
    // With the rune award hook a guest has nothing to scan: only the host
    // changes enemies, and kills arrive through the hook.
    const bool scan = owns_world || !death_hook_active;
    timing.next("nearby characters");
    if (scan && readable(manager + ByDistance + 8, 2 * sizeof(Address))) {
        const Address begin = get<Address>(manager + ByDistance + 8);
        const Address end = get<Address>(manager + ByDistance + 0x10);
        if (end > begin && end - begin <= 0x10000 && (end - begin) % 0x10 == 0 &&
            readable(begin, end - begin)) {
            timing.nearby_candidates = static_cast<unsigned>((end - begin) / 0x10);
            for (Address current = begin; current < end; current += 0x10)
                process_actor(get<Address>(current));
        }
    }
    // Check nearby enemies first. Spread the background map scan over multiple
    // updates so streaming a large map cannot put the whole scan in one frame.
    timing.next("streamed map characters");
    // Without the hook guests scan too (read only) to see teammates' kills.
    if (scan && readable(manager + CharacterSets, CharacterSetCount * sizeof(Address))) {
        scan_map_entries(manager, map_scan, [&](Address actor) {
            ++timing.map_candidates;
            process_actor(actor);
        });
    }
    timing.next("tracked characters");
    std::vector<Address> tracked;
    for (const auto& [actor, old] : observed) {
        if (old.seen_alive && old.last_frame != frame && old.last_frame + 250 >= frame) tracked.push_back(actor);
    }
    for (const Address actor : tracked) {
        Character character{};
        const auto& old = observed[actor];
        if (still_same_enemy(actor, old, character)) {
            enemy_tick(character, player_handle, game_data, owns_world);
        } else {
            observed[actor].seen_alive = false;
        }
    }
    timing.next("cleanup");
    if (enemies && !reported_frame) {
        log_event("World updates active; loaded enemies", player_id, enemies);
        reported_frame = true;
    }
    if (frame % 512 == 0) {
        std::erase_if(observed, [](const auto& entry) { return entry.second.last_frame + 24000 < frame; });
    }
}

Address task_runtime_class(void*) { return 0; }
void task_destroy(void*, unsigned) {}

void task_tick(void*, const void*) {
    static ULONGLONG next{};
    static ULONGLONG next_report{};
    static LARGE_INTEGER frequency{};
    static char peak_message[256]{};
    static double peak_ms{};
    const auto now = GetTickCount64();
    if (failed || now < next) return;
    next = now + 40;
    try {
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        TickTiming timing;
        QueryPerformanceCounter(&timing.started);
        timing.phase_started = timing.started;
        tick(timing);
        timing.next("done");
        const double elapsed_ms = 1000.0 * (timing.phase_started.QuadPart - timing.started.QuadPart) / frequency.QuadPart;
        if (elapsed_ms > peak_ms) {
            peak_ms = elapsed_ms;
            std::snprintf(peak_message, sizeof(peak_message),
                          "Slow gameplay update %.2f ms; %s %.2f ms, nearby=%u map=%u",
                          elapsed_ms, timing.longest_phase, 1000.0 * timing.longest / frequency.QuadPart,
                          timing.nearby_candidates, timing.map_candidates);
        }
        if (now >= next_report) {
            if (peak_ms >= 3.0) log(peak_message);
            peak_ms = 0;
            next_report = now + 5000;
        }
    } catch (...) {
        failed = true;
        log("World processing stopped after an unexpected error");
    }
}

struct TaskVtableFns {
    decltype(&task_runtime_class) runtime_class;
    decltype(&task_destroy) destroy;
    decltype(&task_tick) execute;
};
struct TaskInstance {
    const TaskVtableFns* vtable;
    std::array<Address, 7> storage{};
};
constexpr TaskVtableFns task_functions{task_runtime_class, task_destroy, task_tick};
TaskInstance task{&task_functions};

DWORD WINAPI init(void* module) {
    try {
        wchar_t name[32768];
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(module), name, std::size(name));
        if (!length || length == std::size(name)) return 0;
        state_dir = std::filesystem::path(name).parent_path().parent_path() / L"state";
        std::filesystem::create_directories(state_dir);
        image = reinterpret_cast<Address>(GetModuleHandleW(nullptr));
        if (!matches(image + RegisterTask, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0x8d,0x4c,0x24,0x20,0x49,0x8b,0xd8,0x8b,0xfa,0xe8}) ||
            !matches(image + ForceDeath, {0x48,0x85,0xc9,0x74,0x12,0x84,0xd2,0x75,0x07,0x80,0x89,0xc6,0x01,0x00,0x00,0x03}) ||
            !matches(image + CanTarget, {0x40,0x56,0x48,0x83,0xec,0x30,0x48,0x8b,0xf2,0x48,0x85,0xc9,0x74,0x71})) {
            log("Unsupported executable; gameplay module disabled");
            return 0;
        }
        rng.seed(std::random_device{}());
        load_settings(true);
        // Install before the world loads, while no character can be dying.
        if (!settings.rune_hook) {
            log("Rune award hook turned off in random-leveling.ini; using the enemy scan");
        } else if (const Address trampoline = install_jump(image + RuneAward, RuneAwardPrologue,
                                                    reinterpret_cast<Address>(&on_rune_award))) {
            rune_award_original = reinterpret_cast<RuneAwardFn>(trampoline);
            death_hook_active = true;
            log("Rune award hook installed; kills follow the game's rune sharing");
        } else {
            log("Rune award hook unavailable; using the enemy scan");
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&task_tick), &pinned)) return 0;
        const auto timeout = GetTickCount64() + 120000;
        while (GetTickCount64() < timeout) {
            const auto manager = get<Address>(image + TaskManager);
            if (readable(manager, 16) && get<Address>(manager) == image + TaskVtable &&
                get<Address>(manager + 8) && get<Address>(image + TaskGroupManager)) {
                using Register = bool (*)(Address, unsigned, TaskInstance*);
                seamless_loaded = GetModuleHandleW(L"ersc.dll") != nullptr;
                const bool registered = reinterpret_cast<Register>(image + RegisterTask)(manager, 117, &task);
                log(registered ? "Gameplay task registered" : "Game rejected the gameplay task");
                return 0;
            }
            Sleep(100);
        }
        log("The game task manager did not initialize");
    } catch (...) {
        if (!state_dir.empty()) log("Could not initialize the gameplay module");
    }
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HMODULE self, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (HANDLE worker = CreateThread(nullptr, 0, init, self, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
