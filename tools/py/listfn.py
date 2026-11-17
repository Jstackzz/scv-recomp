"""Print a recompiled function's PowerPC listing with guest addresses, from the generated code.

    python -I tools/py/listfn.py 824943F0 [--grep TEXT]

The generated C++ (sc5/generated/default/sc5_recomp.*.cpp) keeps each instruction as a comment ("// lwz r11,0(r3)")
and each branch target as a label ("loc_82494534:"); addresses are counted from the function's start and set again at
every label, so they stay right where the code generator left an instruction out of the comments.
"""

import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATED = os.path.join(HERE, "..", "..", "sc5", "generated", "default")


def find(func):
    head = f"DEFINE_REX_FUNC(sub_{func})"
    for path in sorted(glob.glob(os.path.join(GENERATED, "sc5_recomp.*.cpp"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        i = text.find(head)
        if i >= 0:
            j = text.find("DEFINE_REX_FUNC(", i + len(head))
            return text[i : j if j > 0 else len(text)]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", help="the function's guest address, hex (824943F0)")
    ap.add_argument("--grep", help="only lines containing this text")
    args = ap.parse_args()
    func = args.address.upper().removeprefix("0X").removeprefix("SUB_")
    src = find(func)
    if src is None:
        print(f"no sub_{func} in {GENERATED}", file=sys.stderr)
        return 1
    addr = int(func, 16)
    label = re.compile(r"^loc_([0-9A-F]{8}):")
    for raw in src.splitlines():
        line = raw.strip()
        m = label.match(line)
        if m:
            addr = int(m.group(1), 16)
            if not args.grep:
                print(f"{m.group(0)}")
            continue
        if line.startswith("// "):
            text = f"{addr:08X}  {line[3:]}"
            if not args.grep or args.grep in text:
                print(text)
            addr += 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
