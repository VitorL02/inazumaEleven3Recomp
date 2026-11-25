#!/usr/bin/env python3
"""Procura a função que carrega overlays: o alvo de BL que mais recebe
r0=0 (ARM9) e r1=<id de overlay> em constante, com muitos ids diferentes."""
import json, struct, sys, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
ext = sys.argv[1] if len(sys.argv) > 1 else "extracted"
lay = json.load(open(f"{ext}/layout.json"))
mods = [(s["ram"], s["file"]) for s in lay["arm9"]["sections"][:1]] + [(o["ram"], o["file"]) for o in lay["overlays"]]
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
ids = collections.defaultdict(set); sites = collections.defaultdict(list)
for ram, f in mods:
    d = open(f"{ext}/{f}", "rb").read()
    regs = {}
    for off in range(0, len(d) - 3, 4):
        w = struct.unpack_from("<I", d, off)[0]
        if (w & 0x0ff00000) == 0x03a00000 and (w >> 28) == 0xe:  # mov rd, #imm
            rd = (w >> 12) & 0xf; rot = ((w >> 8) & 0xf) * 2; imm = w & 0xff
            regs[rd] = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff; regs[rd, "age"] = off
        elif (w & 0xff000000) == 0xeb000000:  # bl
            tgt = ram + off + 8 + ((((w & 0xffffff) ^ 0x800000) - 0x800000) * 4)
            r0 = regs.get(0) if off - regs.get((0, "age"), -99) <= 12 else None
            r1 = regs.get(1) if off - regs.get((1, "age"), -99) <= 12 else None
            if r0 == 0 and r1 is not None and r1 < 133:
                ids[tgt].add(r1); sites[tgt].append((ram + off, f, r1))
            regs = {}
for tgt in sorted(ids, key=lambda t: -len(ids[t]))[:5]:
    print(f"{tgt:08x}: {len(ids[tgt])} ids distintos, {len(sites[tgt])} chamadas; ex.: {sites[tgt][:4]}")
