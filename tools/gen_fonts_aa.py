#!/usr/bin/env python3
"""Gera runtime/fonts_aa.h: fontes proporcionais com antisserrilhado (alfa de 8 bits), Latin-1 32-255,
rasterizadas da DejaVu Sans (licença livre Bitstream Vera; mudanças da DejaVu em domínio público).
Uso: python3 -I tools/gen_fonts_aa.py > runtime/fonts_aa.h"""
from PIL import ImageFont
D = "/usr/share/fonts/truetype/dejavu/"
FONTS = [("FONT_TITLE", "DejaVuSans-Bold.ttf", 46), ("FONT_H", "DejaVuSans-Bold.ttf", 24),
         ("FONT_MENU", "DejaVuSans-Bold.ttf", 19), ("FONT_BODY", "DejaVuSans.ttf", 17),
         ("FONT_SMALL", "DejaVuSans.ttf", 14)]
print("/* Gerado por tools/gen_fonts_aa.py a partir da DejaVu Sans (licença livre Bitstream Vera). */")
print("#pragma once\n#include <stdint.h>")
print("typedef struct { int16_t adv, w, h, ox, oy; uint32_t off; } AAGlyph;")
print("typedef struct { int size, ascent, line; const AAGlyph *g; const uint8_t *px; } AAFont;")
print(f"enum {{ {', '.join(n for n, _, _ in FONTS)}, FONT_COUNT }};")
for idx, (name, file, size) in enumerate(FONTS):
    f = ImageFont.truetype(D + file, size, layout_engine=ImageFont.Layout.BASIC)
    asc, desc = f.getmetrics()
    glyphs, data = [], bytearray()
    for code in range(32, 256):
        ch = chr(code) if not 128 <= code < 160 else " "
        adv = round(f.getlength(ch))
        bbox = f.getbbox(ch)
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((adv, 0, 0, 0, 0, len(data))); continue
        m = f.getmask(ch, mode="L")
        w, h = m.size
        glyphs.append((adv, w, h, bbox[0], bbox[1], len(data)))
        data += bytes(m)
    print(f"static const AAGlyph g_{idx}_glyphs[224] = {{")
    print(",\n".join("{%d,%d,%d,%d,%d,%d}" % g for g in glyphs))
    print("};")
    print(f"static const uint8_t g_{idx}_px[{len(data) or 1}] = {{")
    for i in range(0, len(data), 40):
        print(",".join(str(b) for b in data[i:i + 40]) + ",")
    print("};")
print("static const AAFont g_fonts[FONT_COUNT] = {")
for idx, (name, file, size) in enumerate(FONTS):
    f = ImageFont.truetype(D + file, size, layout_engine=ImageFont.Layout.BASIC)
    asc, desc = f.getmetrics()
    print(f"  {{{size}, {asc}, {asc + desc}, g_{idx}_glyphs, g_{idx}_px}},")
print("};")
