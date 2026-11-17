"""List the files in a CRI CPK archive (the game's .cpk files): python tools/py/cpk.py <file.cpk> [substring]

Reads the archive's header table and its TOC (CRI @UTF tables, not encrypted in SC5) and prints each file's
directory, name, packed and unpacked size and ID; with a substring, only the paths that contain it (any case).
"""
import struct
import sys


def utf_table(b, off):
    """Rows of the @UTF table at b[off:] as dicts."""
    if b[off:off + 4] != b"@UTF":
        raise ValueError(f"no @UTF table at {off:#x}")
    base = off + 8
    _size, = struct.unpack_from(">I", b, off + 4)
    _unk, rows_off, str_off, data_off, _name, ncols, row_len, nrows = struct.unpack_from(">HHIIIHHI", b, base)
    rows_off += base
    str_off += base
    data_off += base

    def string(o):
        end = b.index(b"\0", str_off + o)
        return b[str_off + o:end].decode("latin-1")

    sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 9: 8, 0xA: 4, 0xB: 8}
    fmts = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f", 9: ">d"}

    def value(t, o):
        if t == 0xA:
            return string(struct.unpack_from(">I", b, o)[0])
        if t == 0xB:
            d, n = struct.unpack_from(">II", b, o)
            return (data_off + d, n)
        return struct.unpack_from(fmts[t], b, o)[0]

    cols = []
    p = base + 0x18
    for _ in range(ncols):
        flags = b[p]
        name = string(struct.unpack_from(">I", b, p + 1)[0])
        p += 5
        storage, t = flags & 0xF0, flags & 0x0F
        const = None
        if storage == 0x30:
            const = value(t, p)
            p += sizes[t]
        cols.append((name, storage, t, const))
    rows = []
    for r in range(nrows):
        q = rows_off + r * row_len
        row = {}
        for name, storage, t, const in cols:
            if storage == 0x50:
                row[name] = value(t, q)
                q += sizes[t]
            elif storage == 0x30:
                row[name] = const
            else:
                row[name] = None
        rows.append(row)
    return rows


def main():
    path = sys.argv[1]
    want = sys.argv[2].lower() if len(sys.argv) > 2 else None
    with open(path, "rb") as f:
        head = f.read(0x800)
        hdr = utf_table(head, 0x10)[0]
        toc_off, toc_size = hdr.get("TocOffset"), hdr.get("TocSize")
        if not toc_off:
            raise SystemExit("no TOC (an ID-only archive)")
        f.seek(toc_off)
        toc = f.read(toc_size)
    rows = utf_table(toc, 0x10)
    shown = 0
    for r in rows:
        name = "/".join(x for x in (r.get("DirName"), r.get("FileName")) if x)
        if want and want not in name.lower():
            continue
        print(f"{name}\t{r.get('FileSize')}\t{r.get('ExtractSize')}\tid {r.get('ID')}")
        shown += 1
    print(f"{shown} of {len(rows)} files", file=sys.stderr)


if __name__ == "__main__":
    main()
