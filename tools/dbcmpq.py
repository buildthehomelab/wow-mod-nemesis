"""DBC and MPQ helpers shared by the tools: plain WDBC files, and patch MPQs through StormLib
(`STORMLIB`, default /usr/local/lib/libstorm.dylib). From wow-mod-retail-pets' build_pets.py."""

import ctypes
import os
import struct
import sys

LOCALE_MPQS = ["patch-{0}-3.MPQ", "patch-{0}-2.MPQ", "patch-{0}.MPQ", "locale-{0}.MPQ"]


# --- DBC ---------------------------------------------------------------------------------------

def read_dbc(path):
    data = open(path, "rb").read()
    magic, count, fields, size, strsize = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC" or size != fields * 4:
        sys.exit("%s isn't a plain WDBC file" % path)
    rows = [list(struct.unpack_from("<%dI" % fields, data, 20 + i * size)) for i in range(count)]
    strings = bytearray(data[20 + count * size:20 + count * size + strsize])
    return rows, strings, fields


def write_dbc(path, rows, strings, fields):
    """Rows in the order given, never re-sorted: the client reads Achievement.dbc in file order (see
    series_breaks)."""
    with open(path, "wb") as f:
        f.write(struct.pack("<4s4I", b"WDBC", len(rows), fields, fields * 4, len(strings)))
        for r in rows:
            f.write(struct.pack("<%dI" % fields, *r))
        f.write(strings)


def string_at(strings, at):
    return strings[at:strings.index(b"\0", at)].decode("utf-8")


def add_string(strings, text):
    """Offset of text in the string block, appended unless it is already there as a whole string."""
    raw = text.encode("utf-8") + b"\0"
    at = strings.find(b"\0" + raw)
    if at >= 0:
        return at + 1
    at = len(strings)
    strings.extend(raw)
    return at


def template(rows, row_id, what):
    for r in rows:
        if r[0] == row_id:
            return list(r)
    sys.exit("%s %d missing" % (what, row_id))


def upsert(rows, row):
    rows[:] = [r for r in rows if r[0] != row[0]]
    rows.append(row)


# --- Achievement series ------------------------------------------------------------------------

UNSET = 0xFFFFFFFF                           # -1: either faction
ACHIEVEMENT_SUPERCEDES, ACHIEVEMENT_FACTION, ACHIEVEMENT_CATEGORY, ACHIEVEMENT_FLAGS = 3, 1, 38, 41
ACHIEVEMENT_HIDDEN = 0x2 | 0x800             # never listed / listed only once earned
CATEGORY_FEATS_OF_STRENGTH = 81              # lists earned rows only


def place_stage(rows, row):
    """Inserts an Achievement.dbc row where the client's series logic needs it: a row that follows
    another (Supercedes) right after that row's run of later stages, any other row at the end."""
    before = row[ACHIEVEMENT_SUPERCEDES]
    at = next((i for i, r in enumerate(rows) if r[0] == before), None) if before else None
    if at is None:
        rows.append(row)
        return
    while at + 1 < len(rows) and rows[at + 1][ACHIEVEMENT_SUPERCEDES] == rows[at][0]:
        at += 1
    rows.insert(at + 1, row)


def series_breaks(rows):
    """(faction, achievement, the row listed before it) for every later stage the achievement window
    would list on its own. Wow.exe (GetCategoryNumAchievements 0x5B38E0, GetAchievementInfo 0x5B3FC0)
    hides a series' unfinished stages in one walk over Achievement.dbc in file order: a stage stays
    hidden only if its predecessor is the row it listed last in that category. So every stage has to
    come right after its predecessor among its category's rows, as Blizzard's file has it; sorted by
    id, about 54 stages of other series show at once on a new character."""
    ids = {r[0] for r in rows}
    found = []
    for faction in (0, 1):                   # Horde, Alliance
        last = {}
        for r in rows:
            if r[ACHIEVEMENT_FACTION] not in (UNSET, faction) or r[ACHIEVEMENT_FLAGS] & ACHIEVEMENT_HIDDEN \
                    or r[ACHIEVEMENT_CATEGORY] == CATEGORY_FEATS_OF_STRENGTH:
                continue
            before = r[ACHIEVEMENT_SUPERCEDES]
            if before in ids and last.get(r[ACHIEVEMENT_CATEGORY]) != before:
                found.append((faction, r[0], last.get(r[ACHIEVEMENT_CATEGORY])))
            last[r[ACHIEVEMENT_CATEGORY]] = r[0]
    return found


