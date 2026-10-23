"""Emits every SNR2 DLL at its fixed slot base and writes recomp/modules/<mod>.toml (from the mcport.toml pattern).

    python -I tools/snr2/gen_modules.py [mod ...]      (default: every .rel in build/snr2/rel/manifest.json)

Base strategy (GODHAND_PORT_NOTES.md section 9): the game layer (src/godhand/function_hooks.cpp, slot redirect)
moves the room-DLL and Event.rel allocations out of heap D into two fixed slot heaps in the unused RAM hole
0x7957C0..0x8A0000, so every DLL of a class always links at the same base and one recompiled pack per DLL is enough:
    room DLLs (79)  slot heap 0x799000 size 0xF000   -> DLL base 0x799080
    event.rel       slot heap 0x7A8000 size 0x18000  -> DLL base 0x7A8080
    mcport          slot heap 0x7C0000 size 0x20000 -> DLL base 0x7C0080 (redirected from heap C 0x754230; round 8)
The base is the cHeap_allocFirstFit result for the first block of an empty heap: (heap + 0x20 + 0x1F + 0x80) & -0x80.
Re-emits only when build/snr2/out/<mod>/<mod>_link.json is missing or was emitted at another base. Standard library only;
snr2.py runs at below-normal priority.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.normpath(os.path.join(HERE, "..", ".."))
SNR2 = os.path.join(HERE, "snr2.py")
REL = os.path.join(PORT, "build", "snr2", "rel")
OUT = os.path.join(PORT, "build", "snr2", "out")
TOMLS = os.path.join(PORT, "recomp", "modules")
LOW = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if os.environ.get("GODHAND_LOW_PRIORITY") == "1" else 0

ROOM_SLOT_HEAP, ROOM_SLOT_SIZE = 0x00799000, 0xF000
EVENT_SLOT_HEAP, EVENT_SLOT_SIZE = 0x007A8000, 0x18000
MCPORT_SLOT_HEAP, MCPORT_SLOT_SIZE = 0x007C0000, 0x20000


def first_fit_base(heap):
    return (heap + 0x20 + 0x1F + 0x80) & ~0x7F


def slot_for(mod):
    """(class, base, slot heap, slot size) of a module."""
    if mod == "mcport":
        return "mcport", first_fit_base(MCPORT_SLOT_HEAP), MCPORT_SLOT_HEAP, MCPORT_SLOT_SIZE
    if mod == "event":
        return "event", first_fit_base(EVENT_SLOT_HEAP), EVENT_SLOT_HEAP, EVENT_SLOT_SIZE
    if mod.startswith("r"):
        return "room", first_fit_base(ROOM_SLOT_HEAP), ROOM_SLOT_HEAP, ROOM_SLOT_SIZE
    raise SystemExit("unknown module class: " + mod)


TOML = """[general]
# {rel} (SNR2 DLL, "{image}") statically linked at {base:#010x} by
#   python -I tools/snr2/gen_modules.py {mod}   (= tools/snr2/snr2.py emit {mod} --base {base:#x})
# and recompiled by 'python -I build.py modules {mod}' into build/snr2/gen/{mod}, then packed into
# generated_modules/{mod}/ (never generated/). Class '{cls}': {why}
# The generated code is valid only at that base (literal PCs/return addresses), checked at link time by
# src/godhand/function_hooks.cpp (name + base + code sha256).
# Paths are relative to this file; build.py modules makes them absolute.
input = "../../build/snr2/out/{mod}/{mod}.elf"
output = "../../build/snr2/gen/{mod}"
single_file_output = false
output_worker_threads = 4
# {functions} routine starts from snr2.py. Authoritative.
ghidra_output = "../../build/snr2/out/{mod}/{mod}_functions.csv"
ghidra_map_merge = true
patch_syscalls = false
patch_cop0 = false
patch_cache = false
auto_stub_by_name = false
direct_tail_calls = false
skip = []
stubs = []
# No runtime relocations: every import resolves to a boot .sndata export at static-link time
# ({mod}_runtime_relocations.csv has {rr} rows).
"""

WHY = {
    "room": "slot heap 0x799000/0xF000 (cRelSys_loadSlot ReadAlloc, ra 0x2BE5A0, redirected from heap D 0x754220).",
    "event": "slot heap 0x7A8000/0x18000 (LoadResourceEntry_297378 ReadAlloc, ra 0x297410, redirected from heap D).",
    "mcport": "slot heap 0x7C0000/0x20000 (cMc_DllLoad ReadAlloc, ra 0x2AE540, redirected from heap C 0x754230).",
}


def main(argv):
    manifest = json.load(open(os.path.join(REL, "manifest.json"), encoding="utf-8"))
    mods = [e["name"][:-4] for e in manifest]
    if argv:
        unknown = [m for m in argv if m not in mods]
        if unknown:
            raise SystemExit("not in the manifest: %s" % unknown)
        mods = argv
    failed = []
    for mod in mods:
        cls, base, heap, size = slot_for(mod)
        out = os.path.join(OUT, mod)
        link_path = os.path.join(out, mod + "_link.json")
        link = json.load(open(link_path, encoding="utf-8")) if os.path.exists(link_path) else None
        if not link or int(link["base"], 16) != base:
            r = subprocess.run([sys.executable, "-I", SNR2, "emit", mod, "--base", hex(base), "--out", out],
                               capture_output=True, text=True, creationflags=LOW)
            with open(os.path.join(OUT, mod + ".emit.log"), "w", encoding="utf-8") as f:
                f.write(r.stdout + r.stderr)
            if r.returncode:
                failed.append(mod)
                print("%-7s emit FAILED rc %d (see %s)" % (mod, r.returncode, os.path.join(OUT, mod + ".emit.log")))
                continue
            link = json.load(open(link_path, encoding="utf-8"))
            emitted = "emitted"
        else:
            emitted = "kept"
        if heap is not None:
            # The slot must hold the DLL's sector-rounded read buffer after the first-fit padding (+0x20 header).
            need = (base - heap) + ((link["size"] + 0x7FF) & ~0x7FF) + 0x20
            if need > size:
                failed.append(mod)
                print("%-7s does not fit its slot: needs %#x of %#x" % (mod, need, size))
                continue
        if link.get("runtime_relocations"):
            failed.append(mod)
            print("%-7s has %d runtime relocations (not supported by this toml pattern)" % (mod, link["runtime_relocations"]))
            continue
        text = TOML.format(rel=mod + ".rel", image=link["image_name"], base=base, mod=mod, cls=cls, why=WHY[cls],
                           functions=link["functions"], rr=link["runtime_relocations"])
        path = os.path.join(TOMLS, mod + ".toml")
        if mod == "mcport" and os.path.exists(path):
            print("%-7s %s base %#010x, toml kept (hand-written)" % (mod, emitted, base))
            continue
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old is not None and "[[patches." in old:
            # A toml with switchable patches added by hand (r020: Unlock Hard) is the project's; keep it.
            print("%-7s %s base %#010x, toml kept (has patches)" % (mod, emitted, base))
            continue
        if old != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        print("%-7s %s base %#010x %s functions %d" % (mod, emitted, base, cls, link["functions"]))
    print("modules: %d, failed: %d %s" % (len(mods), len(failed), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
