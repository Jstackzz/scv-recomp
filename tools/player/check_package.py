#!/usr/bin/env python3
"""Checks a player package needs nothing that is not in it or in Windows itself.

Every .exe and .dll in the package has its imports looked up beside it, in the folder of the program that loads it, and
in System32; a DLL found only in System32 that comes from an installer rather than from Windows (the Visual C++ runtime,
DirectX's old redistributables) fails the check: a PC without that installer could not start the program.

With a built game folder (games\\sc5\\sc5game.dll there) it also checks that the game takes every hook from sc5.exe: the
translated code's weak aliases would link quietly without them (sc5/player/README.md).

Usage: check_package.py <package folder> [--llvm <folder of llvm-readobj>]
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

NOT_WINDOWS = re.compile(r"^(msvcp|vcruntime|concrt|vccorlib|vcomp|mfc|atl\d|d3dx|xinput1_[123]|xaudio2_[0-7]|xapofx|physx|openal|ucrtbased)", re.I)
SYSTEM32 = pathlib.Path(os.environ["SystemRoot"]) / "System32"
GAME_DATA = {"disc", "update", "dlc", "user", "cache"}  # the player's own files below games\sc5: no programs of ours


def default_llvm() -> str:
    """tools\\devenv.cmd's clang: SC5_LLVM, this repository's external\\llvm, else MK9 Online's."""
    if os.environ.get("SC5_LLVM"):
        return str(pathlib.Path(os.environ["SC5_LLVM"]) / "bin")
    own = pathlib.Path(__file__).resolve().parents[2] / "external" / "llvm"
    if (own / "bin" / "llvm-readobj.exe").exists():
        return str(own / "bin")
    return str(pathlib.Path.home() / "source" / "repos" / "mk9-rollback" / "external" / "llvm" / "bin")


parser = argparse.ArgumentParser()
parser.add_argument("package")
parser.add_argument("--llvm", default=default_llvm())
args = parser.parse_args()
root = pathlib.Path(args.package)
readobj = pathlib.Path(args.llvm) / "llvm-readobj.exe"


def imports(path: pathlib.Path) -> dict[str, list[str]]:
    text = subprocess.run([str(readobj), "--coff-imports", str(path)], capture_output=True, encoding="utf-8", errors="replace").stdout
    result: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("Name: "):
            current = result.setdefault(line[6:], [])
        elif line.startswith("Symbol: ") and current is not None:
            current.append(line[8:].split(" (")[0])
    return result


def loader_folder(path: pathlib.Path) -> list[pathlib.Path]:
    """Where Windows looks first for what this file imports: beside the program that runs it."""
    parts = [p.lower() for p in path.relative_to(root).parts]
    game = root / "games" / "sc5"
    if parts[:2] == ["games", "sc5"]:
        if len(parts) > 3 and parts[2] == "sdk":
            return [game / "sdk" / "bin"]
        # host\sc5.exe is only ever run as the copy build-game.cmd puts beside the runtime's DLLs
        return [game, game / "sdk" / "bin"]
    if parts[0] == "toolchain":
        return [root / "toolchain" / "bin"]
    return [root]


def is_windows(name: str) -> bool:
    lower = name.lower()
    if lower.startswith(("api-ms-win-", "ext-ms-")):
        return True
    return (SYSTEM32 / name).exists() and not NOT_WINDOWS.match(lower)


problems: list[str] = []
files = [p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in (".exe", ".dll")]
files = [p for p in files if not (p.relative_to(root).parts[:2] == ("games", "sc5") and len(p.relative_to(root).parts) > 3
                                  and p.relative_to(root).parts[2].lower() in GAME_DATA)]
for path in files:
    folders = [path.parent] + loader_folder(path)
    for name in imports(path):
        if any((f / name).exists() for f in folders) or is_windows(name):
            continue
        # sc5.exe's own game part: there once the game is built
        if name.lower() in ("sc5game.dll", "sc5.exe"):
            continue
        problems.append(f"{path.relative_to(root)}: {name}")
print(f"{len(files)} programs and libraries checked")

game = root / "games" / "sc5"
dll = game / "sc5game.dll"
if dll.exists():
    table = imports(dll)
    hooks = table.get("sc5.exe", [])
    exports = re.findall(r"Name: (\S+)", subprocess.run([str(readobj), "--coff-exports", str(game / "sc5.exe")], capture_output=True,
                                                        encoding="utf-8", errors="replace").stdout)
    # A game function's hook is the one a weak alias can leave out without a word (README.md). The others cannot be:
    # a midasm hook or the trap log missing stops the link, and a kernel import's hook (__imp__*) is taken whenever the
    # game imports that function at all.
    wanted = {e for e in exports if re.match(r"sub_[0-9A-F]{8}$", e)}
    missing = sorted(wanted - set(hooks))
    print(f"sc5game.dll takes {len(set(hooks) & wanted)} of sc5.exe's {len(wanted)} game function hooks "
          f"({len(hooks)} functions from sc5.exe in all)")
    problems += [f"sc5game.dll does not take the hook {s} from sc5.exe" for s in missing]

for p in problems:
    print("PROBLEM:", p)
if problems:
    sys.exit(1)
print("nothing needed from outside the package and Windows")
