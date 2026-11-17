"""Xbox 360 disc images, STFS packages (title updates, DLC) and XEX headers.

  python tools/py/x360.py iso-list    <image.iso>
  python tools/py/x360.py iso-extract <image.iso> <out_dir> [--only default.xex ...] [--no-system-update]
  python tools/py/x360.py stfs-info   <package>
  python tools/py/x360.py stfs-extract <package> <out_dir>
  python tools/py/x360.py dlc-install <package> <dlc_dir>   the launcher's game/dlc layout: <dlc_dir>/<package>/ with
                                                          the package's files, <dlc_dir>/<package>.header its header
  python tools/py/x360.py xex-info    <default.xex | default.xexp>

Disc: GDFX file system (XGD1/XGD2/XGD3 or a plain XISO), same reader as mk9-rollback/tools/extract_iso.py.
STFS: LIVE/PIRS/CON packages (a title update is a LIVE package of content type 0x000B0000).
"""
import argparse
import hashlib
import os
import struct
import sys

# ----------------------------------------------------------------------------- disc (GDFX)
SECTOR = 2048
GDFX_MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = (0xFD90000, 0x18300000, 0x2080000, 0x0)   # XGD2, XGD1, XGD3, plain XISO
ATTR_DIRECTORY = 0x10


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        for base in PARTITIONS:
            self.f.seek(base + 32 * SECTOR)
            if self.f.read(20) == GDFX_MAGIC:
                self.base = base
                break
        else:
            raise ValueError("not an Xbox 360 disc image (no GDFX volume descriptor)")
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def _dir(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        data = self.f.read(size)
        out, stack, seen = [], [0] if size else [], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            out.append((data[off + 14:off + 14 + nlen].decode("latin-1"), start, length, attr))
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)
        return out

    def walk(self, sector=None, size=None, prefix=""):
        sector = self.root_sector if sector is None else sector
        size = self.root_size if size is None else size
        for name, start, length, attr in sorted(self._dir(sector, size), key=lambda e: e[0].lower()):
            p = prefix + name
            if attr & ATTR_DIRECTORY:
                yield p, start, length, True
                yield from self.walk(start, length, p + "/")
            else:
                yield p, start, length, False

    def read_file(self, start, length, chunk=8 << 20):
        self.f.seek(self.base + start * SECTOR)
        left = length
        while left:
            b = self.f.read(min(left, chunk))
            if not b:
                raise IOError("short read")
            left -= len(b)
            yield b


# ----------------------------------------------------------------------------- STFS packages
BLOCK = 0x1000
CONTENT_TYPES = {0x000B0000: "title update", 0x00000002: "marketplace content (DLC)", 0x00000001: "saved game",
                 0x00080000: "game demo", 0x00030000: "theme", 0x00004000: "installed game", 0x00007000: "game on demand"}


