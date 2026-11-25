"""The player's own PlayStation 3 Soulcalibur V DLC on the Xbox 360 game (docs/GAME.md, "Unlocks and downloadable
content").

The 360 game's DLC packages hold a key and, for the costume packs, the data the disc lacks. The key is one index file,
index_NN_X360.dat, whose entries list the release ids (costume and creation parts, music, Dampierre) the package
grants; an entry may also name a file in the package (a costume part's model, or a file of the disc that the package
replaces while it is installed). The game keeps 30 package slots, DLC00 to DLC29, and takes an installed package into
the slot its display name names ("DLC01:DAMPIERRE"). The PS3 game's packs are the same keys in another form: a group's
index (USRDIR/DLCnn/index_nn_PS3.edat, in the group's free catalog pack or in each of its packs) lists all of the
group's entries, and each pack bought adds a license file, license_NN.edat, which only the buyer's license
(exdata/<content id>.rap) opens; the index's entries name the license file that grants them (a pack's set entry, whose
items it lists). The costume groups DLC06 to DLC13 keep their data in the group's free catalog pack ("Compatibility
Pack"), USRDIR/DLCnn/dat.cpk, one file per index entry (its id the entry's slot), in the PS3's formats.

This tool reads the player's PS3 packs and licenses and writes, for every group, one Xbox 360 package with the entries
that the player's licenses grant (a set and its items, a music track): PS3-DLCnn, an STFS package laid out as the game's
own DLC packages are (a LIVE header with the display name "DLCnn:...", the file index_nn_X360.dat). For the costume
groups the package also carries the data of those entries, converted to the Xbox 360's formats on this PC from the
player's own catalog pack (convert() below), each entry naming its file; the files of the disc a group replaces (the
creation parts tables, the parts' icons, their names) go only into the highest group written, whose tables list every
lower group's parts too (the game keeps the lowest group's copy of such a file). The player adds the packages on the
launcher's Build game page with their 360 packages (3. Your downloadable content packages, ADD...), and the game
installs them as it does those (sc5/src/sc5/dlc.cpp). A group that a 360 package of the player's in the output folder
already has is left to it. Nothing is granted without the license file the player's own .rap opens, and nothing of
the game's is in this tool: every byte it writes comes from the player's own packs.

    python -I tools/py/ps3dlc.py <PS3 .pkg folder> <exdata folder with the .rap licenses> <output folder>
        [--eboot <the PS3 game's EBOOT.elf>] [--klic <hex>] [--dry-run]

The free catalog packs (the indexes and data of DLC06 to DLC13) are opened with the game's own key, which the game
builds from a substitution table at run time; --eboot reads it from the game's decrypted executable: rpcs3.exe
--decrypt <USRDIR/EBOOT.BIN> (from the disc or the title update package) writes EBOOT.elf beside it. Without it only
the groups whose index comes in a paid pack are converted.
"""

import argparse
import collections
import glob
import hashlib
import hmac
import os
import struct
import sys
import time

from Crypto.Cipher import AES
from Crypto.Util import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpk  # noqa: E402  (tools/py/cpk.py: CRI's @UTF tables)

TITLES = ("BLUS30736", "BLES01250")  # Soulcalibur V, the US and the European disc

# PS3 retail package and NPDRM (EDAT) constants, as RPCS3 has them (rpcs3/Crypto/key_vault.h)
PKG_KEY_PS3 = bytes.fromhex("2E7B71D7C9C9A14EA3221F188828B8F8")
RAP_KEY = bytes.fromhex("869F7745C13FD890CCF29188E3CC3EDF")
RAP_PBOX = bytes.fromhex("0C030604010B0F0802070005" "0A0E0D09")
RAP_E1 = bytes.fromhex("A93E1FD67C55A329B75FDDA62A95C7A5")
RAP_E2 = bytes.fromhex("67D45DA3296D006A4E7C537BF5538C74")
EDAT_KEY_0 = bytes.fromhex("BE959CA8308DEFA2E5E180C63712A9AE")
EDAT_KEY_1 = bytes.fromhex("4CA9C14B01C95309969BEC68AA0BC081")


# ----------------------------------------------------------------------------- PS3 packages and EDAT files
def rap_to_rif(rap):
    """A .rap license's key (RPCS3's rap_to_rif)."""
    key = bytearray(AES.new(RAP_KEY, AES.MODE_CBC, iv=bytes(16)).decrypt(rap))
    for _ in range(5):
        for i in range(16):
            p = RAP_PBOX[i]
            key[p] ^= RAP_E1[p]
        for i in range(15, 0, -1):
            p, pp = RAP_PBOX[i], RAP_PBOX[i - 1]
            key[p] ^= key[pp]
        o = 0
        for i in range(16):
            p = RAP_PBOX[i]
            kc = (key[p] - o) & 0xFF
            ec2 = RAP_E2[p]
            if o != 1 or kc != 0xFF:
                o = 1 if kc < ec2 else 0
                key[p] = (kc - ec2) & 0xFF
            else:
                key[p] = kc
    return bytes(key)


