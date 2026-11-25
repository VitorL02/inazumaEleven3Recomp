#!/usr/bin/env python3
"""Acha o início da função que contém um endereço e quem chama essa função (BL/BLX/B).
Uso: python3 -I tools/xref.py <endereço hex> [extracted]"""
import json, struct, sys
addr = int(sys.argv[1], 16); ext = sys.argv[2] if len(sys.argv) > 2 else "extracted"
lay = json.load(open(f"{ext}/layout.json"))
import os
only = os.environ.get("OV")  # ex.: OV=15 para escolher o overlay quando endereços se sobrepõem
mods = [(s["ram"], s["file"]) for s in lay["arm9"]["sections"]] + [(o["ram"], o["file"]) for o in lay["overlays"] if only is None or o["id"] == int(only)]
data = {f: open(f"{ext}/{f}", "rb").read() for _, f in mods}
start = addr
for ram, f in mods:
    d = data[f]
    if ram <= addr < ram + len(d):
        o = (addr - ram) & ~3
        while o >= 0:
            w = struct.unpack_from("<I", d, o)[0]
            if (w & 0xffff4000) == 0xe92d4000 or (w & 0xffff0000) == 0xe92d0000: break
            o -= 4
        # inclui "push {r0-r3}" anterior (funções variádicas)
        if o >= 4 and struct.unpack_from("<I", d, o - 4)[0] == 0xe92d000f: o -= 4
        start = ram + o; print(f"função começa em {start:08x} ({f})"); break
for ram, f in mods:
    d = data[f]
    for o in range(0, len(d) - 3, 4):
        w = struct.unpack_from("<I", d, o)[0]
        if (w & 0x0e000000) == 0x0a000000:
            link = "bl" if (w >> 28) == 0xf or (w >> 24) & 1 else "b"
            tg = ram + o + 8 + ((((w & 0xffffff) ^ 0x800000) - 0x800000) * 4)
            if (w >> 28) == 0xf: tg += ((w >> 24) & 1) * 2; link = "blx"
            if tg == start: print(f"  {link:3} de {ram+o:08x} ({f})")
