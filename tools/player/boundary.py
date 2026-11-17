#!/usr/bin/env python3
"""What the ready-made sc5.exe and a player's sc5game.dll take from each other (sc5/player/README.md).

Read from the objects of sc5.exe, just before it links (sc5/CMakeLists.txt, SC5_PLAYER_HOST):
  the game's functions sc5.exe replaces (hooks) or names the checks of (midasm hooks) -> sc5.exe exports them
  the game's functions sc5.exe calls                                                  -> sc5game.dll exports them

Writes into --out:
  host_exports.def    what sc5.exe exports
  sc5game.def + .lib  sc5game.dll's exports, as an import library for linking sc5.exe
  game_link.rsp       /EXPORT lines for linking sc5game.dll on the player's PC

A hook works because the translated code defines every game function under its own name only as a weak alias of the
original. sc5game.dll takes the hooks from sc5.exe's import library, and a weak name does not pull a library member in:
build.ninja gives the linker that library with /WHOLEARCHIVE, or every hook would be left out without a word.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tomllib

GAME_FUNCTION = re.compile(r"^(__imp__)?sub_[0-9A-F]{8}$")
HOOK = re.compile(r"^(sub_[0-9A-F]{8}|__imp__\w+|rexcrt_\w+)$")


def midasm_hooks(manifest: pathlib.Path) -> set[str]:
    """The names of the midasm hooks in every config the manifest's codegen reads (sc5_manifest.toml's includes)."""
    names: set[str] = set()
    seen: set[pathlib.Path] = set()

    def read(path: pathlib.Path) -> None:
        path = path.resolve()
        if path in seen:
            return
        seen.add(path)
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        names.update(h["name"] for h in data.get("midasm_hook", []) if "name" in h)
        for section in (data, data.get("entrypoint", {})):
            for include in section.get("includes", []):
                read(path.parent / include)

    read(manifest)
    return names


def symbols(llvm: pathlib.Path, objects: list[str]) -> tuple[set[str], set[str]]:
    response = pathlib.Path(args.out) / "objects.rsp"
    response.write_text("\n".join(f'"{o}"' for o in objects), encoding="utf-8")
    text = subprocess.run([str(llvm / "llvm-nm"), "--format=posix", f"@{response}"], check=True, capture_output=True,
                          encoding="utf-8", errors="replace").stdout
    defined: set[str] = set()
    undefined: set[str] = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(":"):
            continue
        name, kind = fields[0], fields[1]
        if kind == "U":
            undefined.add(name)
        elif kind.isupper() or kind in "wWvV":
            defined.add(name)
    return defined, undefined - defined


parser = argparse.ArgumentParser()
parser.add_argument("--llvm", required=True, help="the folder of llvm-nm and llvm-lib")
parser.add_argument("--manifest", required=True, help="sc5_manifest.toml: the configs whose midasm hooks sc5.exe has")
parser.add_argument("--out", required=True)
parser.add_argument("objects", nargs="+", help="sc5.exe's objects")
args = parser.parse_args()

out = pathlib.Path(args.out)
out.mkdir(parents=True, exist_ok=True)
llvm = pathlib.Path(args.llvm)
defined, undefined = symbols(llvm, [o for o in args.objects if o.endswith(".obj")])

checks = midasm_hooks(pathlib.Path(args.manifest))
hooks = sorted(s for s in defined if HOOK.match(s) or any(s.startswith(f"?{c}@@") for c in checks))
wanted = sorted(s for s in undefined if GAME_FUNCTION.match(s))
found_checks = {c for c in checks if any(s.startswith(f"?{c}@@") for s in hooks)}
if found_checks != checks:
    sys.exit(f"sc5.exe does not define the checks {sorted(checks - found_checks)}")
# sc5.exe gets the image from sc5game.dll through Sc5GameImage (src/sc5/gameimage.h): the DLL's data cannot be linked
# by name from the exe
for data in ("PPCImageConfig", "PPCFuncMappings"):
    if any(s == data or s.startswith(f"?{data}@@") for s in undefined):
        sys.exit(f"sc5.exe uses {data} of the game's code: take it from sc5::GameImage() (src/sc5/gameimage.h)")
if not hooks or not wanted:
    sys.exit(f"nothing crosses between sc5.exe and the game ({len(hooks)} hooks, {len(wanted)} calls): wrong objects?")

(out / "host_exports.def").write_text("EXPORTS\n" + "".join(f'  "{s}"\n' for s in hooks), encoding="utf-8")
(out / "sc5game.def").write_text("LIBRARY sc5game.dll\nEXPORTS\n  Sc5GameImage\n" + "".join(f"  {s}\n" for s in wanted),
                                 encoding="utf-8")
subprocess.run([str(llvm / "llvm-lib"), "/nologo", "/machine:x64", f"/def:{out / 'sc5game.def'}",
                f"/out:{out / 'sc5game.lib'}"], check=True)
lines = ["/EXPORT:Sc5GameImage\n"]
lines += [f"/EXPORT:{s}\n" for s in wanted]
(out / "game_link.rsp").write_text("".join(lines), encoding="utf-8")
print(f"sc5.exe gives the game {len(hooks)} functions and takes {len(wanted)}")