def content_id(path):
    with open(path, "rb") as f:
        return f.read(0x60)[0x30:0x60].split(b"\0")[0].decode("latin1")


def pkg_files(path):
    """A retail PS3 package's files: [(name, data)] (folders left out)."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fPKG":
        raise ValueError(f"{path}: not a PS3 package")
    items = struct.unpack(">I", data[0x14:0x18])[0]
    d_off = struct.unpack(">Q", data[0x20:0x28])[0]
    riv = int.from_bytes(data[0x70:0x80], "big")

    def dec(off, size):
        start = off // 16
        c = AES.new(PKG_KEY_PS3, AES.MODE_CTR, counter=Counter.new(128, initial_value=riv + start))
        pad = off - start * 16
        return c.decrypt(data[d_off + start * 16:d_off + off + size])[pad:pad + size]

    table = dec(0, items * 32)
    out = []
    for i in range(items):
        no, ns, do, ds, flags, _ = struct.unpack(">IIQQII", table[i * 32:i * 32 + 32])
        if flags & 0xFF == 4:
            continue
        out.append((dec(no, ns).decode("latin1"), dec(do, ds)))
    return out


def edat_license(blob):
    """An EDAT's NPD license type: 2 a bought license (.rap), 3 the game's own key."""
    if blob[:4] != b"NPD\0":
        return None
    return struct.unpack(">i", blob[8:12])[0]


def edat_decrypt(blob, key):
    """An EDAT's data with the given key, or None when the key is not its key (the blocks' HMAC)."""
    version = struct.unpack(">i", blob[4:8])[0]
    digest, dev_hash = blob[0x40:0x50], blob[0x60:0x70]
    flags, block_size, file_size = struct.unpack(">iiQ", blob[0x80:0x90])
    if flags & 0x01000000 or flags & 0x01 or not flags & 0x20:
        raise ValueError("an EDAT kind this tool does not read (SDAT, compressed, or without per-block metadata)")
    ek = EDAT_KEY_1 if version == 4 else EDAT_KEY_0
    blocks = (file_size + block_size - 1) // block_size
    out = b""
    for b in range(blocks):
        meta_off = 0x100 + b * (0x20 + block_size)
        meta = blob[meta_off:meta_off + 0x20]
        test = bytes(meta[j] ^ meta[j + 0x10] for j in range(0x10)) + meta[0x10:0x14]
        length = block_size if b < blocks - 1 or file_size % block_size == 0 else file_size % block_size
        enc = blob[meta_off + 0x20:meta_off + 0x20 + (length + 15) // 16 * 16]
        e = AES.new(key, AES.MODE_ECB)
        key_result = e.encrypt((bytes(0xC) if version <= 1 else dev_hash[:0xC]) + struct.pack(">i", b))
        hash_ = e.encrypt(key_result) if flags & 0x10 else key_result
        if flags & 0x08:
            key_final = AES.new(ek, AES.MODE_ECB).decrypt(key_result)
            hash_final = AES.new(ek, AES.MODE_ECB).decrypt(hash_)
        else:
            key_final, hash_final = key_result, hash_
        if flags & 0x10:  # HMAC-SHA1 over the encrypted block
            if hmac.new(hash_final + bytes(4), enc, hashlib.sha1).digest() != test:
                return None
        out += AES.new(key_final, AES.MODE_CBC, iv=bytes(16) if version <= 1 else digest).decrypt(enc)[:length]
    return out


def klicensee_from_eboot(path):
    """The game's own key for its free EDAT files: the PS3 game builds it at run time, each of its 16 bytes taken
    through a substitution table (key[i] = table[source[i]], stored at +0x328 of its DRM object); both tables are
    found through the instructions that do it (lwz r6/r7 from the TOC, lbzx r0,r7,r9, stb r0,0x328(r10))."""
    elf = open(path, "rb").read()
    if elf[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF (decrypt EBOOT.BIN with rpcs3.exe --decrypt first)")
    phoff = struct.unpack(">Q", elf[0x20:0x28])[0]
    phentsize, phnum = struct.unpack(">HH", elf[0x36:0x3A])
    segs = []
    for i in range(phnum):
        ptype, pflags, off, vaddr, _, filesz, _, _ = struct.unpack(">IIQQQQQQ", elf[phoff + i * phentsize:][:56])
        if ptype == 1 and filesz:
            segs.append((off, vaddr, filesz, pflags))

    def rd(va, n):
        for off, vaddr, size, _ in segs:
            if vaddr <= va < vaddr + size:
                return elf[off + va - vaddr:off + va - vaddr + n]
        return None

    # the TOC: the most common one in the function descriptors
    tocs = collections.Counter()
    text = [s for s in segs if s[3] & 1]
    for off, vaddr, size, pflags in segs:
        if pflags & 1:
            continue
        blob = elf[off:off + size]
        for i in range(0, len(blob) - 8, 4):
            f, t = struct.unpack(">II", blob[i:i + 8])
            if any(v <= f < v + s for _, v, s, _ in text) and t:
                tocs[t] += 1
    toc = tocs.most_common(1)[0][0]
    for off, vaddr, size, _ in text:
        words = struct.unpack(">%dI" % (size // 4), elf[off:off + size // 4 * 4])
        for i, w in enumerate(words):
            if w != 0x980A0328:  # stb r0,0x328(r10)
                continue
            if words[i - 1] != 0x7C0748AE:  # lbzx r0,r7,r9
                continue
            slots = {}
            for back in range(2, 24):
                x = words[i - back]
                if x >> 26 == 32 and (x >> 16) & 31 == 2:  # lwz rD,d(r2)
                    rd_ = (x >> 21) & 31
                    d = x & 0xFFFF
                    slots.setdefault(rd_, toc + (d - 0x10000 if d & 0x8000 else d))
            if 6 in slots and 7 in slots:
                table = rd(struct.unpack(">I", rd(slots[7], 4))[0], 256)
                source = rd(struct.unpack(">I", rd(slots[6], 4))[0], 16)
                if table and source:
                    return bytes(table[b] for b in source)
    raise ValueError(f"{path}: the key's substitution was not found")


# ----------------------------------------------------------------------------- the catalog's data (CRI CPK)
def crilayla(data):
    """A file of a CRI archive as stored (CRILAYLA-compressed or plain) -> its data."""
    if data[:8] != b"CRILAYLA":
        return data
    usize, hoff = struct.unpack_from("<II", data, 8)
    out = bytearray(usize + 0x100)
    out[:0x100] = data[hoff + 0x10:hoff + 0x110]  # the first 0x100 bytes are stored plain, after the stream
    p = len(data) - 0x100 - 1  # the stream is read from its end backwards, each byte from its top bit
    stop = 0x10
    buf = 0
    n = 0
    dst = usize + 0x100 - 1  # and written from the end backwards
    while dst >= 0x100:
        if n < 24:  # enough bits for anything but the long lengths
            while n <= 56 and p >= stop:
                buf = (buf << 8) | data[p]
                p -= 1
                n += 8
            buf &= (1 << n) - 1
        n -= 1
        if not (buf >> n) & 1:  # a byte
            n -= 8
            out[dst] = (buf >> n) & 0xFF
            dst -= 1
            continue
        n -= 13  # a copy of what is already written: 13 bits of distance, then the length in 2, 3, 5, 8, 8... bits
        ref = dst + ((buf >> n) & 0x1FFF) + 3
        length = 3
        for w in (2, 3, 5, 8):
            if n < w:
                while n <= 56 and p >= stop:
                    buf = (buf << 8) | data[p]
                    p -= 1
                    n += 8
            n -= w
            v = (buf >> n) & ((1 << w) - 1)
            length += v
            if v != (1 << w) - 1:
                break
        else:
            while True:
                if n < 8:
                    while n <= 56 and p >= stop:
                        buf = (buf << 8) | data[p]
                        p -= 1
                        n += 8
                n -= 8
                v = (buf >> n) & 0xFF
                length += v
                if v != 255:
                    break
        if length > dst - 0xFF or n < 0:
            raise ValueError("CRILAYLA: a damaged stream")
        d = ref - dst
        if d >= length:
            out[dst - length + 1:dst + 1] = out[ref - length + 1:ref + 1]
            dst -= length
        else:  # the copy overlaps what it writes: it repeats every d bytes
            while length:
                k = d if d < length else length
                out[dst - k + 1:dst + 1] = out[ref - k + 1:ref + 1]
                dst -= k
                ref -= k
                length -= k
    return bytes(out)


def cpk_files(blob):
    """An ID-only CRI archive's files (an ITOC, no TOC: the catalog packs' dat.cpk): {id: stored bytes}."""
    if blob[:4] != b"CPK ":
        raise ValueError("not a CRI archive")
    hdr = cpk.utf_table(blob, 0x10)[0]
    itoc = blob[hdr["ItocOffset"]:hdr["ItocOffset"] + hdr["ItocSize"]]
    it = cpk.utf_table(itoc, 0x10)[0]
    files = []
    for key in ("DataL", "DataH"):
        cell = it.get(key)
        if cell and cell[1]:
            files += [(r["ID"], r["FileSize"]) for r in cpk.utf_table(itoc, cell[0])]
    files.sort()
    off, align = hdr["ContentOffset"], hdr.get("Align") or 1
    out = {}
    for fid, size in files:
        out[fid] = blob[off:off + size]
        off += size
        if off % align:
            off += align - off % align
    return out


# ----------------------------------------------------------------------------- PS3 data -> Xbox 360 data
# The rules come from the two discs, file by file (docs/GAME.md, "The player's PS3 DLC"): the PS3 file converted is the
# 360 disc's byte for byte, but for a few files the 360 disc has in another revision and models of two material types
# no DLC uses (refused below). A value the rules do not cover raises ValueError: nothing is guessed.
NUT_FORMATS = {0: 0, 1: 1, 2: 2, 14: 19, 17: 20}  # pixel formats: DXT1, DXT3, DXT5 kept; two 32-bit ones renumbered
NUT_SWAPPED = {0, 1, 2}  # DXT data: every 16-bit word swapped
WRAP = {1: 0, 2: 1, 3: 2}  # the PS3's wrap, mirror and clamp to edge as the 360 numbers them
MAG = {1: 0, 2: 1}  # magnification: nearest, linear -> point, linear
MIN = {1: (0, 2), 2: (1, 2), 4: (1, 0), 6: (1, 1)}  # the PS3's minification (mip filter included) -> 360 min, mip
CULL = {0x0000: 0x0000, 0x0405: 0x0002, 0x0404: 0x0006}  # none, back, front


def _bundle(d):
    """A bundle's (offsets, parts): u32 count, count + 1 offsets, the parts. None for anything else."""
    if len(d) < 8:
        return None
    n = struct.unpack_from(">I", d, 0)[0]
    if not 0 < n < 200 or 4 + 4 * (n + 1) > len(d):
        return None
    offs = struct.unpack_from(f">{n + 1}I", d, 4)
    if list(offs) != sorted(offs) or offs[-1] > len(d) or offs[0] < 4 + 4 * n:
        return None
    return offs, [d[offs[i]:offs[i + 1]] for i in range(n)]


