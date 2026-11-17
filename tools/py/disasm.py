"""Disassemble the dumped image (tools/imagedump): python tools/py/disasm.py <image.bin> <addr> [count] [--base 0x82000000]

Uses sp00nznet's PowerPC decoder (tools/py/thirdparty/ps3recomp/ppu_disasm.py, MIT); Xenon's VMX128 opcodes show
as unknown.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "thirdparty", "ps3recomp"))
from ppu_disasm import decode  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    base = 0x82000000
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base") + 1], 16)
        args = [a for a in args if a != sys.argv[sys.argv.index("--base") + 1]]
    image, addr = args[0], int(args[1], 16)
    count = int(args[2]) if len(args) > 2 else 32
    data = open(image, "rb").read()
    for i in range(count):
        a = addr + 4 * i
        w = struct.unpack_from(">I", data, a - base)[0]
        ins = decode(w, a)
        print(f"{a:08X}: {w:08X}  {ins.mnemonic} {ins.operands}")


if __name__ == "__main__":
    main()
