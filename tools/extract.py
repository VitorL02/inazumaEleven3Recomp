#!/usr/bin/env python3
"""Extrai o código da ROM (ARM9, autoloads, overlays, ARM7) para extracted/.

Uso: python3 -I tools/extract.py <rom.nds> [saida]
Os arquivos gerados não vão para o git (veja .gitignore).
"""
import hashlib, json, os, struct, sys
import ndspy.rom

EXPECTED_SHA1 = "a70e9a336f6ea8ff78003938dcd024533f034a68"  # BOEJ, tradução BR T1.3

def main():
    rom_path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "extracted"
    data = open(rom_path, "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    if sha1 != EXPECTED_SHA1:
        print(f"aviso: SHA-1 {sha1} difere do esperado {EXPECTED_SHA1}")
    rom = ndspy.rom.NintendoDSRom(data)
    os.makedirs(f"{out}/ov", exist_ok=True)
    meta = {"sha1": sha1, "code": rom.idCode.decode(), "arm9": {}, "arm7": {}, "overlays": []}

    arm9 = rom.loadArm9()
    meta["arm9"] = {"entry": rom.arm9EntryAddress, "ram": rom.arm9RamAddress, "sections": []}
    for i, s in enumerate(arm9.sections):
        name = "arm9" if i == 0 else f"autoload_{s.ramAddress:08x}"
        open(f"{out}/{name}.bin", "wb").write(s.data)
        meta["arm9"]["sections"].append({"file": f"{name}.bin", "ram": s.ramAddress,
                                         "size": len(s.data), "bss": s.bssSize})
    open(f"{out}/arm7.bin", "wb").write(rom.arm7)
    meta["arm7"] = {"entry": rom.arm7EntryAddress, "ram": rom.arm7RamAddress, "size": len(rom.arm7)}

    for i, o in sorted(rom.loadArm9Overlays().items()):
        open(f"{out}/ov/ov{i:03d}.bin", "wb").write(o.data)
        meta["overlays"].append({"id": i, "file": f"ov/ov{i:03d}.bin", "ram": o.ramAddress,
                                 "size": len(o.data), "bss": o.bssSize,
                                 "sinit_start": o.staticInitStart, "sinit_end": o.staticInitEnd,
                                 "file_id": o.fileID})
    json.dump(meta, open(f"{out}/layout.json", "w"), indent=1)
    print(f"ok: {len(meta['overlays'])} overlays, {len(arm9.sections)} seções ARM9 em {out}/")

if __name__ == "__main__":
    main()
