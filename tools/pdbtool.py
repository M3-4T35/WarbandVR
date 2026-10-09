"""Exploration de mb_warband_wse2_x64.exe avec son PDB, sans cdb (Linux, Steam Frame...).

Remplace `x`, `uf` et `dt` de cdb à partir de llvm-pdbutil (archive LLVM officielle) et de
capstone (pip). Index des symboles mis en cache à côté de ce script (.pdbtool_cache.json).

  python tools/pdbtool.py find <regex>          fonctions et données dont le nom correspond
  python tools/pdbtool.py uf <nom|0xRVA>         désassemble une fonction, appels et données nommés
  python tools/pdbtool.py at <0xRVA>             symbole contenant cette adresse
  python tools/pdbtool.py calls <nom>            fonctions appelées (directement) par une fonction
  python tools/pdbtool.py callers <nom>          fonctions qui l'appellent directement
  python tools/pdbtool.py type <regex>           disposition d'une classe (champs et offsets)

Variables : WARBAND_DIR (dossier du jeu), LLVM_PDBUTIL (chemin de llvm-pdbutil).
Adresses en RVA (exe+0x...), comme dans warband_vr.log.
"""
import bisect
import json
import os
import re
import struct
import subprocess
import sys

GAME = os.environ.get("WARBAND_DIR", os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/MountBlade Warband"))
EXE = os.path.join(GAME, "mb_warband_wse2_x64.exe")
PDB = os.path.join(GAME, "mb_warband_wse2_x64.pdb")
PDBUTIL = os.environ.get("LLVM_PDBUTIL", os.path.expanduser("~/.local/opt/llvm-tools/bin/llvm-pdbutil"))
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".pdbtool_cache.json")
TYPES_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".pdbtool_types.txt")


def pdbutil(*args):
    return subprocess.run([PDBUTIL, *args, PDB], capture_output=True, text=True, errors="replace").stdout


def load_exe():
    data = open(EXE, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode()
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, va, vsize, rptr, rsize))
    return data, base, secs


def build_index(secs):
    """[(rva, taille, nom, sorte)] : fonctions (S_*PROC32) puis données et symboles publics."""
    va = [s[1] for s in secs]
    entries = {}
    proc = re.compile(r"S_[GL]PROC32 \[size = \d+\] `(.*)`\n\s+parent = \d+, end = \d+, addr = (\d+):(\d+), code size = (\d+)")
    for m in proc.finditer(pdbutil("dump", "-symbols")):
        rva = va[int(m.group(2)) - 1] + int(m.group(3))
        entries[rva] = (rva, int(m.group(4)), m.group(1), "f")
    data = re.compile(r"S_[GL]DATA32 \[size = \d+\] `(.*)`\n\s+type = .*, addr = (\d+):(\d+)")
    pub = re.compile(r"S_PUB32 \[size = \d+\] `(.*)`\n\s+flags = .*, addr = (\d+):(\d+)")
    for text, rx in ((pdbutil("dump", "-globals"), data), (pdbutil("dump", "-symbols"), data),
                     (pdbutil("dump", "-publics"), pub)):
        for m in rx.finditer(text):
            sec = int(m.group(2))
            if sec < 1 or sec > len(va):
                continue
            rva = va[sec - 1] + int(m.group(3))
            entries.setdefault(rva, (rva, 0, m.group(1), "d"))
    return sorted(entries.values())


def index(secs):
    stamp = os.path.getmtime(PDB)
    if os.path.exists(CACHE):
        c = json.load(open(CACHE))
        if c.get("stamp") == stamp:
            return [tuple(e) for e in c["entries"]]
    entries = build_index(secs)
    json.dump({"stamp": stamp, "entries": entries}, open(CACHE, "w"))
    return entries


class Ctx:
    def __init__(self):
        self.data, self.base, self.secs = load_exe()
        self.entries = index(self.secs)
        self.rvas = [e[0] for e in self.entries]
        self.funcs = [e for e in self.entries if e[3] == "f"]

    def at(self, rva):
        i = bisect.bisect_right(self.rvas, rva) - 1
        if i < 0:
            return None
        e = self.entries[i]
        if e[3] == "f" and rva >= e[0] + max(e[1], 1):
            return None
        return e

    def name(self, rva):
        e = self.at(rva)
        if not e:
            return None
        return e[2] if rva == e[0] else f"{e[2]}+0x{rva - e[0]:X}"

    def lookup(self, key):
        if key.startswith("0x"):
            e = self.at(int(key, 16))
            return [e] if e else []
        exact = [e for e in self.funcs if e[2] == key]
        return exact or [e for e in self.funcs if key in e[2]]

    def read(self, rva, size):
        for name, va, vsize, rptr, rsize in self.secs:
            if va <= rva < va + max(vsize, rsize):
                off = rptr + rva - va
                return self.data[off:off + size]
        return b""