def _ver(v):
    return f"{(v >> 28) & 0xF}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def _u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def _u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x971A + 0x1000)
        self.magic = h[:4]
        if self.magic not in (b"LIVE", b"PIRS", b"CON "):
            raise ValueError("not an STFS package (LIVE/PIRS/CON)")
        self.h = h
        self.header_size = struct.unpack(">I", h[0x340:0x344])[0]
        self.content_type = struct.unpack(">I", h[0x344:0x348])[0]
        self.content_size = struct.unpack(">Q", h[0x34C:0x354])[0]
        self.media_id, self.version, self.base_version, self.title_id = struct.unpack(">IIII", h[0x354:0x364])
        vd = h[0x379:0x379 + 0x24]
        self.block_separation = vd[2]
        self.ft_block_count = struct.unpack("<H", vd[3:5])[0]
        self.ft_block = _u24le(vd, 5)
        self.alloc_blocks = struct.unpack(">I", vd[0x1A:0x1E])[0]
        self.sex = (~self.block_separation) & 1          # 0 = female (one hash table per level), 1 = male (two)
        self.first_block = (self.header_size + 0xFFF) & ~0xFFF
        self.display_name = h[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        self.title_name = h[0x1691:0x1691 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        self.files = self._file_table()

    # block number -> file offset (free60 / Velocity "ComputeBackingDataBlockNumber")
    def _backing(self, block):
        r = (((block + 0xAA) // 0xAA) << self.sex) + block
        if block < 0xAA:
            return r
        if block < 0x70E4:
            return r + (((block + 0x70E4) // 0x70E4) << self.sex)
        return (1 << self.sex) + r + (((block + 0x70E4) // 0x70E4) << self.sex)

    def _block_off(self, block):
        return self.first_block + (self._backing(block) << 12)

    def _level0_hash_entry_off(self, block):
        if block < 0xAA:
            hb = 0
        else:
            step0 = 0xAB if self.sex == 0 else 0xAC
            hb = (block // 0xAA) * step0
            hb += ((block // 0x70E4) + 1) << self.sex
            if block // 0x70E4:
                hb += 1 << self.sex
        off = self.first_block + (hb << 12)
        if self.sex == 1 and (self.block_separation & 2):   # male package using its second table
            off += 0x1000
        return off + (block % 0xAA) * 0x18

    def _next_block(self, block):
        self.f.seek(self._level0_hash_entry_off(block) + 0x15)
        return _u24be(self.f.read(3), 0)

    def _read_block(self, block):
        self.f.seek(self._block_off(block))
        return self.f.read(BLOCK)

    def _file_table(self):
        raw, block = b"", self.ft_block
        for i in range(self.ft_block_count):
            raw += self._read_block(block)
            if i + 1 < self.ft_block_count:
                block = self._next_block(block)
        files = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            nlen = e[0x28] & 0x3F
            if nlen == 0:
                continue
            files.append({
                "name": e[:nlen].decode("latin-1"), "dir": bool(e[0x28] & 0x80), "consecutive": bool(e[0x28] & 0x40),
                "blocks": _u24le(e, 0x29), "start": _u24le(e, 0x2F),
                "parent": struct.unpack(">h", e[0x32:0x34])[0], "size": struct.unpack(">I", e[0x34:0x38])[0],
            })
        for idx, fe in enumerate(files):
            parts, p, guard = [fe["name"]], fe["parent"], 0
            while p != -1 and 0 <= p < len(files) and guard < 64:
                parts.insert(0, files[p]["name"])
                p = files[p]["parent"]
                guard += 1
            fe["path"] = "/".join(parts)
        return files

    def read(self, fe):
        left, block, n = fe["size"], fe["start"], 0
        out = bytearray()
        while left > 0:
            b = self._read_block(block)
            out += b[:min(BLOCK, left)]
            left -= BLOCK
            n += 1
            if left > 0:
                block = block + 1 if fe["consecutive"] else self._next_block(block)
        return bytes(out)


# ----------------------------------------------------------------------------- XEX headers
OPT_NAMES = {0x2FF: "resource info", 0x3FF: "file format", 0x5FF: "delta patch descriptor", 0x405: "base reference",
             0x10001: "original base", 0x10100: "entry point", 0x10201: "image base", 0x103FF: "import libraries",
             0x183FF: "original PE name", 0x200FF: "static libraries", 0x20104: "TLS info", 0x30000: "system flags",
             0x40006: "execution info", 0x40310: "game ratings", 0x406FF: "multidisc media ids",
             0x407FF: "alternate title ids", 0x40801: "additional title memory"}


def xex_info(path):
    d = open(path, "rb").read()
    if d[:4] != b"XEX2":
        raise ValueError("not an XEX2 file")
    flags, hdr_size, _, sec_off, count = struct.unpack(">IIIII", d[4:24])
    info = {"module_flags": hex(flags), "header_size": hdr_size, "file_size": len(d), "sha1": hashlib.sha1(d).hexdigest()}
    for i in range(count):
        key, val = struct.unpack(">II", d[24 + 8 * i:32 + 8 * i])
        name = OPT_NAMES.get(key, hex(key))
        if key == 0x40006:
            media, ver, base, title, plat, exe_t, disc, ndisc, save = struct.unpack(">IIIIBBBBI", d[val:val + 24])
            info["execution"] = {"media_id": f"{media:08X}", "version": _ver(ver), "base_version": _ver(base),
                                 "title_id": f"{title:08X}", "disc": f"{disc}/{ndisc}"}
        elif key == 0x5FF:
            size, tv, sv = struct.unpack(">III", d[val:val + 12])
            info["delta_patch"] = {"target_version": _ver(tv), "source_version": _ver(sv),
                                   "source_digest": d[val + 0xC:val + 0x20].hex(), "size": size}
        elif key == 0x183FF:
            n = struct.unpack(">I", d[val:val + 4])[0]
            info["original_pe_name"] = d[val + 4:val + n].split(b"\0")[0].decode("latin-1")
        elif key == 0x3FF:
            _, enc, comp = struct.unpack(">IHH", d[val:val + 8])
            info["file_format"] = {"encryption": ["none", "normal"][enc] if enc < 2 else enc,
                                   "compression": {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}.get(comp, comp)}
        else:
            info.setdefault("headers", []).append(name)
    return info


# ----------------------------------------------------------------------------- CLI
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("iso-list"); p.add_argument("iso")
    p = sub.add_parser("iso-extract"); p.add_argument("iso"); p.add_argument("out")
    p.add_argument("--only", nargs="*"); p.add_argument("--no-system-update", action="store_true")
    p = sub.add_parser("stfs-info"); p.add_argument("pkg")
    p = sub.add_parser("stfs-extract"); p.add_argument("pkg"); p.add_argument("out")
    p = sub.add_parser("dlc-install"); p.add_argument("pkg"); p.add_argument("dlc_dir")
    p = sub.add_parser("xex-info"); p.add_argument("xex")
    a = ap.parse_args(argv)

    if a.cmd in ("iso-list", "iso-extract"):
        disc = Disc(a.iso)
        want = {w.lower() for w in a.only} if getattr(a, "only", None) else None
        n = total = 0
        for path, start, length, is_dir in disc.walk():
            if a.cmd == "iso-list":
                print(f"{'<DIR>' if is_dir else length:>12} {path}")
                continue
            if getattr(a, "no_system_update", False) and path.split("/")[0].lower() == "$systemupdate":
                continue
            if want is not None and path.lower() not in want:
                continue
            if ".." in path.split("/"):
                raise SystemExit(f"unsafe path {path}")
            dest = os.path.join(a.out, *path.split("/"))
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest + ".part", "wb") as o:
                for b in disc.read_file(start, length):
                    o.write(b)
            os.replace(dest + ".part", dest)
            n += 1
            total += length
        if a.cmd == "iso-extract":
            print(f"extracted {n} files, {total / 1e6:.1f} MB")
    elif a.cmd == "dlc-install":
        # a downloadable content package as Build game lays it out (docs\PROTOCOL.md 3.7)
        s = Stfs(a.pkg)
        if s.content_type != 0x00000002 or s.title_id != 0x4E4D083D:
            raise SystemExit(f"not Soulcalibur V's downloadable content: type {s.content_type:08X}, title {s.title_id:08X}")
        name = os.path.basename(a.pkg)
        root = os.path.join(a.dlc_dir, name)
        for fe in s.files:
            if ".." in fe["path"].split("/"):
                raise SystemExit(f"unsafe path {fe['path']}")
            dest = os.path.join(root, *fe["path"].split("/"))
            if fe["dir"]:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            open(dest, "wb").write(s.read(fe))
        with open(a.pkg, "rb") as f:
            header = f.read(s.header_size)
        open(os.path.join(a.dlc_dir, name + ".header"), "wb").write(header)
        print(f"{s.display_name!r}: {len(s.files)} files into {root}, header {len(header)} bytes")
    elif a.cmd in ("stfs-info", "stfs-extract"):
        s = Stfs(a.pkg)
        if a.cmd == "stfs-info":
            print(f"{s.magic.decode()} package: {s.display_name!r} ({s.title_name})")
            print(f"  content type {s.content_type:08X} ({CONTENT_TYPES.get(s.content_type, '?')}), title id {s.title_id:08X}")
            print(f"  version {_ver(s.version)} (raw {s.version:08X}), base version {_ver(s.base_version)}, media id {s.media_id:08X}")
            print(f"  header 0x{s.header_size:X}, {'female' if s.sex == 0 else 'male'}, file table block {s.ft_block} x{s.ft_block_count}, {s.alloc_blocks} blocks")
            for fe in s.files:
                print(f"  {'<DIR>' if fe['dir'] else fe['size']:>10}  {fe['path']}{'' if fe['consecutive'] or fe['dir'] else '  (chained)'}")
        else:
            for fe in s.files:
                if ".." in fe["path"].split("/"):
                    raise SystemExit(f"unsafe path {fe['path']}")
                dest = os.path.join(a.out, *fe["path"].split("/"))
                if fe["dir"]:
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                data = s.read(fe)
                open(dest, "wb").write(data)
                print(f"extracted {fe['path']} ({len(data)} bytes, sha1 {hashlib.sha1(data).hexdigest()[:12]})")
    else:
        import json
        print(json.dumps(xex_info(a.xex), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
