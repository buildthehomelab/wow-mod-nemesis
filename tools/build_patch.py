#!/usr/bin/env python3
"""Adds the Feat of Strength and titles of a nemesis to your server's client patch.

    python3 build_patch.py --patch patch-z80-spells-and-items.MPQ --out new/patch-z80-spells-and-items.MPQ

Achievement.dbc gets row 9510 and CharTitles.dbc gets rows 191 and 192 with title bits 156 and 157
(the 3.3.5a client stops at 177 and bit 142; the core keeps 192 title bits). The patch has to hold both files already:
a patch MPQ replaces whole files, so everything else is carried over unchanged. New rows go at the
end, and a rerun rewrites ours where they are, so running it on its own output changes nothing.
data/sql/db-world/mod_nemesis_world_rewards.sql is the server's copy of these rows.

Released under the MIT License.
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcmpq  # noqa: E402
from dbcmpq import add_string, read_dbc, write_dbc  # noqa: E402

ACHIEVEMENT = 9510
TITLES = [(191, 156, "%s, Bane of the Undying"), (192, 157, "%s the Nemesis Hunter")]
NAME = "Bane of the Undying"
DESCRIPTION = "Hunted by your nemesis, defeat it at its last rank."
REWARD = "Title Reward: <Name>, Bane of the Undying"
ICON = 538
UI_ORDER = 1100
CATEGORY_FEATS_OF_STRENGTH = 81
LANG_MASK = 16712190                       # the enUS string plus the flags every stock row has
UNSET = 0xFFFFFFFF                         # -1: either faction, any map
ACHIEVEMENT_FIELDS, CHARTITLES_FIELDS = 62, 37
A_FACTION, A_INSTANCE, A_TITLE, A_DESCRIPTION = 1, 2, 4, 21
A_CATEGORY, A_UI_ORDER, A_ICON, A_REWARD = 38, 40, 42, 43
T_NAME, T_NAME1, T_MASK_ID = 2, 19, 36


def find_file(files, name):
    for archived, local in files.items():
        if archived.lower() == name.lower():
            return local
    sys.exit("the patch has no %s" % name)


def put(rows, row):
    for i, existing in enumerate(rows):
        if existing[0] == row[0]:
            rows[i] = row
            return
    rows.append(row)


def patch_achievements(path):
    rows, strings, fields = read_dbc(path)
    if fields != ACHIEVEMENT_FIELDS:
        sys.exit("Achievement.dbc has %d fields, not %d" % (fields, ACHIEVEMENT_FIELDS))
    row = [0] * fields
    row[0] = ACHIEVEMENT
    row[A_FACTION] = UNSET
    row[A_INSTANCE] = UNSET
    row[A_TITLE] = add_string(strings, NAME)
    row[A_TITLE + 16] = LANG_MASK
    row[A_DESCRIPTION] = add_string(strings, DESCRIPTION)
    row[A_DESCRIPTION + 16] = LANG_MASK
    row[A_CATEGORY] = CATEGORY_FEATS_OF_STRENGTH
    row[A_UI_ORDER] = UI_ORDER
    row[A_ICON] = ICON
    row[A_REWARD] = add_string(strings, REWARD)
    row[A_REWARD + 16] = LANG_MASK
    put(rows, row)
    write_dbc(path, rows, strings, fields)


def patch_titles(path):
    rows, strings, fields = read_dbc(path)
    if fields != CHARTITLES_FIELDS:
        sys.exit("CharTitles.dbc has %d fields, not %d" % (fields, CHARTITLES_FIELDS))
    ours = {title_id for title_id, _bit, _text in TITLES}
    bits = {bit for _title_id, bit, _text in TITLES}
    taken = [r[0] for r in rows if r[0] not in ours and r[T_MASK_ID] in bits]
    if taken:
        sys.exit("CharTitles.dbc rows %s already use our title bits" % taken)
    for title_id, bit, text in TITLES:
        row = [0] * fields
        row[0] = title_id
        row[T_NAME] = add_string(strings, text)
        row[T_NAME + 16] = LANG_MASK
        row[T_NAME1] = add_string(strings, text)
        row[T_NAME1 + 16] = LANG_MASK
        row[T_MASK_ID] = bit
        put(rows, row)
    write_dbc(path, rows, strings, fields)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--patch", required=True, help="your server's client patch with Achievement.dbc and CharTitles.dbc")
    parser.add_argument("--out", required=True, help="the new patch")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        files = dbcmpq.unpack(args.patch, tmp)
        patch_achievements(find_file(files, "DBFilesClient\\Achievement.dbc"))
        patch_titles(find_file(files, "DBFilesClient\\CharTitles.dbc"))
        dbcmpq.pack(args.out, [(local, archived) for archived, local in sorted(files.items())])
        print("%s: %d files, %.1f MB" % (args.out, len(files), os.path.getsize(args.out) / 1e6))


if __name__ == "__main__":
    main()