def disasm(ctx, e):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    rva, size, name, _ = e
    print(f"{name}  (exe+0x{rva:X}, {size} octets)")
    for i in md.disasm(ctx.read(rva, size), rva):
        note = ""
        m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", i.op_str)
        if m:
            tgt = i.address + i.size + (int(m.group(2), 16) * (1 if m.group(1) == "+" else -1))
            note = f"  ; [{ctx.name(tgt) or hex(tgt)}]"
        elif i.mnemonic in ("call", "jmp") or i.mnemonic.startswith("j"):
            try:
                tgt = int(i.op_str, 16)
                n = ctx.name(tgt)
                if n and not n.startswith(name + "+"):
                    note = f"  ; {n}"
            except ValueError:
                pass
        print(f"  {i.address:08X}  {i.mnemonic:6} {i.op_str}{note}")


def direct_calls(ctx, e):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    out = []
    for i in md.disasm(ctx.read(e[0], e[1]), e[0]):
        if i.mnemonic == "call":
            try:
                out.append((i.address, int(i.op_str, 16)))
            except ValueError:
                pass
    return out


def types_text():
    if not os.path.exists(TYPES_CACHE) or os.path.getmtime(TYPES_CACHE) < os.path.getmtime(PDB):
        open(TYPES_CACHE, "w").write(pdbutil("dump", "-types"))
    return open(TYPES_CACHE).read()


def show_types(pattern):
    """Classes/structures définies (pas les déclarations anticipées) dont le nom correspond."""
    text = types_text()
    records = {}
    for m in re.finditer(r"^\s+(0x[0-9A-F]+) \| (LF_\w+) \[size = \d+\](?: `(.*)`)?\n((?:\s{10,}.*\n)*)", text, re.M):
        records[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    rx = re.compile(pattern)
    for idx, (kind, name, body) in records.items():
        if kind not in ("LF_CLASS", "LF_STRUCTURE", "LF_UNION") or not name or not rx.search(name):
            continue
        if "forward ref" in body:
            continue
        fl = re.search(r"field list: (0x[0-9A-F]+)", body)
        size = re.search(r"sizeof (\d+)", body)
        print(f"{name}  (type {idx}, sizeof 0x{int(size.group(1)) if size else 0:X})")
        if not fl or fl.group(1) not in records:
            continue
        for line in records[fl.group(1)][2].splitlines():
            m = re.search(r"LF_MEMBER \[name = `(.*)`, Type = (0x[0-9A-F]+)(?: \((.*)\))?, offset = (\d+)", line)
            if m:
                tname = m.group(3) or type_name(records, m.group(2))
                print(f"  +0x{int(m.group(4)):04X}  {m.group(1):40} {tname}")
                continue
            m = re.search(r"type = (0x[0-9A-F]+), offset = (\d+)", line)
            if m and "LF_BCLASS" in prev:
                print(f"  +0x{int(m.group(2)):04X}  (base) {type_name(records, m.group(1))}")
            prev = line
        print()


prev = ""


def type_name(records, idx, depth=0):
    r = records.get(idx)
    if not r or depth > 4:
        return idx
    kind, name, body = r
    if name:
        return name
    ref = re.search(r"referent = (0x[0-9A-F]+)", body)
    if kind == "LF_POINTER" and ref:
        return type_name(records, ref.group(1), depth + 1) + "*"
    m = re.search(r"modifies = (0x[0-9A-F]+)", body)
    if kind == "LF_MODIFIER" and m:
        return type_name(records, m.group(1), depth + 1)
    m = re.search(r"element type = (0x[0-9A-F]+)", body)
    if kind == "LF_ARRAY" and m:
        return type_name(records, m.group(1), depth + 1) + "[]"
    return f"{kind} {idx}"


def main():
    global prev
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == "type":
        show_types(arg)
        return
    ctx = Ctx()
    if cmd == "find":
        rx = re.compile(arg)
        for e in ctx.entries:
            if rx.search(e[2]):
                print(f"exe+0x{e[0]:08X}  {'fonction' if e[3] == 'f' else 'donnee  '}  {e[1]:6}  {e[2]}")
    elif cmd == "at":
        print(ctx.name(int(arg, 16)))
    elif cmd == "uf":
        for e in ctx.lookup(arg)[:3]:
            disasm(ctx, e)
    elif cmd == "calls":
        for e in ctx.lookup(arg)[:1]:
            for src, tgt in direct_calls(ctx, e):
                print(f"  {src:08X}  {ctx.name(tgt) or hex(tgt)}")
    elif cmd == "callers":
        targets = {e[0] for e in ctx.lookup(arg)[:1]}
        import capstone
        for f in ctx.funcs:
            for src, tgt in direct_calls(ctx, f):
                if tgt in targets:
                    print(f"  {src:08X}  {f[2]}")


if __name__ == "__main__":
    main()
