#!/usr/bin/env python3
"""Writes data/sql/db-world/mod_nemesis_world.sql from a world database.

For every open-world rare it makes two copies: the hunter (ranks 1-4) and its final form
(rank 5). A copy has the rare's name with a title, its models and weapons and its loot table.
Its stats come from the module at run time, so no stats are copied.

Entries already in the output file keep their numbers: the client caches creature names by
entry, so moving a name to another entry would show players the wrong one.

    pip install pymysql
    DB_USER=... DB_PASS=... python3 tools/build_creatures.py [--host 127.0.0.1] [--port 3306]

Released under the MIT License.
"""

import argparse
import os
import pathlib
import re

import pymysql

HUNTER_BASE = 9600000
FINAL_BASE = 9601000
RANGE_SIZE = 1000
RANK_RARE_ELITE = 2
RANK_RARE = 4
FACTION_HOSTILE_TO_ALL = 14
FLAG_DONT_CALL_ASSISTANCE = 0x02000000  # only the first call; the script stops the later ones
SCRIPT_NAME = "npc_mod_nemesis"
OPEN_WORLD_MAPS = (0, 1, 530, 571)
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "data/sql/db-world/mod_nemesis_world.sql"

TEMPLATE_COLUMNS = (
    "entry", "name", "subname", "minlevel", "maxlevel", "faction", "npcflag", "speed_walk", "speed_run",
    "`rank`", "dmgschool", "DamageModifier", "BaseAttackTime", "RangeAttackTime", "unit_class", "unit_flags",
    "unit_flags2", "family", "type", "type_flags", "lootid", "AIName", "MovementType", "HoverHeight",
    "HealthModifier", "ManaModifier", "ArmorModifier", "ExperienceModifier", "RegenHealth", "flags_extra",
    "ScriptName",
)


def sql_string(text):
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def titled(name, title):
    """'Narg the Taskmaster' -> 'Narg the Vengeful'; 'Ash'adar, Herald' -> 'Ash'adar the Vengeful'."""
    base = re.split(r" the |, ", name, maxsplit=1)[0].strip()
    return f"{base} the {title}"


def known_entries():
    """rare -> offset, from the mapping rows of the file written last time."""
    if not OUTPUT.exists():
        return {}
    pattern = re.compile(r"^\((\d+), (\d+), (\d+)\)[,;]$", re.M)
    return {int(rare): int(hunter) - HUNTER_BASE for rare, hunter, _ in pattern.findall(OUTPUT.read_text())}


def template_row(entry, rare, title, rank, lootid):
    level = max(1, rare["maxlevel"])
    values = (
        entry, sql_string(titled(rare["name"], title)), "'Nemesis'", level, level, FACTION_HOSTILE_TO_ALL, 0, 1,
        1.14286, rank, rare["dmgschool"], 1, 2000, 2000, 1, 0, 2048, rare["family"], rare["type"], 0,
        lootid, "''", 0, 1, 1, 1, 1, 1, 1, FLAG_DONT_CALL_ASSISTANCE, sql_string(SCRIPT_NAME),
    )
    return "(" + ", ".join(str(value) for value in values) + ")"


def loot_tables_with_mounts(cursor, lootids):
    """The loot tables that can drop a mount, directly or through references.

    A copy can be killed again and again, so a rare's mount must not come along with its loot.
    """
    cursor.execute("SELECT entry FROM item_template WHERE class = 15 AND subclass = 5")
    mounts = {row["entry"] for row in cursor.fetchall()}
    cursor.execute("SELECT Entry, Item, Reference FROM reference_loot_template")
    references = {}
    for row in cursor.fetchall():
        references.setdefault(row["Entry"], []).append(row)

    def has_mount(rows, seen):
        for row in rows:
            if row["Reference"]:
                if row["Reference"] not in seen:
                    seen.add(row["Reference"])
                    if has_mount(references.get(row["Reference"], []), seen):
                        return True
            elif row["Item"] in mounts:
                return True
        return False

    ids = ",".join(str(lootid) for lootid in lootids)
    cursor.execute(f"SELECT Entry, Item, Reference FROM creature_loot_template WHERE Entry IN ({ids})")
    tables = {}
    for row in cursor.fetchall():
        tables.setdefault(row["Entry"], []).append(row)
    return {entry for entry, rows in tables.items() if has_mount(rows, set())}