def _swap16(b):
    out = bytearray(b)
    out[0::2], out[1::2] = b[1::2], b[0::2]
    return out


def convert_nut(b):
    """Textures (Namco NUT): NTP3 -> NTXR."""
    out = bytearray(b)
    out[0:4] = b"NTXR"
    version, count = struct.unpack_from(">HH", b, 4)
    if version != 0x0100:
        raise ValueError(f"textures of version {version:#x}")
    p = 0x10
    for _ in range(count):
        total, _, data_size, header_size = struct.unpack_from(">IIIH", b, p)
        fmt = b[p + 0x13]
        if fmt not in NUT_FORMATS or total < header_size + data_size or p + total > len(b):
            raise ValueError(f"a texture of pixel format {fmt} or a damaged header")
        out[p + 0x13] = NUT_FORMATS[fmt]
        if fmt in NUT_SWAPPED:
            d = p + header_size
            out[d:d + data_size] = _swap16(b[d:d + data_size])
        p += total
    return bytes(out)


def _color_offset(vert_type, uv_type):
    """Where a vertex's colour (4 bytes) lies in the vertex clump, and the clump's vertex size; None: no colour."""
    color, uvs = uv_type & 0x0F, uv_type >> 4
    if color == 0:
        return None
    if color != 2:
        raise ValueError(f"vertex colours of type {color}")
    tail = 4 + 4 * uvs  # the colour (u8 x4), the UVs (half x2 each)
    if vert_type & 0xF0:  # skinned: position, normals and weights in the other clump
        return 0, tail
    normals = {0: 0, 6: 8, 7: 24}.get(vert_type & 0x0F)
    if normals is None:
        raise ValueError(f"vertex normals of type {vert_type & 0x0F}")
    return 12 + normals, 12 + normals + tail


