#!/usr/bin/env python3
"""Disassembla um trecho: python3 -I tools/dis.py <endereço hex> [n] [thumb] [extracted]"""
import json, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
addr = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
thumb = len(sys.argv) > 3 and sys.argv[3] == "thumb"
ext = sys.argv[4] if len(sys.argv) > 4 else "extracted"
lay = json.load(open(f"{ext}/layout.json"))
import os
only = os.environ.get("OV")  # ex.: OV=15 para escolher o overlay quando endereços se sobrepõem
mods = [(s["ram"], s["file"]) for s in lay["arm9"]["sections"]] + [(o["ram"], o["file"]) for o in lay["overlays"] if only is None or o["id"] == int(only)]
for ram, f in mods:
    d = open(f"{ext}/{f}", "rb").read()
    if ram <= addr < ram + len(d):
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
        for i, ins in enumerate(md.disasm(d[addr - ram:], addr)):
            if i >= n: break
            print(f"{ins.address:08x}  {ins.bytes.hex():8}  {ins.mnemonic} {ins.op_str}")
        break