def insert(lines, head, rows, chunk=200):
    for start in range(0, len(rows), chunk):
        lines.append(f"INSERT INTO {head} VALUES")
        lines.append(",\n".join(rows[start:start + chunk]) + ";")
        lines.append("")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3306)
    parser.add_argument("--database", default="acore_world")
    args = parser.parse_args()

    db = pymysql.connect(host=args.host, port=args.port, user=os.environ["DB_USER"], password=os.environ["DB_PASS"],
                         database=args.database, charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor)
    with db.cursor() as cursor:
        maps = ",".join(str(map_id) for map_id in OPEN_WORLD_MAPS)
        cursor.execute(f"""
            SELECT DISTINCT t.entry, t.name, t.maxlevel, t.dmgschool, t.family, t.type, t.lootid
            FROM creature c JOIN creature_template t ON t.entry = c.id
            WHERE t.`rank` IN ({RANK_RARE}, {RANK_RARE_ELITE}) AND c.map IN ({maps}) AND (c.phaseMask & 1)
              AND NOT (t.flags_extra & 128) AND t.entry NOT BETWEEN {HUNTER_BASE} AND {FINAL_BASE + RANGE_SIZE - 1}
            ORDER BY t.entry""")
        rares = [rare for rare in cursor.fetchall()
                 if rare["name"] and rare["name"][0] != "[" and "DND" not in rare["name"] and "(PH)" not in rare["name"]]

        ids = ",".join(str(rare["entry"]) for rare in rares)
        cursor.execute(f"""SELECT CreatureID, Idx, CreatureDisplayID, DisplayScale, Probability
                           FROM creature_template_model WHERE CreatureID IN ({ids}) ORDER BY CreatureID, Idx""")
        models = cursor.fetchall()
        cursor.execute(f"""SELECT CreatureID, ID, ItemID1, ItemID2, ItemID3
                           FROM creature_equip_template WHERE CreatureID IN ({ids}) ORDER BY CreatureID, ID""")
        weapons = cursor.fetchall()
        mount_loot = loot_tables_with_mounts(cursor, {rare["lootid"] for rare in rares if rare["lootid"]})

    offsets = known_entries()
    next_offset = max(offsets.values(), default=-1) + 1
    for rare in rares:
        if rare["entry"] not in offsets:
            offsets[rare["entry"]] = next_offset
            next_offset += 1
    if next_offset > RANGE_SIZE:
        raise SystemExit(f"{next_offset} rares do not fit the {RANGE_SIZE} entries of the range")

    mapping, templates, model_rows, weapon_rows = [], [], [], []
    for rare in rares:
        offset = offsets[rare["entry"]]
        hunter, final = HUNTER_BASE + offset, FINAL_BASE + offset
        mapping.append(f"({rare['entry']}, {hunter}, {final})")
        lootid = rare["lootid"]
        if lootid in mount_loot:
            print(f"no loot for the copies of {rare['name']} ({rare['entry']}): its table {lootid} can drop a mount")
            lootid = 0
        templates.append(template_row(hunter, rare, "Vengeful", RANK_RARE, lootid))
        templates.append(template_row(final, rare, "Undying", RANK_RARE_ELITE, lootid))
    for model in models:
        offset = offsets[model["CreatureID"]]
        for base in (HUNTER_BASE, FINAL_BASE):
            model_rows.append(f"({base + offset}, {model['Idx']}, {model['CreatureDisplayID']}, "
                              f"{model['DisplayScale']:g}, {model['Probability']:g}, 0)")
    for weapon in weapons:
        offset = offsets[weapon["CreatureID"]]
        for base in (HUNTER_BASE, FINAL_BASE):
            weapon_rows.append(f"({base + offset}, {weapon['ID']}, {weapon['ItemID1']}, "
                               f"{weapon['ItemID2']}, {weapon['ItemID3']}, 0)")

    low, high = HUNTER_BASE, FINAL_BASE + RANGE_SIZE - 1
    lines = [
        "-- mod-nemesis: generated by tools/build_creatures.py, do not edit by hand.",
        "-- A hunter and a final form for each open-world rare. Stats come from the module at run time.",
        "",
        "CREATE TABLE IF NOT EXISTS `mod_nemesis_creature` (",
        "  `rare_entry` int unsigned NOT NULL COMMENT 'creature_template of the rare',",
        "  `hunter_entry` int unsigned NOT NULL COMMENT 'copy for ranks 1 to 4',",
        "  `final_entry` int unsigned NOT NULL COMMENT 'copy for the last rank',",
        "  PRIMARY KEY (`rare_entry`)",
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-nemesis: which copies belong to which rare';",
        "",
        "DELETE FROM `mod_nemesis_creature`;",
        f"DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN {low} AND {high};",
        f"DELETE FROM `creature_equip_template` WHERE `CreatureID` BETWEEN {low} AND {high};",
        f"DELETE FROM `creature_template` WHERE `entry` BETWEEN {low} AND {high};",
        "",
    ]
    insert(lines, "`mod_nemesis_creature` (`rare_entry`, `hunter_entry`, `final_entry`)", mapping)
    insert(lines, "`creature_template` (" + ", ".join(TEMPLATE_COLUMNS) + ")", templates)
    insert(lines, "`creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, "
                  "`Probability`, `VerifiedBuild`)", model_rows)
    insert(lines, "`creature_equip_template` (`CreatureID`, `ID`, `ItemID1`, `ItemID2`, `ItemID3`, `VerifiedBuild`)",
           weapon_rows)
    OUTPUT.write_text("\n".join(lines))
    print(f"{len(rares)} rares, {len(templates)} creatures, {len(model_rows)} models, {len(weapon_rows)} weapon rows")


if __name__ == "__main__":
    main()
