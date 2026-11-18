"""The player's own PlayStation 3 Soulcalibur V DLC on the Xbox 360 game (docs/GAME.md, "Unlocks and downloadable
content").

The 360 game's DLC packages hold no content, only a key: one index file, index_NN_X360.dat, whose entries list the
release ids (costume and creation parts, music, Dampierre) the package grants; everything is on the disc. The game keeps
30 package slots, DLC00 to DLC29, and takes an installed package into the slot its display name names
("DLC01:DAMPIERRE"). The PS3 game's packs are the same keys in another form: a group's index
(USRDIR/DLCnn/index_nn_PS3.edat, in the group's free catalog pack or in each of its packs) lists all of the group's
entries, and each pack bought adds a license file, license_NN.edat, which only the buyer's license
(exdata/<content id>.rap) opens; the index's entries name the license file that grants them (a pack's set entry,
whose items it lists).

This tool reads the player's PS3 packs and licenses and writes, for every group, one Xbox 360 package with the entries
that the player's licenses grant (a set and its items, a music track): PS3-DLCnn, an STFS package laid out as the game's
own DLC packages are (a LIVE header with the display name "DLCnn:...", one file, index_nn_X360.dat). The player adds
them on the launcher's Build game page with their 360 packages (3. Your downloadable content packages, ADD...), and the
game installs them as it does those (sc5/src/sc5/dlc.cpp). A group that a 360 package of the player's in the output
folder already has is left to it. Nothing is granted without the license file the player's own .rap opens.

    python -I tools/py/ps3dlc.py <PS3 .pkg folder> <exdata folder with the .rap licenses> <output folder>
        [--eboot <the PS3 game's EBOOT.elf>] [--klic <hex>] [--dry-run]

The free catalog packs (the indexes of DLC06 to DLC13) are opened with the game's own key, which the game builds from a
substitution table at run time; --eboot reads it from the game's decrypted executable: rpcs3.exe --decrypt
<USRDIR/EBOOT.BIN> (from the disc or the title update package) writes EBOOT.elf beside it. Without it only the groups
whose index comes in a paid pack are converted.
"""

import argparse
import collections
import glob
import hashlib
import hmac
import os
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Util import Counter

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


# ----------------------------------------------------------------------------- the DLC index
Entry = collections.namedtuple("Entry", "group slot a ids1 x8 x10 ids2 flag cid lic")


def parse_index(d):
    """A PS3 index (version 1, or a catalog's version 2 with a leading zero word): (slot count, entries)."""
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


def x360_index(slots, entries):
    """The 360 game's index (sub_82388990, sub_82388760): version 1, no strings, each entry's tail a zero offer id and
    license bit 0 (the package's license mask, every bit for the player's own packages)."""
    out = struct.pack(">III", 1, sum(len(e.ids1) + len(e.ids2) for e in entries), 1) + b"\0"
    out += struct.pack(">HH", slots, len(entries))
    for e in entries:
        out += struct.pack(">HHB", e.group, e.slot, e.a)
        out += struct.pack(">H%dH" % len(e.ids1), len(e.ids1), *e.ids1)
        out += struct.pack(">HH", e.x8, e.x10)
        out += struct.pack(">H%dH" % len(e.ids2), len(e.ids2), *e.ids2)
        out += struct.pack(">IBQI", 0, e.flag, 0, 1)
    return out