def convert_nud(b):
    """Meshes (Namco NUD): NDP3 -> NDXR: the materials' render states and samplers in the 360's numbers, the vertex
    colours byte-reversed."""
    out = bytearray(b)
    out[0:4] = b"NDXR"
    polysets = struct.unpack_from(">H", b, 0xA)[0]
    clump = struct.unpack_from(">I", b, 0x10)[0] + 0x30
    index_size, vertex_size = struct.unpack_from(">II", b, 0x14)
    vertices = clump + index_size
    seen = set()
    for k in range(polysets):
        count, start = struct.unpack_from(">HI", b, 0x30 + k * 0x30 + 0x2A)
        for j in range(count):
            q = start + j * 0x30
            _, vstart, _, vcount, vert_type, uv_type = struct.unpack_from(">IIIHBB", b, q)
            for m in struct.unpack_from(">IIII", b, q + 0x10):
                if not m or m in seen:
                    continue
                seen.add(m)
                if b[m] not in (0x24, 0x28):
                    raise ValueError(f"a material of type {b[m]:#x}")
                alpha, cull = struct.unpack_from(">H", b, m + 0xE)[0], struct.unpack_from(">H", b, m + 0x12)[0]
                if alpha and not 0x0200 <= alpha <= 0x0207 or cull not in CULL:
                    raise ValueError(f"a material's alpha test {alpha:#x} or cull mode {cull:#x}")
                struct.pack_into(">H", out, m + 0xE, alpha - 0x0200 if alpha else 0)  # GL's compare -> the 360's
                struct.pack_into(">H", out, m + 0x12, CULL[cull])
                for t in range(struct.unpack_from(">H", b, m + 0xA)[0]):
                    e = m + 0x20 + t * 0x18
                    ws, wt, wr, mag, mn = b[e + 0xC:e + 0x11]
                    if (ws not in WRAP or wt not in WRAP or wr != 3 or mag not in MAG or mn not in MIN or
                            b[e + 0x11:e + 0x18] != b"\0\0\0\0\0\x1f\0"):
                        raise ValueError(f"a texture sampler {b[e + 0xC:e + 0x18].hex()}")
                    out[e + 0xC:e + 0x12] = bytes([WRAP[ws], WRAP[wt], 2, MAG[mag], *MIN[mn]])
                    out[e + 0x14:e + 0x18] = b"\xbf\x80\0\0"  # LOD bias -1.0
            col = _color_offset(vert_type, uv_type)
            if col:
                at, stride = col
                if vstart + vcount * stride > vertex_size:
                    raise ValueError("vertices past their clump")
                for v in range(vertices + vstart + at, vertices + vstart + vcount * stride, stride):
                    out[v:v + 4] = b[v:v + 4][::-1]
    return bytes(out)


