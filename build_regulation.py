"""Build and verify the Elden Ring rules that can be encoded in regulation.bin."""

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
TOOLS = ROOT / ".build" / "elden-ring-super-randomizer" / "tools"
sys.path[:0] = [str(ROOT / ".build" / "pylibs"), str(TOOLS)]

import erreg
import oodle

SLOTS = range(1, 9)
EQUIPMENT = {2, 3, 4}  # Weapons, armor, and talismans.
SLOT_FIELDS = ("lotItemId", "lotItemCategory", "lotItemBasePoint", "lotItemNum",
               "getItemFlagId", "cumulateLotPoint", "enableLuck", "cumulateReset")
LOT_FIELD = "itemLotId_enemy"
MAP_LOT_FIELD = "itemLotId_map"
# The game stores an event flag only when its 1000-flag group has storage in
# the save. Group 1032502 (tile m60_32_50) has none, so flags there were never
# kept and equipment dropped again. Use the unused top of overworld treasure
# groups (10XXYY7xxx) that vanilla already uses for item pickups.
FLAG_GROUP_COUNT = 6
FLAG_LOCAL_RANGE = range(999, 899, -1)


def flag_pool(used_flags):
    counts = Counter(flag // 1000 for flag in used_flags
                     if 1_000_000_000 <= flag < 1_100_000_000 and (flag // 1000) % 10 == 7)
    groups = [group for group, _ in sorted(counts.items(), key=lambda item: (-item[1], item[0]))]
    groups = groups[:FLAG_GROUP_COUNT]
    if len(groups) < FLAG_GROUP_COUNT:
        raise ValueError("Not enough vanilla treasure flag groups")
    return groups, (group * 1000 + local for group in groups for local in FLAG_LOCAL_RANGE
                    if group * 1000 + local not in used_flags)


def field(name, slot):
    return f"{name}{slot:02}"


def read_slot(lots, row, slot):
    return tuple(lots.get(row, field(name, slot)) for name in SLOT_FIELDS)


def write_slot(lots, row, slot, values):
    for name, value in zip(SLOT_FIELDS, values, strict=True):
        lots.set(row, field(name, slot), value)


def consecutive_rows(lots, start):
    while start in lots.by_id:
        yield lots.by_id[start]
        start += 1


def existing_flags(*tables):
    for table in tables:
        for row in table.rows:
            for name in ("getItemFlagId", *(field("getItemFlagId", slot) for slot in SLOTS)):
                value = table.get(row, name)
                if value:
                    yield value
            cumulative = table.get(row, "cumulateNumFlagId")
            if cumulative:
                yield from range(cumulative, cumulative + 8)


def patch_equipment(npcs, enemy, world, ammunition):
    used_flags = set(existing_flags(enemy, world))
    flag_groups, flag_ids = flag_pool(used_flags)
    item_flags = {}
    enemy_groups = {}
    redirected = {}
    skipped_flagged = set()
    bank_size = 100_000
    occupied = {row_id // bank_size for row_id in world.by_id}
    original_map = set(world.by_id)
    new_lot = next(bank * bank_size + 1 for bank in range(8000, 9999) if bank not in occupied)
    limit = new_lot + bank_size - 2

    for npc in npcs.rows:
        start = npcs.get(npc, LOT_FIELD)
        if start <= 0 or start not in enemy.by_id:
            continue
        if start not in enemy_groups:
            replacements = []
            equipment = {}
            for row in consecutive_rows(enemy, start):
                slots = [read_slot(enemy, row, slot) for slot in SLOTS]
                sum_weights = sum(slot[2] for slot in slots)
                random_gear = [i for i, slot in enumerate(slots) if
                               slot[0] > 0 and slot[1] in EQUIPMENT and
                               0 < slot[2] < sum_weights and slot[3] > 0]
                if (enemy.get(row, "getItemFlagId") or
                        enemy.get(row, "cumulateNumFlagId") or any(slots[i][4] for i in random_gear)):
                    if random_gear:
                        skipped_flagged.add(row.id)
                    random_gear = []

                removed = []
                for i in random_gear:
                    item, category, weight, quantity, *_ = slots[i]
                    key = (category, item)
                    equipment[key] = max(equipment.get(key, 0), quantity)
                    if category != 2 or item not in ammunition:
                        removed.append(i)
                replacements.append((row, slots, removed))
            for key in sorted(equipment):
                if key not in item_flags:
                    item_flags[key] = next(flag_ids)
            gear = {key: quantity for key, quantity in equipment.items()
                    if key[0] != 2 or key[1] not in ammunition}
            enemy_groups[start] = replacements, gear

        replacements, equipment = enemy_groups[start]
        if not equipment:
            continue
        old_map_start = npcs.get(npc, MAP_LOT_FIELD)
        group = (start, old_map_start)
        if group not in redirected:
            destination = new_lot
            if old_map_start in original_map and old_map_start > 0:
                for row in list(consecutive_rows(world, old_map_start)):
                    world.add_row(new_lot, row)
                    new_lot += 1

            # NpcParam accepts only one loot source. Move the existing rolls
            # along with the guaranteed items to map lots, which honor the
            # game's one-time acquisition flags.
            for row, slots, removed in replacements:
                if removed and not any(slot[0] > 0 and slot[2] and i not in removed
                                       for i, slot in enumerate(slots)):
                    continue
                clone = world.add_row(new_lot, row)
                new_lot += 1
                for i in removed:
                    _, _, weight, _, _, _, luck, _ = slots[i]
                    write_slot(world, clone, i + 1, (0, 0, weight, 0, 0, 0, luck, 0))

            for (category, item), quantity in sorted(equipment.items()):
                row = world.add_row(new_lot, replacements[0][0])
                for slot in SLOTS:
                    write_slot(world, row, slot, (0,) * len(SLOT_FIELDS))
                world.set(row, "cumulateNumFlagId", 0)
                world.set(row, "cumulateNumMax", 0)
                world.set(row, "getItemFlagId", item_flags[(category, item)])
                write_slot(world, row, 1, (item, category, 1000, quantity, 0, 0, 0, 0))
                new_lot += 1
            redirected[group] = (destination, tuple(sorted(equipment)))
            new_lot += 1
            if new_lot > limit:
                raise ValueError("Map item-lot bank exhausted")

        npcs.set(npc, LOT_FIELD, -1)
        npcs.set(npc, MAP_LOT_FIELD, redirected[group][0])

    used_groups = sorted({flag // 1000 for flag in item_flags.values()})
    if not set(used_groups) <= set(flag_groups):
        raise ValueError("Item flag outside the chosen flag groups")
    return redirected, {key: flag for key, flag in item_flags.items()
                        if key[0] != 2 or key[1] not in ammunition}, sorted(skipped_flagged), used_groups


def write_boss_list(npcs, path=ROOT / "runtime" / "boss_npcs.inc"):
    """NpcParam rows whose runes are awarded as a boss reward."""
    ids = sorted(row.id for row in npcs.rows if npcs.get(row, "isSoulGetByBoss"))
    lines = [f"    {', '.join(str(i) for i in ids[n:n + 8])}," for n in range(0, len(ids), 8)]
    path.write_text("// Generated by build_regulation.py from NpcParam.isSoulGetByBoss.\n"
                    + "\n".join(lines) + "\n", encoding="utf-8")
    return len(ids)


def load_tables(reg):
    defs = TOOLS / "paramdef"
    npc_def = erreg.load_paramdef(defs / "NpcParam.xml")
    lot_def = erreg.load_paramdef(defs / "ItemLotParam.xml")
    return (reg.param("NpcParam", npc_def), reg.param("ItemLotParam_enemy", lot_def),
            reg.param("ItemLotParam_map", lot_def))


def verify(reg, built, redirects, item_flags):
    a_npcs, _, a_world = load_tables(reg)
    b_npcs, _, b_world = load_tables(built)
    for before, after in ((a_npcs, b_npcs), (a_world, b_world)):
        if [(r.id, r.data) for r in before.rows] != [(r.id, r.data) for r in after.rows]:
            raise ValueError(f"{before.param_type} changed during serialization")

    unchanged = 0
    for index, file in enumerate(reg.files):
        if file.name.rsplit("\\", 1)[-1] not in {"NpcParam.param", "ItemLotParam_map.param"}:
            if reg.bnd.datas[index] != built.bnd.datas[index]:
                raise ValueError(f"Unrelated param changed: {file.name}")
            unchanged += 1

    for start, gear in redirects.values():
        if not gear:
            continue
        rows = list(consecutive_rows(b_world, start))[-len(gear):]
        for row, (category, item) in zip(rows, gear, strict=True):
            if b_world.get(row, "getItemFlagId") != item_flags[(category, item)]:
                raise ValueError(f"Wrong one-time item flag in row {row.id}")
            if read_slot(b_world, row, 1)[:3] != (item, category, 1000):
                raise ValueError(f"Wrong guaranteed drop in row {row.id}")
            if any(read_slot(b_world, row, i)[2] for i in range(2, 9)):
                raise ValueError(f"Competing drop outcome in row {row.id}")
    return unchanged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "random-leveling-assets" / "regulation.bin")
    args = parser.parse_args()

    source = (args.game / "regulation.bin").resolve()
    target = args.output.resolve()
    if source == target:
        parser.error("The output must be separate from the installed regulation.bin")
    if not oodle.use_dll(args.game / "oo2core_6_win64.dll"):
        parser.error("Cannot load the game's Oodle compression library")

    regulation = erreg.Regulation(source)
    npcs, enemy, world = load_tables(regulation)
    teams = Counter(npcs.get(row, "teamType") for row in npcs.rows)
    boss_npcs = write_boss_list(npcs)
    # Character team types 6 and 7 are the two ordinary hostile NPC teams.
    rest_changes = 0
    for npc in npcs.rows:
        if npcs.get(npc, "teamType") in {6, 7} and not npcs.get(npc, "disableRespawn"):
            npcs.set(npc, "disableRespawn", 1)
            rest_changes += 1
    weapons = regulation.param("EquipParamWeapon", erreg.load_paramdef(TOOLS / "paramdef" / "EquipParamWeapon.xml"))
    ammunition = {row.id for row in weapons.rows if weapons.get(row, "weaponCategory") in {13, 14}}
    redirects, item_flags, skipped, flag_groups = patch_equipment(npcs, enemy, world, ammunition)
    (ROOT / "runtime" / "item_flag_groups.inc").write_text(
        "// Generated by build_regulation.py: event-flag groups holding one-time equipment flags.\n"
        + "".join(f"    {group},\n" for group in flag_groups), encoding="utf-8")

    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(".tmp")
    try:
        regulation.save(temporary, codec=erreg.CODEC_KRAKEN)
        unchanged = verify(regulation, erreg.Regulation(temporary), redirects, item_flags)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    report = {
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "output_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "equipment_items": len(item_flags),
        "patched_equipment_lots": sum(bool(gear) for _, gear in redirects.values()),
        "left_unchanged_flagged_or_cumulative_lots": skipped,
        "npc_rest_respawn_disabled": rest_changes,
        "boss_npc_rows": boss_npcs,
        "item_flag_groups": flag_groups,
        "unchanged_binder_entries": unchanged,
        "npc_team_distribution": dict(sorted(teams.items())),
    }
    (ROOT / "build-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
