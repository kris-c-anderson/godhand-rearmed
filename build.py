"""God Hand (USA, SLUS-21503) PC port: generate, configure, build, boot.

    python -I build.py generate [--iso X] [--toml F]  recompile SLUS_215.03 from your disc image into generated/
    python -I build.py modules [--force] [mod ...]    recompile SNR2 DLLs (recomp/modules/<mod>.toml, after
                                                      tools/snr2/gen_modules.py) into generated_modules/<mod>/;
                                                      unchanged modules are skipped (stamp)
    python -I build.py vu [capture dir ...]           VU1 microcode ahead of time: the uploads found on your disc plus
                                                      captured images (vu1_*.bin) into generated/vu1
    python -I build.py configure [-DOPTION=...]       CMake configure (Ninja, RelWithDebInfo, no LTCG)
    python -I build.py build [target]                 compile godhand.exe (configures first if needed)
    python -I build.py all [--iso X]                  generate -> configure -> build
    python -I build.py characters [--iso X] [--texture-pack D] [id ...]  playable characters (assets/characters) built
                                                      from your disc into build/port/bin/mods/characters (D: the HD
                                                      pack folder, for the painted textures' full-resolution versions)
    python -I build.py boot [--seconds N] [--iso X]   one hidden, time-capped run; output in build/port/bin/logs
    python -I build.py run [args]                     a normal run in a window (arguments go to godhand.exe)
    python -I build.py status                         what exists so far

Needs Python 3.10+, Visual Studio 2022 with C++ (cl, CMake and Ninja come with it; VSROOT if it is not in the default
folder; tools/win/vsenv.cmd sets it up) and your own God Hand (USA) disc image in game/ (or GODHAND_ISO / --iso).
Adapted from the Urban Reign port's build.py and tools/generate.py (one file here; no menu, release or bench).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build" / "port"
BIN = BUILD / "bin"
EXE = BIN / "godhand.exe"
VSENV = ROOT / "tools" / "win" / "vsenv.cmd"
JOBS = os.environ.get("GODHAND_JOBS", str(os.cpu_count() or 8))
GENERATED = ROOT / "generated"
TOML = Path(os.environ.get("GODHAND_TOML", ROOT / "recomp" / "godhand.toml"))

SERIAL = "SLUS-21503"
BOOT_ELF = "SLUS_215.03"
BOOT_SHA256 = "1742f95bef65bdb2aa57b7a77df4ac7619a092e9646e1ea325bc32ec8a64f3cd"
BOOT2 = b"cdrom0:\\SLUS_215.03;1"

EXIT_CODES = {
    0: "the runtime returned by itself",
    1: "initialisation failed or fatal exception",
    2: "no disc image found",
    3: "wrong disc or boot ELF hash mismatch",
    4: "CPU lacks SSE4.1/AVX2",
    5: "stopped on a missing function (MissingFunction=stop)",
    6: "crashed (unhandled exception or fatal signal; see the [crash] lines)",
    124: "time limit reached",
    125: "killed by build.py (did not stop by itself after the time limit)",
}


# Builds, the recompiler and test boots run at below-normal priority so the PC stays responsive (child processes such
# as ninja/cl inherit it) only when GODHAND_LOW_PRIORITY=1 (2026-10-18: normal priority and all cores by default).
LOW_PRIORITY = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if os.environ.get("GODHAND_LOW_PRIORITY") == "1" else 0


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    kwargs["creationflags"] = kwargs.get("creationflags", 0) | LOW_PRIORITY
    return subprocess.run([str(c) for c in cmd], **kwargs).returncode


def find_vsroot():
    """VSROOT for tools/win/vsenv.cmd, whose default is ...\\2022\\Community. This PC has only the 2022 Build Tools in
    Program Files (x86), so look: VSROOT, then vswhere, then the usual folders."""
    if os.environ.get("VSROOT"):
        return os.environ["VSROOT"]
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        if out and (Path(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").exists():
            return out
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            root = Path(base) / "Microsoft Visual Studio" / "2022" / edition
            if (root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").exists():
                return str(root)
    return None


def in_vs(*cmd):
    """Runs a command inside the Visual Studio x64 environment (tools/win/vsenv.cmd)."""
    if not VSENV.exists():
        print(f"{VSENV} is missing (copy tools/win/vsenv.cmd from the Urban Reign port).")
        return 1
    env = dict(os.environ)
    vsroot = find_vsroot()
    if vsroot:
        env["VSROOT"] = vsroot
    return run(["cmd", "/c", VSENV, *cmd], env=env)


def game_running():
    if os.name != "nt":
        return False
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq godhand.exe", "/NH"], capture_output=True, text=True).stdout
    return "godhand.exe" in out.lower()


# ---- disc ---------------------------------------------------------------------------------------------------------

def iso_read_root_file(iso_path, name):
    """One file from the ISO9660 root directory (SYSTEM.CNF and SLUS_215.03 are both there), or None."""
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            return None
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        f.seek(lba * 2048)
        data = f.read(size)
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:
                i = (i // 2048 + 1) * 2048
                continue
            entry = data[i:i + length]
            entry_name = entry[33:33 + entry[32]].decode("ascii", "replace").split(";")[0]
            if entry_name.upper() == name.upper():
                f.seek(struct.unpack_from("<I", entry, 2)[0] * 2048)
                return f.read(struct.unpack_from("<I", entry, 10)[0])
            i += length
    return None


def boots_god_hand(iso_path):
    try:
        cnf = iso_read_root_file(iso_path, "SYSTEM.CNF")
    except OSError:
        return False
    return bool(cnf) and b"BOOT2" in cnf and BOOT2 in cnf


def find_iso(arg=None):
    if arg:
        return Path(arg)
    if os.environ.get("GODHAND_ISO"):
        return Path(os.environ["GODHAND_ISO"])
    for path in sorted((ROOT / "game").glob("*.iso")):
        if boots_god_hand(path):
            return path
    return None


def extract_boot_elf(iso):
    """Writes build/SLUS_215.03 from the disc after checking its SHA-256. Returns its path or None."""
    elf = iso_read_root_file(iso, BOOT_ELF)
    if elf is None:
        print(f"{BOOT_ELF} not found on {iso}")
        return None
    digest = hashlib.sha256(elf).hexdigest()
    if digest != BOOT_SHA256:
        print(f"{BOOT_ELF} hash mismatch ({digest}); expected {BOOT_SHA256} ({SERIAL})")
        return None
    out = ROOT / "build" / BOOT_ELF
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_bytes() != elf:
        out.write_bytes(elf)
    return out


# ---- generate -----------------------------------------------------------------------------------------------------

def find_recompiler():
    """GODHAND_RECOMP, then the recompiler built from runtime/ps2xRecomp into out/recomp, then the port build tree."""
    candidates = []
    if os.environ.get("GODHAND_RECOMP"):
        candidates.append(Path(os.environ["GODHAND_RECOMP"]))
    candidates += [ROOT / "out" / "recomp" / "ps2_recomp.exe",
                   BUILD / "runtime" / "ps2xRecomp" / "ps2_recomp.exe",
                   ROOT / "tools" / "ps2_recomp.exe"]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def stage_toml(toml_path, elf, staging):
    """recomp/godhand.toml with input/output pointed at build/ and its relative CSV paths made absolute."""
    text = toml_path.read_text(encoding="utf-8")
    text = re.sub(r'(?m)^input\s*=.*$', f'input = "{elf.as_posix()}"', text)
    text = re.sub(r'(?m)^output\s*=.*$', f'output = "{staging.as_posix()}"', text)

    def absolute(match):
        key, value = match.group(1), match.group(2)
        path = (toml_path.parent / value).resolve() if not Path(value).is_absolute() else Path(value)
        if path.exists():
            return f'{key} = "{path.as_posix()}"'
        # Both are required: without the boundaries CSV the swallowed SDK starts, the boot task 0x30D358 and the
        # DLL-registration entry 0x394C18 silently disappear from the generated code.
        raise SystemExit(f"{key} {path} is missing (named in {toml_path})")

    text = re.sub(r'(?m)^(ghidra_output|runtime_relocations)\s*=\s*"([^"]+)"', absolute, text)
    staged = ROOT / "build" / "godhand.staging.toml"
    staged.write_text(text, encoding="utf-8")
    return staged


def sync_tree(src, dst):
    """Copies only changed files, so an incremental build recompiles only what changed; removes stale ones."""
    dst.mkdir(parents=True, exist_ok=True)
    wanted = {p.name for p in src.iterdir() if p.is_file()}
    changed = removed = 0
    for name in wanted:
        new = (src / name).read_bytes()
        target = dst / name
        if target.exists() and target.read_bytes() == new:
            continue
        target.write_bytes(new)
        changed += 1
    for target in dst.iterdir():
        if target.is_file() and target.name not in wanted:
            target.unlink()
            removed += 1
    print(f"generated sources: {len(wanted)} total, {changed} written, {removed} removed")


def generate(args=()):
    args = list(args)
    iso_arg = args[args.index("--iso") + 1] if "--iso" in args and args.index("--iso") + 1 < len(args) else None
    toml_path = Path(args[args.index("--toml") + 1]) if "--toml" in args and args.index("--toml") + 1 < len(args) else TOML
    if not toml_path.exists():
        print(f"{toml_path} not found (the God Hand recompiler config; see GODHAND_PORT_NOTES.md).")
        return 1
    if toml_path.name != "godhand.toml":
        # generate syncs into generated/ and deletes every file it did not write: a module toml here would wipe the
        # boot code and force a full rebuild. Modules go through 'build.py modules'.
        print(f"generate takes only godhand.toml (got {toml_path}); SNR2 modules: python -I build.py modules <mod>")
        return 1
    iso = find_iso(iso_arg)
    if not iso:
        print(f"No disc image found. Put your own God Hand (USA) image in game/, set GODHAND_ISO or pass --iso.")
        return 1
    print(f"disc image: {iso}")
    elf = extract_boot_elf(iso)
    if not elf:
        return 1
    recompiler = find_recompiler()
    # The recompiler built here is rebuilt when its sources changed (switchable patches, fixes), so the game code is
    # always generated by the current one. GODHAND_RECOMP or out/recomp: taken as they are.
    built_here = BUILD / "runtime" / "ps2xRecomp" / "ps2_recomp.exe"
    if recompiler and Path(recompiler) == built_here and not os.environ.get("GODHAND_RECOMP"):
        newest = max(f.stat().st_mtime for f in (ROOT / "runtime" / "ps2xRecomp").rglob("*") if f.is_file())
        if newest > built_here.stat().st_mtime:
            print("The recompiler's sources changed: rebuilding ps2_recomp.")
            recompiler = None
    if not recompiler:
        print("Building the recompiler (ps2_recomp) first.")
        if configure(["-DGODHAND_BUILD_RECOMP=ON"]) or in_vs("cmake", "--build", BUILD, "--target", "ps2_recomp", "--", "-j", JOBS):
            return 1
        recompiler = find_recompiler()
        if not recompiler:
            print("ps2_recomp.exe still not found after building it.")
            return 1
    staging = ROOT / "build" / "generated_staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    staged = stage_toml(toml_path, elf, staging)
    log = ROOT / "build" / "recomp.log"
    print(f"+ {recompiler} {staged}  (output in {log})", flush=True)
    with open(log, "w", encoding="utf-8") as out:
        code = subprocess.run([str(recompiler), str(staged)], cwd=ROOT / "build", stdout=out, stderr=subprocess.STDOUT,
                              creationflags=LOW_PRIORITY).returncode
    if code:
        print(f"ps2_recomp failed ({code}); see {log}")
        return code
    if not (staging / "register_functions.cpp").exists():
        print(f"ps2_recomp wrote no register_functions.cpp into {staging}; see {log}")
        return 1
    sync_tree(staging, GENERATED)
    return 0


# ---- SNR2 modules (DLLs) ------------------------------------------------------------------------------------------

MODULE_TOMLS = ROOT / "recomp" / "modules"
GENERATED_MODULES = ROOT / "generated_modules"
SNR2_OUT = ROOT / "build" / "snr2" / "out"
SNR2_GEN = ROOT / "build" / "snr2" / "gen"
REGISTER_LINE = re.compile(r"g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // 0x([0-9a-fA-F]+)")
DECL_LINE = re.compile(r"^void (\w+)\(uint8_t\* rdram", re.M)


def stage_module_toml(toml_path, staging):
    """recomp/modules/<mod>.toml with input/ghidra_output absolute and output forced to build/snr2/gen/<mod>."""
    text = toml_path.read_text(encoding="utf-8")

    def absolute(match):
        key, value = match.group(1), match.group(2)
        path = (toml_path.parent / value).resolve() if not Path(value).is_absolute() else Path(value)
        if not path.exists():
            raise SystemExit(f"{key} {path} is missing (named in {toml_path}; run tools/snr2/snr2.py emit first)")
        return f'{key} = "{path.as_posix()}"'

    text = re.sub(r'(?m)^(input|ghidra_output|runtime_relocations)\s*=\s*"([^"]+)"', absolute, text)
    text = re.sub(r'(?m)^output\s*=.*$', f'output = "{staging.as_posix()}"', text)
    staged = ROOT / "build" / "snr2" / f"{toml_path.stem}.staging.toml"
    staged.write_text(text, encoding="utf-8")
    return staged


def module_pack_source(mod, link, linked, entries):
    """<mod>_pack.cpp: the module's dense-table entries + identity (name, base, size, code range, code sha256).
    The functions live in namespace gh_<mod> (module_namespace): every room pack is generated for the same slot base,
    so the address-suffixed names repeat across packs."""
    base = int(link["base"], 16)
    text_begin, text_end = (int(x, 16) for x in link["code"])
    live = []  # runtime-relocation sites in module code (0 for every DLL so far; snr2.py emit lists them)
    rr = SNR2_OUT / mod / f"{mod}_runtime_relocations.csv"
    if rr.exists():
        for line in rr.read_text(encoding="utf-8").splitlines()[1:]:
            if line.strip():
                live.append(int(line.split(",")[0], 16))
    code = bytearray(linked[text_begin - base:text_end - base])
    for site in live:
        code[site - text_begin:site - text_begin + 4] = b"\0\0\0\0"
    digest = hashlib.sha256(bytes(code)).hexdigest()
    image_name = link["image_name"].replace("\\", "\\\\")
    out = [f"// Generated by build.py modules from build/snr2/gen/{mod}/register_functions.cpp. Do not edit.",
           f"// {link['module']} linked at {link['base']} ({link['size']} bytes), code {link['code'][0]}-{link['code'][1]}.",
           '#include "godhand/module_registry.h"',
           f'#include "{mod}_functions.h"']
    out += ["", "namespace", "{"]
    if live:
        out.append("    const uint32_t kLiveSites[] = {" + ", ".join(f"0x{x:08X}u" for x in live) + "};")
    out.append("    const godhand::ModuleEntry kEntries[] = {")
    out += [f"        {{0x{addr:08X}u, gh_{mod}::{name}}}," for addr, name in entries]
    out += ["    };", "}", "", "namespace godhand", "{",
            f"    extern const ModulePack kModulePack_{mod} = {{",
            f'        "{image_name}", 0x{base:08X}u, 0x{link["size"]:X}u, 0x{text_begin:08X}u, 0x{text_end:08X}u,',
            f'        "{digest}",',
            f"        {'kLiveSites' if live else 'nullptr'}, {len(live)}u,",
            f"        kEntries, {len(entries)}u}};",
            "}", ""]
    return "\n".join(out), digest


MODULE_PIPELINE = "modules-v2-namespace"  # bump when the post-processing below changes (invalidates the stamps)
INCLUDE_FUNCS = "#include <ps2_recompiled_functions.h>"
INCLUDE_STUBS = "#include <ps2_recompiled_stubs.h>"


def module_stamp(mod, toml_path, recompiler):
    """Inputs of one module pack: the toml, snr2's outputs, the recompiler binary and this post-processing version."""
    h = hashlib.sha256(MODULE_PIPELINE.encode())
    for path in (toml_path, SNR2_OUT / mod / f"{mod}_link.json", SNR2_OUT / mod / f"{mod}_linked.bin",
                 SNR2_OUT / mod / f"{mod}_functions.csv", SNR2_OUT / mod / f"{mod}.elf",
                 SNR2_OUT / mod / f"{mod}_runtime_relocations.csv"):
        h.update(path.name.encode())
        h.update(path.read_bytes() if path.exists() else b"<missing>")
    st = Path(recompiler).stat()
    h.update(f"{st.st_size}:{st.st_mtime_ns}".encode())
    return h.hexdigest()