def convert(d):
    """One file of the game's data (or a part of a bundle), PS3 -> Xbox 360. Models (NMD), tables, text and the rest
    are the same on both."""
    head = d[:4]
    if head == b"NTP3":
        return convert_nut(d)
    if head == b"NDP3":
        return convert_nud(d)
    bundle = _bundle(d)
    if bundle:
        offs, parts = bundle
        return d[:offs[0]] + b"".join(convert(p) for p in parts) + d[offs[-1]:]
    return d


# ----------------------------------------------------------------------------- the DLC index
Entry = collections.namedtuple("Entry", "group slot a ids1 x8 x10 ids2 flag cid lic")


def parse_index(d):
    """A PS3 index (version 1, or a catalog's version 2 with a leading zero word): (slot count, entries). An entry's x8
    and x10 are an archive and a file of the disc that the entry's data replaces (x8 0xFFFF: none)."""
    p = 0

    def take(fmt):
        nonlocal p
        v = struct.unpack_from(fmt, d, p)
        p += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]

    first = take(">I")
    if first == 0:
        take(">I")  # the version, 2
    take(">I")  # the ids, all entries together
    size = take(">I")
    strings = d[p:p + size]
    p += size

    def string(o):
        return strings[o:strings.index(b"\0", o)].decode("latin1") if o else ""

    slots, count = take(">HH")
    entries = []
    for _ in range(count):
        group, slot, a = take(">HHB")
        n1 = take(">H")
        ids1 = [take(">H") for _ in range(n1)]
        x8, x10 = take(">HH")
        n2 = take(">H")
        ids2 = [take(">H") for _ in range(n2)]
        _, flag, cid_off, lic_off, _ = take(">IBIIB")
        entries.append(Entry(group, slot, a, ids1, x8, x10, ids2, flag, string(cid_off), string(lic_off)))
    if p != len(d):
        raise ValueError(f"index: {len(d) - p} bytes left over")
    return slots, entries


def x360_index(slots, entries, files=None):
    """The 360 game's index (sub_82398158, sub_82397F28): version 1, the names of the entries' files (strings, offset
    0 none), each entry's tail the offset of its file's name, a zero offer id and license bit 0 (the package's license
    mask, every bit for the player's own packages)."""
    files = files or {}
    strings = bytearray(b"\0")
    name_at = {}
    for e in entries:
        if e.slot in files:
            name_at[e.slot] = len(strings)
            strings += files[e.slot].encode("ascii") + b"\0"
    out = struct.pack(">III", 1, sum(len(e.ids1) + len(e.ids2) for e in entries), len(strings)) + bytes(strings)
    out += struct.pack(">HH", slots, len(entries))
    for e in entries:
        out += struct.pack(">HHB", e.group, e.slot, e.a)
        out += struct.pack(">H%dH" % len(e.ids1), len(e.ids1), *e.ids1)
        out += struct.pack(">HH", e.x8, e.x10)
        out += struct.pack(">H%dH" % len(e.ids2), len(e.ids2), *e.ids2)
        out += struct.pack(">IBQI", name_at.get(e.slot, 0), e.flag, 0, 1)
    return out


