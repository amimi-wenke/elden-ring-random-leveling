#include "random_leveling.cpp"

#include <cassert>

namespace {
Address suppressed_character{};
void record_suppression(Address character, bool award_loot) {
    assert(!award_loot);
    suppressed_character = character;
}
}

int main() {
    const auto probe = reinterpret_cast<Address>(
        VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(probe && readable(probe, 8192));
    DWORD old_protection{};
    assert(VirtualProtect(reinterpret_cast<void*>(probe + 4096), 4096, PAGE_NOACCESS, &old_protection));
    assert(readable(probe, 4096));
    assert(!readable(probe, 8192));
    assert(!readable(probe + 4096, 1));
    assert(VirtualFree(reinterpret_cast<void*>(probe), 0, MEM_RELEASE));
    assert(!readable(probe, 1));

    assert(scaled(1000, 0) == 1000);
    assert(scaled(1000, 1) == 1050);
    assert(scaled(1000, 2) == 1100);
    assert(scaled(1000, 3) == 1150);
    assert(scaled(21, 3) == 24);

    std::array<int, 4> health{600, 1200, 1200, 1000};
    const auto address = reinterpret_cast<Address>(health.data()) - Health;
    const auto enemy = [&]() {
        return Character{0, address, health[0], health[1], health[2], health[3], 1001, 6, false};
    };
    Observation original;
    world.deaths = 1;
    scale_enemy(enemy(), original);
    assert((health == std::array<int, 4>{630, 1260, 1260, 1050}));
    world.deaths = 2;
    scale_enemy(enemy(), original);
    scale_enemy(enemy(), original);
    assert((health == std::array<int, 4>{660, 1320, 1320, 1100}));
    // The game recalculates HP from the modified base after a temporary debuff.
    health = {550, 1100, 1100, 1100};
    scale_enemy(enemy(), original);
    assert((health == std::array<int, 4>{550, 1100, 1100, 1100}));
    world.deaths = 3;
    scale_enemy(enemy(), original);
    assert((health == std::array<int, 4>{575, 1150, 1150, 1150}));

    wchar_t temp[MAX_PATH];
    assert(GetTempPathW(std::size(temp), temp));
    state_dir = std::filesystem::path(temp) / (L"elden-random-leveling-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(state_dir);

    std::array<unsigned, 32> stats{};
    const auto player = reinterpret_cast<Address>(stats.data());
    for (unsigned i = 0; i < 8; ++i) put(player + StatStart + i * 4, 99u);
    put(player + StatStart + 5 * 4, 98u);
    put(player + Level, 712u);
    add_random_stat(player, 11);
    assert(get<unsigned>(player + StatStart + 5 * 4) == 99);
    assert(get<unsigned>(player + Level) == 713);
    add_random_stat(player, 12);
    assert(get<unsigned>(player + Level) == 713);

    open_world(1, 123, 0, 0);
    assert(append_record('K', 456));
    assert(append_record('D', 3));
    open_world(1, 123, 0, 0);
    assert(world.defeated.contains(456) && world.deaths == 3);
    std::filesystem::remove(world.journal);
    open_world(2, 123, 0, 0);
    assert(world.defeated.empty() && world.deaths == 0);

    assert(can_write_world(false, 0, 0));
    assert(can_write_world(true, 0, 0));
    assert(!can_write_world(true, 1, 0));
    assert(!can_write_world(true, 0, 1));
    assert(journal_path(2, 123, false) != journal_path(2, 123, true));

    std::vector<std::uint8_t> map_manager(CharacterSets + CharacterSetCount * sizeof(Address));
    std::array<std::uint8_t, 0x20> set{};
    std::vector<std::uint8_t> entries(700 * 0x10);
    const auto manager_address = reinterpret_cast<Address>(map_manager.data());
    const auto set_address = reinterpret_cast<Address>(set.data());
    const auto entries_address = reinterpret_cast<Address>(entries.data());
    put(manager_address + CharacterSets, set_address);
    put(set_address + 0x10, 700u);
    put(set_address + 0x18, entries_address);
    for (unsigned index = 0; index < 700; ++index) {
        put(entries_address + index * 0x10, Address(index + 1));
        put(entries_address + index * 0x10 + 8, std::uint8_t(2));
    }
    MapScanCursor scan;
    unsigned visits{};
    const auto visit = [&](Address id) { assert(id == ++visits); };
    scan_map_entries(manager_address, scan, visit);
    assert(visits == MapEntriesPerUpdate && scan.entry == MapEntriesPerUpdate);
    scan_map_entries(manager_address, scan, visit);
    assert(visits == 700 && scan.entry == 0 && scan.set <= MapSetsPerUpdate);

    // Same map placement, different game addresses after reload.
    std::array<std::uint8_t, 0x200> first{}, reloaded{}, original_data{}, reloaded_data{};
    const auto first_actor = reinterpret_cast<Address>(first.data());
    const auto new_actor = reinterpret_cast<Address>(reloaded.data());
    const auto first_data = reinterpret_cast<Address>(original_data.data());
    const auto new_data = reinterpret_cast<Address>(reloaded_data.data());
    for (const auto actor : {first_actor, new_actor}) {
        put(actor + 0x3c, 0x3c223800u);
        put(actor + 0x1e8, 1042380205u);
    }
    put(first_actor + Handle, Key(91));
    put(new_actor + Handle, Key(92));
    Character npc{first_actor, first_data, 300, 300, 300, 300, 2000, 7, false};
    const Key placement = placement_key(npc);
    assert(placement);
    ++frame;
    enemy_tick(npc, 55, player, true);
    npc.hp = 0;
    npc.dead = true;
    ++frame;
    enemy_tick(npc, 55, player, true);
    assert(world.defeated.contains(placement));
    open_world(2, 123, 1, 1000);
    image = reinterpret_cast<Address>(&record_suppression) - ForceDeath;
    npc = {new_actor, new_data, 300, 300, 300, 300, 2000, 7, false};
    assert(placement_key(npc) == placement);
    ++frame;
    enemy_tick(npc, 55, player, false);
    assert(suppressed_character == 0);
    ++frame;
    enemy_tick(npc, 55, player, true);
    assert(suppressed_character == new_actor);

    suppressed_character = 0;
    put(new_actor + 0x1e8, 1042380800u);
    const Key boss_key = placement_key(npc);
    assert(script_controls_death(npc));
    world.defeated.insert(boss_key);
    ++frame;
    enemy_tick(npc, 55, player, true);
    assert(suppressed_character == 0);
    std::filesystem::remove(world.journal);

    // Level curve: wildlife gives nothing, enemies 1 point, bosses 20 points.
    assert(level_cost(1) == 5 && level_cost(10) == 5 && level_cost(14) == 5 && level_cost(15) == 6 &&
           level_cost(20) == 7 && level_cost(100) == 23 && level_cost(150) == 33);
    const auto stat_sum = [&]() {
        unsigned total{};
        for (unsigned i = 0; i < 8; ++i) total += get<unsigned>(player + StatStart + i * 4);
        return total;
    };
    for (unsigned i = 0; i < 8; ++i) put(player + StatStart + i * 4, 10u);
    put(player + Level, 10u);
    open_world(3, 123, 0, 0);
    put(first_actor + 0x1e8, 1042380205u);
    const Character critter{first_actor, first_data, 0, 300, 300, 300, 6010, 0, true, 60100000};
    assert(is_critter(6010) && is_critter(6200) && is_critter(5490) && !is_critter(6091) && !is_critter(6220));
    reward_kill(critter, player, 1, true);
    assert(get<unsigned>(player + Level) == 10 && world.kills == 0);
    const Character grunt{first_actor, first_data, 0, 300, 300, 300, 2000, 6, true, 20000000};
    assert(!is_boss(grunt));
    for (int i = 0; i < 4; ++i) reward_kill(grunt, player, 2, true);
    assert(get<unsigned>(player + Level) == 10 && world.progress == 4);
    reward_kill(grunt, player, 2, true); // Level 10 costs 5.
    assert(get<unsigned>(player + Level) == 11 && world.progress == 0);
    open_world(3, 123, 0, 0);
    assert(world.kills == 5 && world.progress == 0);
    reward_kill(grunt, player, 2, true);
    assert(world.kills == 6 && world.progress == 1);
    // An arena add with an 800-series entity is an ordinary enemy now.
    Character add = grunt;
    put(first_actor + 0x1e8, 1042380810u);
    assert(script_controls_death(add) && !is_boss(add));
    put(first_actor + 0x1e8, 1042380205u);
    Character boss = grunt;
    boss.npc_param = BossNpcParams[0];
    assert(is_boss(boss));
    // 1 + 20 points: levels 11, 12, 13 and 14 cost 5 each.
    reward_kill(boss, player, 3, true);
    assert(get<unsigned>(player + Level) == 15 && world.progress == 1 && world.kills == 6);
    assert(stat_sum() == 85);
    open_world(3, 123, 0, 0);
    assert(world.progress == 1);
    std::filesystem::remove(world.journal);

    // A level bought with runes is undone and the runes are returned.
    growth = {};
    put(player + Runes, 5000u);
    undo_rune_levels(player, 77);
    put(player + StatStart, get<unsigned>(player + StatStart) + 2);
    put(player + Level, 17u);
    put(player + Runes, 1200u);
    undo_rune_levels(player, 77);
    assert(get<unsigned>(player + Level) == 15 && stat_sum() == 85 && get<unsigned>(player + Runes) == 5000);
    // A reward level is kept.
    assert(add_random_stat(player, 4));
    undo_rune_levels(player, 77);
    assert(get<unsigned>(player + Level) == 16 && stat_sum() == 86);
    // A respec (same level, points moved) is kept.
    put(player + StatStart, get<unsigned>(player + StatStart) - 1);
    put(player + StatStart + 4, get<unsigned>(player + StatStart + 4) + 1);
    const unsigned respec_vigor = get<unsigned>(player + StatStart);
    undo_rune_levels(player, 77);
    assert(get<unsigned>(player + StatStart) == respec_vigor);
    // Picking up runes is not touched.
    put(player + Runes, 9000u);
    undo_rune_levels(player, 77);
    assert(get<unsigned>(player + Runes) == 9000);

    // Event flag storage lookup over an MSVC std::map shaped tree.
    std::array<std::uint8_t, 0x48> flag_manager{};
    std::array<std::uint8_t, 0x38> head{}, root{}, right{};
    const auto fm = reinterpret_cast<Address>(flag_manager.data());
    const auto h = reinterpret_cast<Address>(head.data());
    const auto r = reinterpret_cast<Address>(root.data());
    const auto rr = reinterpret_cast<Address>(right.data());
    put(fm + 0x1c, 1000u);
    put(fm + 0x20, 125u);
    put(fm + 0x38, h);
    put(h + 8, r);
    put(h + 0x19, std::uint8_t(1));
    put(r, h); put(r + 0x10, rr); put(r + 0x20, 1043317u); put(r + 0x28, 1u);
    put(rr, h); put(rr + 0x10, h); put(rr + 0x20, 1051577u); put(rr + 0x28, 2u);
    Address flag_manager_global = fm;
    image = reinterpret_cast<Address>(&flag_manager_global) - EventFlagMan;
    assert(flag_group_state(1043317) == 1);
    assert(flag_group_state(1051577) == 1);
    assert(flag_group_state(1032502) == 0);
    assert(flag_group_state(400) == 0);
    put(fm + 0x20, 100u);
    assert(flag_group_state(1043317) == -1);

    std::filesystem::remove(state_dir / L"random-leveling.log");
    std::filesystem::remove(state_dir);
    std::puts("Random Leveling runtime checks passed.");
}
