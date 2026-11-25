#!/usr/bin/env python3
"""Descoberta de funções ARM/Thumb no ARM9 e nos overlays.

Faz descida recursiva a partir de pontos de entrada conhecidos (entry, tabelas
de static init dos overlays, alvos de BL/BLX) e também uma varredura de
prólogos (push {..., lr}) para achar funções chamadas só por ponteiro.

Uso: python3 -I tools/analyze.py [extracted] [saida.json]
"""
import json, struct, sys, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_IMM, ARM_OP_REG, ARM_REG_PC, ARM_REG_LR, ARM_CC_AL

EXT = sys.argv[1] if len(sys.argv) > 1 else "extracted"
OUT = sys.argv[2] if len(sys.argv) > 2 else "analysis/functions.json"

md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM); md_arm.detail = True
md_thm = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md_thm.detail = True

class Module:
    def __init__(self, name, ram, data):
        self.name, self.ram, self.data = name, ram, data
        self.end = ram + len(data)
    def has(self, a): return self.ram <= a < self.end
    def word(self, a): return struct.unpack_from("<I", self.data, a - self.ram)[0]

layout = json.load(open(f"{EXT}/layout.json"))
static = [Module(s["file"][:-4], s["ram"], open(f"{EXT}/{s['file']}", "rb").read())
          for s in layout["arm9"]["sections"]]
overlays = {o["id"]: (Module(f"ov{o['id']:03d}", o["ram"], open(f"{EXT}/{o['file']}", "rb").read()), o)
            for o in layout["overlays"]}

def analyze(mod, seeds, context):
    """Descida recursiva dentro de `mod`. `context` = módulos estáticos visíveis."""
    funcs = {}            # addr -> {"thumb":bool}
    calls_out = collections.Counter()
    ext_calls = collections.Counter()
    unresolved_out = collections.Counter()
    blocks_seen = set()
    work = list(seeds)
    for a, t in seeds: funcs.setdefault(a, {"thumb": t})
    while work:
        addr, thumb = work.pop()
        if (addr, thumb) in blocks_seen or not mod.has(addr): continue
        blocks_seen.add((addr, thumb))
        md = md_thm if thumb else md_arm
        pc = addr
        while mod.has(pc):
            off = pc - mod.ram
            ins = next(md.disasm(mod.data[off:off+4], pc, 1), None)
            if ins is None: break
            m, ops = ins.mnemonic, ins.operands
            nxt = pc + ins.size
            cond_al = ins.cc in (ARM_CC_AL, 0)
            if m in ("bl", "blx") and ops and ops[0].type == ARM_OP_IMM:
                tgt = ops[0].imm & 0xffffffff
                tthumb = (not thumb) if m == "blx" else thumb
                if mod.has(tgt):
                    if tgt not in funcs: funcs[tgt] = {"thumb": tthumb}
                    work.append((tgt, tthumb))
                    calls_out[tgt] += 1
                elif any(c.has(tgt) for c in context):
                    ext_calls[(tgt, tthumb)] += 1
                else:
                    unresolved_out[tgt] += 1
            elif m == "b" and ops and ops[0].type == ARM_OP_IMM:
                tgt = ops[0].imm & 0xffffffff
                if mod.has(tgt): work.append((tgt, thumb))
                elif any(c.has(tgt) for c in context): ext_calls[(tgt, thumb)] += 1  # tail call
                if cond_al: break
            elif m in ("cbz", "cbnz"):
                pass
            elif m == "bx" and cond_al:
                break
            elif m.startswith("pop") or m.startswith("ldm"):
                if any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops) and cond_al: break
            elif ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC and m in ("mov", "ldr") and cond_al:
                break
            elif m.startswith("add") and ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC and not thumb:
                # tabela de salto: addls pc, pc, rX, lsl #2 ; b default ; b caso0 ; ...
                p = nxt + 4
                while mod.has(p):
                    w = mod.word(p)
                    if (w >> 24) & 0xff != 0xea: break
                    tgt = (p + 8 + (((w & 0xffffff) ^ 0x800000) - 0x800000) * 4) & 0xffffffff
                    work.append((tgt, False)); p += 4
                if cond_al: break
            pc = nxt
    # varredura de prólogos ARM (stmdb sp!, {..., lr}) alinhados em 4 bytes, fora do código já visto
    return funcs, calls_out, ext_calls, unresolved_out, blocks_seen

def prologue_scan(mod, known):
    found = []
    for off in range(0, len(mod.data) - 3, 4):
        w = struct.unpack_from("<I", mod.data, off)[0]
        if (w & 0xffff4000) == 0xe92d4000:  # stmfd sp!, {...,lr}
            a = mod.ram + off
            if a not in known: found.append(a)
    return found

result = {"static": {}, "overlays": {}}
main = static[0]; itcm = static[1]
entry = layout["arm9"]["entry"]
# Módulo estático: ARM9 principal
f9, c9, e9, u9, _ = analyze(main, [(entry, False)], [itcm])
extra = prologue_scan(main, f9)
f9b, c9b, e9b, u9b, _ = analyze(main, [(a, False) for a in extra] + [(a, d["thumb"]) for a, d in f9.items()], [itcm])
f9.update({a: d for a, d in f9b.items() if a not in f9})
fi, *_ = analyze(itcm, [(a, t) for (a, t) in e9b if itcm.has(a)] + [(a, False) for a in prologue_scan(itcm, {})], [main])
result["static"] = {
    "arm9": {"ram": main.ram, "size": len(main.data), "functions": len(f9),
             "thumb_functions": sum(1 for d in f9.values() if d["thumb"]),
             "from_recursion": len(f9) - len(extra), "from_prologue_scan": len(extra),
             "calls_into_overlay_space": sum(u9b.values()),
             "overlay_call_targets": len(u9b)},
    "itcm": {"ram": itcm.ram, "size": len(itcm.data), "functions": len(fi)},
}
all_funcs = {"arm9": sorted((a, d["thumb"]) for a, d in f9.items()),
             "itcm": sorted((a, d["thumb"]) for a, d in fi.items())}
# Overlays
tot = 0
for oid, (mod, meta) in sorted(overlays.items()):
    seeds = []
    for p in range(meta["sinit_start"], meta["sinit_end"], 4):
        if mod.has(p):
            v = mod.word(p)
            if v: seeds.append((v & ~1, bool(v & 1)))
    seeds += [(a, False) for a in prologue_scan(mod, {})]
    fo, co, eo, uo, _ = analyze(mod, seeds, [main, itcm])
    tot += len(fo)
    result["overlays"][oid] = {"ram": mod.ram, "size": len(mod.data), "functions": len(fo),
                               "thumb_functions": sum(1 for d in fo.values() if d["thumb"]),
                               "calls_to_static": sum(eo.values()), "calls_outside": sum(uo.values())}
    all_funcs[mod.name] = sorted((a, d["thumb"]) for a, d in fo.items())
result["totals"] = {"functions": len(f9) + len(fi) + tot}
import os; os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
json.dump(result, open(OUT, "w"), indent=1)
with open(OUT.replace(".json", "_list.txt"), "w") as fh:
    for name, lst in all_funcs.items():
        for a, t in lst: fh.write(f"{name} {a:08x} {'thumb' if t else 'arm'}\n")
print(json.dumps(result["static"], indent=1)); print(result["totals"])