# ----------------------------------------------------------------------------- Xbox 360 packages (STFS)
def _backing(block):
    """Where data block `block` lies among a package's blocks, past the hash tables before it (one table per level:
    free60 / Velocity's ComputeBackingDataBlockNumber, launcher/src/X360.cpp)."""
    r = (block + 0xAA) // 0xAA + block
    if block >= 0xAA:
        r += (block + 0x70E4) // 0x70E4
    if block >= 0x70E4:
        r += 1
    return r


def _level0_at(table):
    """Where level-0 hash table `table` (data blocks 170 * table on) lies."""
    block = table * 0xAA
    if block < 0xAA:
        return 0
    r = (block // 0xAA) * 0xAB + block // 0x70E4 + 1
    return r + 1 if block >= 0x70E4 else r


def stfs_package(path, display, files):
    """Writes an Xbox 360 package (STFS) of the game's downloadable content holding `files` ([(name, data)], at its
    root) to `path`, laid out as the game's own DLC packages are (launcher/src/X360.cpp reads them): a LIVE header with
    its metadata (version 1, no pictures), the file table in the first blocks, then each file's blocks one after
    another, with hash tables of up to three levels between them. Its hashes are a console's (each block's in its
    level-0 table, each table's in the level above, the top table's in the volume descriptor, the header's from 0x344 to
    the first block); the signature is left empty, which neither the launcher nor the runtime reads."""
    header_size = 0x971A
    first = (header_size + 0xFFF) & ~0xFFF  # where the blocks start
    table_blocks = max(1, (len(files) + 0x3F) // 0x40)
    table = bytearray(table_blocks * 0x1000)
    chains = [(0, table_blocks)]  # each chain's first block and length: the file table's, then each file's
    start = table_blocks
    for i, (name, blob) in enumerate(files):
        raw = name.encode("ascii")
        n = (len(blob) + 0xFFF) // 0x1000
        if not n or len(raw) > 0x28:
            raise ValueError(f"{name}: empty, or too long a name")
        e = memoryview(table)[i * 0x40:(i + 1) * 0x40]
        e[0:len(raw)] = raw
        e[0x28] = 0x40 | len(raw)  # its blocks follow each other
        e[0x29:0x2C] = n.to_bytes(3, "little")
        e[0x2C:0x2F] = n.to_bytes(3, "little")
        e[0x2F:0x32] = start.to_bytes(3, "little")
        struct.pack_into(">hI", e, 0x32, -1, len(blob))  # in the root; its size
        chains.append((start, n))
        start += n
    count = start
    if count > 0xAA * 0x70E4:
        raise ValueError("a package of more than 20 GB")
    blocks = bytearray(table)
    for _, blob in files:
        blocks += blob
        blocks += bytes(-len(blob) % 0x1000)
    nxt = {}
    for s, n in chains:
        for b in range(s, s + n):
            nxt[b] = b + 1 if b < s + n - 1 else 0xFFFFFF
    # the hash tables, level 0 up: each entry the SHA-1 of a block (or of a table below), level 0's also "in use" and
    # the next block of its chain
    level0 = []
    for t in range((count + 0xA9) // 0xAA):
        h = bytearray(0x1000)
        for i, b in enumerate(range(t * 0xAA, min(count, (t + 1) * 0xAA))):
            h[i * 0x18:i * 0x18 + 0x14] = hashlib.sha1(blocks[b * 0x1000:(b + 1) * 0x1000]).digest()
            h[i * 0x18 + 0x14] = 0x80
            h[i * 0x18 + 0x15:i * 0x18 + 0x18] = nxt[b].to_bytes(3, "big")
        level0.append(bytes(h))

    def above(tables):
        out = []
        for t in range((len(tables) + 0xA9) // 0xAA):
            h = bytearray(0x1000)
            for i, low in enumerate(tables[t * 0xAA:(t + 1) * 0xAA]):
                h[i * 0x18:i * 0x18 + 0x14] = hashlib.sha1(low).digest()
            out.append(bytes(h))
        return out

    level1 = above(level0) if count > 0xAA else []
    level2 = above(level1) if count > 0x70E4 else []
    top = (level2 or level1 or level0)[0]
    # where everything lies
    place = {}
    for b in range(count):
        place[_backing(b)] = ("data", b)
    for t, h in enumerate(level0):
        place[_level0_at(t)] = ("table", h)
    for t, h in enumerate(level1):
        place[0xAB if t == 0 else 1 + t * 0x718F] = ("table", h)
    if level2:
        place[0x718F] = ("table", level2[0])
    backing = max(place) + 1
    if len(place) != backing:
        raise ValueError("the package's blocks overlap (a bug)")
    h = bytearray(first)
    h[0:4] = b"LIVE"
    struct.pack_into(">QII", h, 0x22C, 0xFFFFFFFFFFFFFFFF, 1, 1)  # licensed to every profile, as the game's own are
    struct.pack_into(">IIIQ", h, 0x340, header_size, 0x00000002, 1, backing * 0x1000)  # marketplace content
    struct.pack_into(">I", h, 0x360, 0x4E4D083D)  # Soulcalibur V
    # the STFS volume descriptor: its size, block separation 1 (one table per hash block), the file table's block count
    # (u16 LE) and first block (u24 LE), the top hash table's hash, the blocks in use and free
    h[0x379:0x37C] = bytes([0x24, 0, 1])
    h[0x37C:0x37E] = table_blocks.to_bytes(2, "little")
    h[0x37E:0x381] = (0).to_bytes(3, "little")
    h[0x381:0x395] = hashlib.sha1(top).digest()
    struct.pack_into(">II", h, 0x395, count, 0)
    name = display.encode("utf-16-be")[:0xFE]
    h[0x411:0x411 + len(name)] = name
    title = "SOULCALIBUR V".encode("utf-16-be")
    h[0x1691:0x1691 + len(title)] = title
    h[0x1711] = 0xC0  # transfer flags, as the game's own
    h[0x32C:0x340] = hashlib.sha1(h[0x344:first]).digest()
    with open(path, "wb") as f:
        f.write(h)
        for i in range(backing):
            kind, what = place[i]
            f.write(what if kind == "table" else blocks[what * 0x1000:(what + 1) * 0x1000])
    return backing * 0x1000 + first


def display_name(path):
    """A 360 package's display name, or that of an installed one's header (beside its files); "" for anything else."""
    try:
        with open(path, "rb") as f:
            h = f.read(0x511)
    except OSError:
        return ""
    if len(h) < 0x511 or h[:4] not in (b"LIVE", b"PIRS", b"CON "):
        return ""
    return h[0x411:0x511].decode("utf-16-be", "replace").split("\0")[0]


# ----------------------------------------------------------------------------- main
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("packages", help="the folder with the PS3 .pkg files")
    ap.add_argument("exdata", help="the folder with the .rap licenses")
    ap.add_argument("out", help="the folder to write the 360 packages to (the one with your own 360 packages, say)")
    ap.add_argument("--eboot", help="the PS3 game's decrypted EBOOT.elf (for the free catalogs)")
    ap.add_argument("--klic", help="the game's own key, hex (instead of --eboot)")
    ap.add_argument("--dry-run", action="store_true", help="say what would be written")
    a = ap.parse_args(argv)

    klic = bytes.fromhex(a.klic) if a.klic else klicensee_from_eboot(a.eboot) if a.eboot else None
    owned = collections.defaultdict(dict)  # group -> license file -> content id
    indexes = {}  # group -> decrypted index (the first that opens)
    catalog = {}  # group -> its catalog pack's data (dat.cpk), the costume groups'
    no_license = []
    for path in sorted(glob.glob(os.path.join(a.packages, "*.pkg"))):
        cid = content_id(path)
        if not any(t in cid for t in TITLES):
            continue
        rap_path = os.path.join(a.exdata, cid + ".rap")
        rap = open(rap_path, "rb").read() if os.path.exists(rap_path) else b""
        rif = rap_to_rif(rap) if len(rap) == 16 else None
        opened = has_license = False
        for name, blob in pkg_files(path):
            parts = name.split("/")
            if len(parts) != 3 or parts[0] != "USRDIR" or not parts[1].startswith("DLC"):
                continue
            group = int(parts[1][3:])
            if parts[2] == "dat.cpk":
                catalog.setdefault(group, blob)
                continue
            if not name.endswith(".edat"):
                continue
            has_license |= parts[2].startswith("license_")
            kind = edat_license(blob)
            key = klic if kind == 3 else rif
            data = edat_decrypt(blob, key) if key else None
            if data is None:
                continue
            if parts[2].startswith("license_"):
                owned[group][parts[2]] = cid
                opened = True
            elif parts[2].startswith("index_") and group not in indexes:
                indexes[group] = data
        if has_license and not opened:
            no_license.append(cid)

    if any(os.path.isdir(p) for p in glob.glob(os.path.join(a.out, "PS3-DLC*"))):
        print(f"{a.out} is a game's dlc folder (what the launcher installed): write the packages to another folder, "
              "then add them on the launcher's Build game page")
        return 2
    os.makedirs(a.out, exist_ok=True)
    taken_by_360 = set()  # the groups the player's own 360 packages there have
    for path in glob.glob(os.path.join(a.out, "*")):
        if os.path.basename(path).startswith("PS3-DLC") or not os.path.isfile(path):
            continue
        name = display_name(path)
        if name.startswith("DLC") and name[3:5].isdigit():
            taken_by_360.add(int(name[3:5]))

    plans = []  # (group, slots, chosen entries, packs)
    for group in sorted(set(owned) | set(indexes)):
        if group not in indexes:
            print(f"DLC{group:02d}: licenses {sorted(owned[group])} but no index (its catalog pack, or --eboot)")
            continue
        if group in taken_by_360:
            print(f"DLC{group:02d}: the player's own 360 package has it, left to that")
            continue
        slots, entries = parse_index(indexes[group])
        by_slot = {e.slot: e for e in entries}
        grant = set()
        packs = []
        for e in entries:
            if e.lic and e.lic in owned[group]:
                grant.add(e.slot)
                packs.append(e.cid.split("-")[-1])
                grant.update(s for s in e.ids1 if s in by_slot)  # a set: its items
        if not grant:
            continue
        if group in catalog:  # the group's own data that no license names (its parts' icons, a table)
            grant.update(e.slot for e in entries
                         if not e.lic and not e.ids1 and not e.ids2 and e.x8 == 0xFFFF and e.slot not in grant)
        plans.append((group, slots, entries, grant, packs))
    # the files of the disc that the costume groups replace (the creation parts tables, the parts' icons and names): only
    # the highest group's, whose tables list every lower group's parts as well; the game keeps the lowest group's
    top = max((g for g, *_ in plans if g in catalog), default=None)
    written = []
    total = 0
    for group, slots, entries, grant, packs in plans:
        chosen = [e for e in entries if e.slot in grant or (group == top and e.x8 != 0xFFFF)]
        ids = sum(len(e.ids2) for e in chosen)
        pkg = f"PS3-DLC{group:02d}"
        display = f"DLC{group:02d}:PS3 " + " ".join(packs)
        files = {}
        blobs = []
        if group in catalog:
            started = time.time()
            data = cpk_files(catalog[group])
            keep = []
            for e in chosen:
                if e.slot not in data:
                    keep.append(e)
                    continue
                try:
                    blob = convert(crilayla(data[e.slot]))
                except ValueError as err:
                    print(f"  DLC{group:02d} slot {e.slot}: its data could not be converted ({err}): left out")
                    continue
                files[e.slot] = f"{e.slot:03d}.dat"
                blobs.append((files[e.slot], blob))
                keep.append(e)
            chosen = keep
            print(f"{pkg}: {len(blobs)} files of data converted in {time.time() - started:.0f} s "
                  f"({sum(len(b) for _, b in blobs) / 1e6:.0f} MB)")
        replaced = sum(1 for e in chosen if e.x8 != 0xFFFF)
        print(f"{pkg}: {len(packs)} packs, {len(chosen)} entries, {ids} release ids"
              + (f", {replaced} files of the disc replaced" if replaced else "") + f"  ({display[:100]})")
        if not a.dry_run:
            index = (f"index_{group:02d}_X360.dat", x360_index(slots, chosen, files))
            total += stfs_package(os.path.join(a.out, pkg), display[:127], [index] + blobs)
        written.append(pkg)
    for group in sorted(set(owned) & set(indexes)):
        if group in (g for g, *_ in plans) and group not in catalog and any(
                e.x8 != 0xFFFF for e in parse_index(indexes[group])[1]):
            print(f"DLC{group:02d}: its costumes' data is in the group's free catalog pack, which is not here: "
                  "the package keys them only")
    # a pack whose license opens in neither region's copy (a missing or wrong .rap)
    have = {c.split("-")[-1] for g in owned.values() for c in g.values()}
    for cid in no_license:
        if cid.split("-")[-1] not in have:
            print(f"no license opens {cid}: its .rap is missing or belongs to another pack")
    print(f"{len(written)} packages {'would be ' if a.dry_run else ''}written to {a.out}"
          + (f" ({total / 1e6:.0f} MB)" if total else ""))
    if written and not a.dry_run:
        print("Add them on the launcher's Build game page (3. Your downloadable content packages, ADD...), with your "
              "360 packages, then CHECK AND BUILD.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