# --- MPQ ---------------------------------------------------------------------------------------

def stormlib():
    lib = ctypes.CDLL(os.environ.get("STORMLIB", "/usr/local/lib/libstorm.dylib"))
    H = ctypes.c_void_p
    lib.SFileOpenArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]
    lib.SFileExtractFile.argtypes = [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    lib.SFileHasFile.argtypes = [H, ctypes.c_char_p]
    lib.SFileFindFirstFile.argtypes = [H, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_char_p]
    lib.SFileFindFirstFile.restype = H
    lib.SFileFindNextFile.argtypes = [H, ctypes.c_void_p]
    lib.SFileFindClose.argtypes = [H]
    lib.SFileCreateArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]
    lib.SFileAddFileEx.argtypes = [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
    lib.SFileCloseArchive.argtypes = [H]
    for fn in (lib.SFileOpenArchive, lib.SFileExtractFile, lib.SFileHasFile, lib.SFileFindNextFile,
               lib.SFileFindClose, lib.SFileCreateArchive, lib.SFileAddFileEx, lib.SFileCloseArchive):
        fn.restype = ctypes.c_bool
    return lib, H


def open_mpq(lib, H, mpq):
    h = H()
    if not lib.SFileOpenArchive(mpq.encode(), 0, 0x100, ctypes.byref(h)):
        sys.exit("can't open " + mpq)
    return h


def unpack(mpq, folder):
    """Extracts every file; returns {archive name: local path}."""
    lib, H = stormlib()
    h = open_mpq(lib, H, mpq)
    names = []
    found = ctypes.create_string_buffer(2048)  # SFILE_FIND_DATA starts with the name (1024 bytes)
    find = lib.SFileFindFirstFile(h, b"*", found, None)
    while find:
        name = found.value.decode("latin-1")
        if name not in ("(listfile)", "(attributes)", "(signature)"):
            names.append(name)
        if not lib.SFileFindNextFile(find, found):
            lib.SFileFindClose(find)
            break
    files = {}
    for i, name in enumerate(names):
        local = os.path.join(folder, "%05d" % i)
        if not lib.SFileExtractFile(h, name.encode("latin-1"), local.encode(), 0):
            sys.exit("can't extract %s from %s" % (name, mpq))
        files[name] = local
    lib.SFileCloseArchive(h)
    if not files:
        sys.exit("%s has no files (or no listfile)" % mpq)
    return files


def extract(mpq, name, dst):
    lib, H = stormlib()
    h = open_mpq(lib, H, mpq)
    ok = lib.SFileExtractFile(h, name.encode("latin-1"), dst.encode(), 0)
    lib.SFileCloseArchive(h)
    return ok


def client_file(client, locale, name, dst):
    """The client's newest copy of a locale file (the later locale patches load over the earlier)."""
    folder = os.path.join(client, "Data", locale)
    for pattern in LOCALE_MPQS:
        mpq = os.path.join(folder, pattern.format(locale))
        if os.path.exists(mpq) and extract(mpq, name, dst):
            return mpq
    sys.exit("no %s in %s's locale MPQs" % (name, folder))


def pack(out, files):
    """files: [(local path, archive name)]. zlib-compressed, with listfile and attributes."""
    lib, H = stormlib()
    if os.path.exists(out):
        os.remove(out)
    h = H()
    if not lib.SFileCreateArchive(out.encode(), 0x00300000, max(64, len(files) * 2), ctypes.byref(h)):
        sys.exit("can't create " + out)
    for src, name in files:
        if not lib.SFileAddFileEx(h, src.encode(), name.encode("latin-1"), 0x80000200, 0x02, 0x02):
            lib.SFileCloseArchive(h)
            os.remove(out)
            sys.exit("can't add " + name)
    if not lib.SFileCloseArchive(h):
        os.remove(out)
        sys.exit("can't finish " + out)