def module_namespace(mod, staging):
    """Puts every generated function of the module into namespace gh_<mod>, renames the module header to
    <mod>_functions.h (unique name and guard: the old shared name/guard PS2_RECOMPILED_FUNCTIONS_H made one module's
    header hide another's) and drops the empty stubs header. Returns an error text or None."""
    ns = f"gh_{mod}"
    header = staging / "ps2_recompiled_functions.h"
    text = header.read_text(encoding="utf-8")
    decls = [line for line in text.splitlines() if line.startswith("void ")]
    if len(decls) != len(DECL_LINE.findall(text)):
        return "unexpected declaration lines in ps2_recompiled_functions.h"
    (staging / f"{mod}_functions.h").write_text(
        "// Generated by build.py modules from ps2_recompiled_functions.h. Do not edit.\n"
        "#pragma once\n\n#include <cstdint>\n\nstruct R5900Context;\nclass PS2Runtime;\n\n"
        f"namespace {ns}\n{{\n" + "\n".join(decls) + f"\n}} // namespace {ns}\n", encoding="utf-8")
    header.unlink()
    stubs = staging / "ps2_recompiled_stubs.h"
    if stubs.exists():
        body = [line for line in stubs.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.startswith("#") and not line.startswith("//")]
        if body:
            return f"ps2_recompiled_stubs.h is not empty ({body[:3]}); stubs in modules are not supported"
        stubs.unlink()
    for path in staging.glob("*.cpp"):
        if path.name == "register_functions.cpp":
            continue
        lines = path.read_text(encoding="utf-8").split("\n")
        if INCLUDE_FUNCS not in lines:
            return f"{path.name}: no '{INCLUDE_FUNCS}' line"
        lines = [f'#include "{mod}_functions.h"' if line == INCLUDE_FUNCS else line
                 for line in lines if line != INCLUDE_STUBS]
        first = next((i for i, line in enumerate(lines) if line.strip() and not line.startswith("#")), None)
        if first is None:
            return f"{path.name}: no code after the include block"
        rest = "\n".join(lines[first:])
        if re.search(r"(?m)^#include", rest):
            return f"{path.name}: an #include after the include block would land inside namespace {ns}"
        path.write_text("\n".join(lines[:first]) + f"\nnamespace {ns}\n{{\n" + rest.rstrip("\n")
                        + f"\n}} // namespace {ns}\n", encoding="utf-8")
    return None