def stfs_package(display, files):
    """An Xbox 360 package (STFS) of the game's downloadable content holding `files` ([(name, data)], at its root), laid
    out as the game's own DLC packages are (launcher/src/X360.cpp reads them): a LIVE header with its metadata (version
    1, no pictures), one hash table, the file table in block 0, then each file's blocks one after another. Its hashes
    are a console's (each block's in the hash table, the table's in the volume descriptor, the header's from 0x344 to
    the first block); the signature is left empty, which neither the launcher nor the runtime reads."""
    header_size = 0x971A
    first = (header_size + 0xFFF) & ~0xFFF  # where the hash table starts, the blocks after it
    table = bytearray(0x1000)
    data = []
    last = {0}  # each chain's last block: the file table's, then each file's
    start = 1
    for i, (name, blob) in enumerate(files):
        raw = name.encode("ascii")
        n = (len(blob) + 0xFFF) // 0x1000
        if not n or len(raw) > 0x28 or i >= 0x40:
            raise ValueError(f"{name}: empty, too long a name, or more files than one file table block lists")
        e = memoryview(table)[i * 0x40:(i + 1) * 0x40]
        e[0:len(raw)] = raw
        e[0x28] = 0x40 | len(raw)  # its blocks follow each other
        e[0x29:0x2C] = n.to_bytes(3, "little")
        e[0x2C:0x2F] = n.to_bytes(3, "little")
        e[0x2F:0x32] = start.to_bytes(3, "little")
        struct.pack_into(">hI", e, 0x32, -1, len(blob))  # in the root; its size
        data.append(blob + bytes(n * 0x1000 - len(blob)))
        start += n
        last.add(start - 1)
    blocks = bytes(table) + b"".join(data)
    count = len(blocks) // 0x1000
    if count > 0xAA:
        raise ValueError("more blocks than one hash table covers")
    hashes = bytearray(0x1000)
    for b in range(count):
        o = b * 0x18
        hashes[o:o + 0x14] = hashlib.sha1(blocks[b * 0x1000:(b + 1) * 0x1000]).digest()
        hashes[o + 0x14] = 0x80  # in use
        hashes[o + 0x15:o + 0x18] = (0xFFFFFF if b in last else b + 1).to_bytes(3, "big")  # the chain's next block
    h = bytearray(first)
    h[0:4] = b"LIVE"
    struct.pack_into(">QII", h, 0x22C, 0xFFFFFFFFFFFFFFFF, 1, 1)  # licensed to every profile, as the game's own are
    struct.pack_into(">IIIQ", h, 0x340, header_size, 0x00000002, 1, 0x1000 + len(blocks))  # marketplace content
    struct.pack_into(">I", h, 0x360, 0x4E4D083D)  # Soulcalibur V
    # the STFS volume descriptor: its size, block separation 1 (one hash table per level), the file table's block count
    # (u16 LE) and first block (u24 LE), the hash table's hash, the blocks in use and free
    h[0x379:0x381] = bytes([0x24, 0, 1, 1, 0, 0, 0, 0])
    h[0x381:0x395] = hashlib.sha1(hashes).digest()
    struct.pack_into(">II", h, 0x395, count, 0)
    name = display.encode("utf-16-be")[:0xFE]
    h[0x411:0x411 + len(name)] = name
    title = "SOULCALIBUR V".encode("utf-16-be")
    h[0x1691:0x1691 + len(title)] = title
    h[0x1711] = 0xC0  # transfer flags, as the game's own
    h[0x32C:0x340] = hashlib.sha1(h[0x344:first]).digest()
    return bytes(h) + bytes(hashes) + blocks


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
    no_license = []
    for path in sorted(glob.glob(os.path.join(a.packages, "*.pkg"))):
        cid = content_id(path)
        if not any(t in cid for t in TITLES):
            continue
        rap_path = os.path.join(a.exdata, cid + ".rap")
        rap = open(rap_path, "rb").read() if os.path.exists(rap_path) else b""
        rif = rap_to_rif(rap) if len(rap) == 16 else None
        opened = False
        for name, blob in pkg_files(path):
            parts = name.split("/")
            if len(parts) != 3 or parts[0] != "USRDIR" or not parts[1].startswith("DLC") or not name.endswith(".edat"):
                continue
            group = int(parts[1][3:])
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
        if not opened and any(n.split("/")[-1].startswith("license_") for n, _ in pkg_files(path)):
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

    written = []
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
        chosen = [e for e in entries if e.slot in grant]
        if not chosen:
            continue
        ids = sum(len(e.ids2) for e in chosen)
        pkg = f"PS3-DLC{group:02d}"
        display = f"DLC{group:02d}:PS3 " + " ".join(packs)
        print(f"{pkg}: {len(packs)} packs, {len(chosen)} entries, {ids} release ids  ({display[:100]})")
        if not a.dry_run:
            with open(os.path.join(a.out, pkg), "wb") as f:
                f.write(stfs_package(display[:127], [(f"index_{group:02d}_X360.dat", x360_index(slots, chosen))]))
        written.append(pkg)
    # a pack whose license opens in neither region's copy (a missing or wrong .rap)
    have = {c.split("-")[-1] for g in owned.values() for c in g.values()}
    for cid in no_license:
        if cid.split("-")[-1] not in have:
            print(f"no license opens {cid}: its .rap is missing or belongs to another pack")
    print(f"{len(written)} packages {'would be ' if a.dry_run else ''}written to {a.out}")
    if written and not a.dry_run:
        print("Add them on the launcher's Build game page (3. Your downloadable content packages, ADD...), with your "
              "360 packages, then CHECK AND BUILD.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