def module_table_coverage(mod, link, linked, entries, staging):
    """Per jr-site gate (replaces the jump-table count comparison): every snr2 jump-table target must be a case label
    of a generated 'switch (jumpTarget)' or a dense-table entry (resume entry), and every non-ra 'jr' in the module code
    must have a switch or an IndirectJump dispatch. Returns (errors, summary)."""
    base = int(link["base"], 16)
    cs, ce = (int(x, 16) for x in link["code"])
    entry_set = {a for a, _n in entries}
    cases, switch_sites, dispatch_sites = set(), set(), set()
    for path in staging.glob("*.cpp"):
        if path.name == "register_functions.cpp":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for chunk in text.split("const uint32_t jumpTarget")[1:]:
            m = re.search(r"dispatchGuestBranch\(rdram, ctx, jumpTarget, 0x([0-9A-Fa-f]+)u, [^)]*IndirectJump", chunk)
            if not m:
                continue
            site = int(m.group(1), 16)
            head = chunk[:m.start()]
            if "switch (jumpTarget)" in head:
                switch_sites.add(site)
                cases |= {int(x, 16) for x in re.findall(r"case 0x([0-9A-Fa-f]+)u:", head)}
            else:
                dispatch_sites.add(site)
    errors = []
    jr_sites = []
    for off in range(cs - base, ce - base, 4):
        w = struct.unpack_from("<I", linked, off)[0]
        if (w >> 26) == 0 and (w & 0x3F) == 8 and ((w >> 21) & 31) != 31:
            jr_sites.append(base + off)
    neither = [hex(a) for a in jr_sites if a not in switch_sites and a not in dispatch_sites]
    if neither:
        errors.append(f"jr sites with neither a switch nor a dispatch: {neither[:8]}")
    total = via_case = via_entry = 0
    missing = []
    for item in link.get("jump_table_list", []):
        table = int(item.split(" ")[0], 16)
        count = int(item.split(" x")[1].split(" ")[0])
        for k in range(count):
            target = struct.unpack_from("<I", linked, table - base + 4 * k)[0]
            total += 1
            if target in cases:
                via_case += 1
            elif target in entry_set:
                via_entry += 1
            else:
                missing.append(f"{table:#x}[{k}]->{target:#x}")
    if missing:
        errors.append(f"jump-table targets with no case label and no entry: {missing[:8]}")
    summary = (f"jr sites {len(jr_sites)} (switch {len([a for a in jr_sites if a in switch_sites])}), table targets "
               f"{total} (case {via_case}, resume entry {via_entry})")
    return errors, summary


def modules(args=()):
    """Recompiles SNR2 DLLs into generated_modules/<mod>/ (never generated/): ps2_recomp at below-normal priority into
    build/snr2/gen/<mod>, every function put into namespace gh_<mod> (room packs share one slot base, so their
    address-suffixed names repeat), register_functions.cpp turned into <mod>_pack.cpp (ModuleEntry array + ModulePack;
    the original would define the boot table's g_ps2RecompiledFunctionTable* globals a second time), offline gates
    (no name shared with the boot code, every definition inside the namespace, per jr-site table coverage), then a sync
    of that one folder. A module whose inputs are unchanged (stamp file) is skipped; --force redoes it."""
    import json
    args = list(args)
    force = "--force" in args
    names = [a for a in args if a != "--force"] or sorted(p.stem for p in MODULE_TOMLS.glob("*.toml"))
    if not names:
        print(f"no module tomls in {MODULE_TOMLS}")
        return 1
    recompiler = find_recompiler()
    if not recompiler:
        print("ps2_recomp.exe not found (out/recomp/ps2_recomp.exe); build it first.")
        return 1
    boot_names = set(DECL_LINE.findall((GENERATED / "ps2_recompiled_functions.h").read_text(encoding="utf-8")))
    done = skipped = 0
    for mod in names:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", mod):
            print(f"{mod}: module names must be C identifiers")
            return 1
        toml_path = MODULE_TOMLS / f"{mod}.toml"
        link_json = SNR2_OUT / mod / f"{mod}_link.json"
        linked_bin = SNR2_OUT / mod / f"{mod}_linked.bin"
        if not toml_path.exists() or not link_json.exists() or not linked_bin.exists():
            print(f"{mod}: need {toml_path}, {link_json} and {linked_bin} (python -I tools/snr2/gen_modules.py {mod})")
            return 1
        staging = SNR2_GEN / mod
        dest = GENERATED_MODULES / mod
        if dest.resolve().parent != GENERATED_MODULES.resolve() or GENERATED.resolve() in (staging.resolve(), dest.resolve()):
            print(f"{mod}: refusing output {dest} / {staging}")
            return 1
        stamp = module_stamp(mod, toml_path, recompiler)
        stamp_file = dest / f"{mod}.stamp"
        if not force and stamp_file.exists() and stamp_file.read_text(encoding="utf-8").strip() == stamp \
                and (dest / f"{mod}_pack.cpp").exists():
            skipped += 1
            continue
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        staged = stage_module_toml(toml_path, staging)
        log = SNR2_GEN / f"{mod}.recomp.log"
        print(f"+ {recompiler} {staged}  (output in {log})", flush=True)
        with open(log, "w", encoding="utf-8") as out:
            code = subprocess.run([str(recompiler), str(staged)], cwd=ROOT / "build", stdout=out, stderr=subprocess.STDOUT,
                                  creationflags=LOW_PRIORITY).returncode
        if code or not (staging / "register_functions.cpp").exists():
            print(f"{mod}: ps2_recomp failed ({code}); see {log}")
            return code or 1
        link = json.loads(link_json.read_text(encoding="utf-8"))
        linked = linked_bin.read_bytes()
        reg = (staging / "register_functions.cpp").read_text(encoding="utf-8")
        entries = sorted({(int(a, 16), n) for n, a in REGISTER_LINE.findall(reg)})
        base = int(link["base"], 16)
        bad = [hex(a) for a, _n in entries if not (base <= a < base + link["size"])]
        if not entries or bad:
            print(f"{mod}: register_functions.cpp has {len(entries)} entries, {len(bad)} outside the image {bad[:8]}")
            return 1
        decls = set(DECL_LINE.findall((staging / "ps2_recompiled_functions.h").read_text(encoding="utf-8")))
        clash = sorted(decls & boot_names)
        if clash:
            print(f"{mod}: function names also defined by the boot code: {clash[:10]}")
            return 1
        missing_decl = sorted({n for _a, n in entries} - decls)
        if missing_decl:
            print(f"{mod}: table entries without a declaration: {missing_decl[:10]}")
            return 1
        sources = [p for p in staging.glob("*.cpp") if p.name != "register_functions.cpp"]
        body = "".join(p.read_text(encoding="utf-8", errors="replace") for p in sources)
        if "g_ps2RecompiledFunctionTable" in body:
            print(f"{mod}: a generated source other than register_functions.cpp touches g_ps2RecompiledFunctionTable")
            return 1
        errors, coverage = module_table_coverage(mod, link, linked, entries, staging)
        if errors:
            print(f"{mod}: jump-table coverage gate failed: {'; '.join(errors)}")
            return 1
        error = module_namespace(mod, staging)
        if error:
            print(f"{mod}: {error}")
            return 1
        # Namespace gate: no function definition outside gh_<mod> (the godhand link uses /FORCE:MULTIPLE, so a stray
        # global duplicate would be merged silently).
        opener = f"namespace gh_{mod}\n{{\n"
        for path in staging.glob("*.cpp"):
            if path.name == "register_functions.cpp":
                continue
            text = path.read_text(encoding="utf-8")
            at = text.find(opener)
            if at < 0 or DECL_LINE.search(text[:at]) or not text.rstrip().endswith(f"}} // namespace gh_{mod}"):
                print(f"{mod}: {path.name} has code outside namespace gh_{mod}")
                return 1
        print(f"{mod}: {len(sources)} sources, {len(decls)} functions, {len(entries)} table entries; {coverage}")
        pack, digest = module_pack_source(mod, link, linked, entries)
        (staging / f"{mod}_pack.cpp").write_text(pack, encoding="utf-8")
        # Kept for reference outside the synced folder (it would redefine the boot table's globals).
        (staging / "register_functions.cpp").replace(SNR2_GEN / f"{mod}.register_functions.cpp.txt")
        (staging / f"{mod}.stamp").write_text(stamp + "\n", encoding="utf-8")
        print(f"{mod}: base {link['base']} code sha256 {digest}")
        sync_tree(staging, dest)
        done += 1
    print(f"modules: {done} regenerated, {skipped} unchanged (stamp)")
    return 0

# ---- configure / build --------------------------------------------------------------------------------------------

DEFAULT_DEPS = ROOT.parent / "PS2Recomp" / "out" / "rel" / "_deps"


def configure(extra=()):
    extra = list(extra)
    args = ["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            # No /GL over the generated code ever (src/CMakeLists.txt); GODHAND_LTO adds it for the port and IOP only.
            "-DGODHAND_LTO=OFF"]
    # GODHAND_DEPS: an already downloaded FetchContent _deps folder (raylib-src, ...) to reuse instead of downloading.
    # Default: the PS2Recomp checkout's out/rel/_deps next to this port, when it exists.
    deps = os.environ.get("GODHAND_DEPS") or (str(DEFAULT_DEPS) if DEFAULT_DEPS.is_dir() else "")
    if deps:
        for name in ("raylib", "sdl3", "imgui", "rlimgui", "nlohmann_json", "fmt", "elfio", "toml11", "libdwarf",
                     "rabbitizer"):
            src = Path(deps) / f"{name}-src"
            if src.is_dir():
                args.append(f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={src.as_posix()}")
    # Never download at configure time unless asked: a missing dependency fails instead.
    if os.environ.get("GODHAND_ALLOW_DOWNLOADS") != "1":
        args.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    # The recompiler is not part of the game build; only an explicit -DGODHAND_BUILD_RECOMP=ON turns it on, and every
    # other configure resets it (a stale ON would pull elfio, toml11, fmt, libdwarf and rabbitizer).
    if not any(a.startswith("-DGODHAND_BUILD_RECOMP=") for a in extra):
        args.append("-DGODHAND_BUILD_RECOMP=OFF")
    return in_vs(*args, *extra)


def has_generated_code():
    return (GENERATED / "register_functions.cpp").exists()


def build(target="godhand"):
    if not has_generated_code():
        print("The recompiled game code is missing: run 'python -I build.py generate' first.")
        return 1
    if game_running():
        print("godhand.exe is running: close it first (the link would fail).")
        return 1
    # build.ninja, not CMakeCache.txt: CMake writes the cache even when configure fails.
    if not (BUILD / "build.ninja").exists() and configure():
        return 1
    # The output is also written to build/port/last_build.log for the link gate below.
    log_path = BUILD / "last_build.log"
    env = dict(os.environ)
    vsroot = find_vsroot()
    if vsroot:
        env["VSROOT"] = vsroot
    cmd = ["cmd", "/c", VSENV, "cmake", "--build", BUILD, "--target", target, "--", "-j", JOBS]
    print("+", " ".join(str(c) for c in cmd), f"(log {log_path})", flush=True)
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen([str(c) for c in cmd], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", creationflags=LOW_PRIORITY)
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
        code = proc.wait()
    if code:
        return code
    # Link gate: godhand links with /FORCE:MULTIPLE, so a duplicate module symbol would only be a warning and one
    # definition would be picked silently. Every module function lives in namespace gh_<mod> (build.py modules).
    dups = [line.strip() for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines()
            if ("LNK4006" in line or "LNK4088" in line) and ("gh_" in line or "kModulePack_" in line)]
    if dups:
        print(f"link gate: {len(dups)} duplicate module symbol warnings (first: {dups[0][:200]})")
        return 1
    return 0


def build_all(args=()):
    return generate(args) or configure() or build()


# ---- run / boot ---------------------------------------------------------------------------------------------------

def play(args=()):
    if not EXE.exists():
        print("godhand.exe not built yet: run 'python -I build.py build' first.")
        return 1
    # Session header (sweep L2): play_last.log had elapsed seconds only, no wall-clock time or exe identity.
    digest = hashlib.sha256(EXE.read_bytes()).hexdigest()
    print(f"[session] start {time.strftime('%Y-%m-%d %H:%M:%S %z')} exe {EXE.name} sha256 {digest}", flush=True)
    # --time-limit 0: a normal run is not capped (the ini default TimeLimitSeconds is for hidden boots).
    code = run([EXE, "--window", "--time-limit", "0", *args], cwd=BIN)
    print(f"[session] end {time.strftime('%Y-%m-%d %H:%M:%S %z')} exit {code}", flush=True)
    return code


def keep_play_log(args=()):
    """Start-God-Hand.cmd, before it overwrites logs/play_last.log: keep the previous session as
    logs/play_<its time>.log (nothing is deleted; each is ~1 MB). Sweep 2026-10-17 L2: two sessions' logs were lost."""
    logs = BIN / "logs"
    last = logs / "play_last.log"
    if last.is_file() and last.stat().st_size > 0:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(last.stat().st_mtime))
        target = logs / f"play_{stamp}.log"
        if not target.exists():
            shutil.copy2(last, target)
    return 0


def summarize(log_path, code, stamp):
    print(f"exit code {code}: {EXIT_CODES.get(code, 'unknown')}")
    text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    picks = [line for line in text if "[DLL]" in line or "missing-target" in line or "time limit" in line
             or " ERROR " in line or "[crash]" in line or "runtime exited" in line]
    for line in picks[:60]:
        print("  " + line[:240])
    if len(picks) > 60:
        print(f"  ... {len(picks) - 60} more")
    print(f"log: {log_path}")
    for pattern in (f"*{stamp}*",):
        for path in sorted(log_path.parent.glob(pattern)):
            if path != log_path:
                print(f"     {path}")


def boot(args=()):
    """One hidden run with a wall-clock cap. Nothing appears on screen (no console window, hidden game window, muted).
    godhand.exe stops itself at the cap (exit 124); only if it is still alive 60 s later is this one process killed."""
    args = list(args)
    seconds = 120
    extra = []
    i = 0
    while i < len(args):
        if args[i] == "--seconds" and i + 1 < len(args):
            seconds = int(args[i + 1])
            i += 2
        elif args[i] == "--iso" and i + 1 < len(args):
            extra += ["--iso", args[i + 1]]
            i += 2
        else:
            extra.append(args[i])
            i += 1
    if not EXE.exists():
        print("godhand.exe not built yet: run 'python -I build.py build' first.")
        return 1
    if game_running():
        print("godhand.exe is already running; not starting another one.")
        return 1
    logs = BIN / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = logs / f"boot_{stamp}.log"
    env = dict(os.environ)
    env["GODHAND_HEADLESS"] = "1"
    env["GODHAND_TIME_LIMIT_SECONDS"] = str(seconds)
    env.setdefault("GODHAND_MISSING_FUNCTION", "stop")
    env.setdefault("PS2X_DUMP_RAM_ON_MISSING", str(logs / f"missing_ram_{stamp}.bin"))
    env.setdefault("GODHAND_DUMP_RAM", str(logs / f"timeout_ram_{stamp}.bin"))
    env.setdefault("GODHAND_DLL_LOG", str(logs / f"dll_loads_{stamp}.csv"))
    # Hidden runs never read or write the player's controls.ini: a fresh default-layout file per run.
    env.setdefault("GODHAND_CONTROLS_FILE", str(logs / f"controls_{stamp}.ini"))
    # ... nor the player's memory card (sweep 2026-10-17 L3/CR-2): 30 earlier hidden boots opened bin/memcards/Mcd001.ps2
    # read/write and a time-limit stop can land mid-save. Default: a per-run copy (the run still sees the player's saves).
    def resolved(value):
        path = Path(value)
        return (path if path.is_absolute() else BIN / path).resolve()
    if resolved(env["GODHAND_CONTROLS_FILE"]) == (BIN / "controls.ini").resolve():
        print("refusing: GODHAND_CONTROLS_FILE is the player's controls.ini; hidden runs use a scratch file.")
        return 1
    if env.get("GODHAND_MC_DIR"):
        if resolved(env["GODHAND_MC_DIR"]) == (BIN / "memcards").resolve():
            print("refusing: GODHAND_MC_DIR is the player's memory card folder; hidden runs use a copy.")
            return 1
    else:
        mc_copy = logs / f"mc_{stamp}"
        mc_copy.mkdir(parents=True, exist_ok=True)
        for name in ("Mcd001.ps2", "Mcd002.ps2"):
            if (BIN / "memcards" / name).is_file():
                shutil.copy2(BIN / "memcards" / name, mc_copy / name)
        env["GODHAND_MC_DIR"] = str(mc_copy)
    print(f"memory cards: {env['GODHAND_MC_DIR']}")
    # Read by the runtime before main() (a static initializer), so it must come from the real environment.
    env.setdefault("PS2X_BRANCH_TRACE", "256")
    env.setdefault("PS2X_BRANCH_TRACE_FILE", str(logs / f"branch_trace_{stamp}.txt"))
    cmd = [str(EXE), "--headless", "--time-limit", str(seconds), *extra]
    print("+", " ".join(cmd), f"> {log_path}", flush=True)
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | LOW_PRIORITY
    with open(log_path, "w", encoding="utf-8") as out:
        proc = subprocess.Popen(cmd, cwd=BIN, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                creationflags=flags)
        print(f"godhand.exe pid {proc.pid}, cap {seconds} s")
        try:
            code = proc.wait(timeout=seconds + 60)
        except subprocess.TimeoutExpired:
            print(f"pid {proc.pid} did not stop by itself within {seconds + 60} s; killing that process only")
            proc.kill()
            proc.wait()
            code = 125  # TerminateProcess exit code is 1, which would read as "initialisation failed"
    summarize(log_path, code, stamp)
    return code


def status(args=()):
    iso = find_iso()
    rows = [
        ("recompiler config", TOML),
        ("disc image", iso or ROOT / "game" / "<none>.iso"),
        ("boot ELF copy", ROOT / "build" / BOOT_ELF),
        ("generated code", GENERATED / "register_functions.cpp"),
        ("VU1 microcode compiled", GENERATED / "vu1" / "vu1_programs.cpp"),
        ("CMake cache", BUILD / "CMakeCache.txt"),
        ("godhand.exe", EXE),
        ("tools/win/vsenv.cmd", VSENV),
    ]
    for label, path in rows:
        print(f"  [{'x' if Path(path).exists() else ' '}] {label}: {path}")
    print(f"  Visual Studio (VSROOT): {find_vsroot() or 'not found'}")
    return 0


# ---- VU microcode ahead of time ------------------------------------------------------------------------------------

VU_IMAGES = ROOT / "build" / "vu1_images"
VU1_RECOMP = BUILD / "runtime" / "ps2xTest" / "vu1_recomp.exe"


def vu(args=()):
    """Compiles the game's VU1 microcode into generated/vu1 (README "Faster 3D"). Inputs, collected in build/vu1_images:
    every microcode upload (VIF MPG) found in the boot ELF and the SNR2 DLLs (tools/vu/extract_uploads.py), and the
    vu1_*.bin images and vup_*_cap_*.bin uploads captured while playing (PS2X_VU1_CAPTURE, logs/vu1_missing) from the
    folders given here or in
    GODHAND_VU_CAPTURE (separated by ';'). Captured images are kept in build/vu1_images across runs. At run time the
    compiled blocks are assembled for whatever combination of programs is in VU1's micro memory."""
    elf = ROOT / "build" / BOOT_ELF
    if not elf.exists():
        print(f"{elf} not found: run 'python -I build.py generate' first.")
        return 1
    VU_IMAGES.mkdir(parents=True, exist_ok=True)
    rels = sorted((ROOT / "build" / "snr2" / "rel").glob("*.rel"))
    if run([sys.executable, "-I", ROOT / "tools" / "vu" / "extract_uploads.py", VU_IMAGES, elf, *rels]):
        return 1
    folders = list(args) + [f for f in os.environ.get("GODHAND_VU_CAPTURE", "").split(";") if f]
    copied = 0
    for folder in folders:
        for image in sorted(Path(folder).glob("vu1_*.bin")) + sorted(Path(folder).glob("vup_*_cap_*.bin")):
            size = image.stat().st_size
            if (size == 16384 or image.name.startswith("vup_")) and size % 8 == 0 and not (VU_IMAGES / image.name).exists():
                shutil.copy2(image, VU_IMAGES / image.name)
                copied += 1
    images = len(list(VU_IMAGES.glob("vu1_*.bin")))
    uploads = len(list(VU_IMAGES.glob("vup_*_cap_*.bin")))
    print(f"captured: {images} images, {uploads} uploads ({copied} new)")
    if not (BUILD / "build.ninja").exists() or "GODHAND_BUILD_TOOLS:BOOL=ON" not in \
            (BUILD / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace"):
        if configure(["-DGODHAND_BUILD_TOOLS=ON"]):
            return 1
    if build("vu1_recomp"):
        return 1
    # Into a fresh folder first: generated/vu1 is replaced only by a complete run, so a failed run leaves the last
    # good VU code in place (the build goes on with it, with a warning).
    staging = ROOT / "build" / "vu1_staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    # vu1_recomp links the runtime library, which loads SDL3.dll when controllers go through SDL: it is in bin/.
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    code = run([VU1_RECOMP, VU_IMAGES, staging], env=env)
    if code:
        print(f"vu1_recomp failed (exit code {code & 0xFFFFFFFF:#010x}); keeping the VU code of the last good run in "
              f"{GENERATED / 'vu1'}.")
        return 0 if (GENERATED / "vu1" / "vu1_programs.cpp").exists() else code
    sync_tree(staging, GENERATED / "vu1")
    return 0


def characters(args=()):
    args = list(args)
    iso_arg = None
    if "--iso" in args and args.index("--iso") + 1 < len(args):
        i = args.index("--iso"); iso_arg = args[i + 1]; del args[i:i + 2]
    iso = find_iso(iso_arg)
    if not iso:
        print("No disc image found. Put your own God Hand (USA) image in game/, set GODHAND_ISO or pass --iso.")
        return 1
    pack = None
    if "--texture-pack" in args and args.index("--texture-pack") + 1 < len(args):
        i = args.index("--texture-pack"); pack = args[i + 1]; del args[i:i + 2]
    sys.path.insert(0, str(ROOT / "tools" / "models"))
    pydeps = ROOT / "build" / "pydeps"  # numpy, Pillow, xxhash (pip install --target build/pydeps) for the painted textures
    if pydeps.is_dir():
        sys.path.append(str(pydeps))
    import build_characters
    build_characters.texture_pack = pack or None  # the HD pack the painted arm textures start from (optional)
    BIN.mkdir(parents=True, exist_ok=True)
    return build_characters.build(str(iso), str(BIN), tuple(args))


COMMANDS = {
    "generate": generate,
    "characters": characters,
    "vu": vu,
    "modules": modules,
    "configure": lambda a: configure(a),
    "build": lambda a: build(a[0] if a else "godhand"),
    "all": build_all,
    "boot": boot,
    "run": play,
    "play": play,
    "keep-play-log": keep_play_log,
    "status": status,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        return 0 if len(sys.argv) < 2 else 2
    return COMMANDS[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
