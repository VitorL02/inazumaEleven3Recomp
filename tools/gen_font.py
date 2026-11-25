#!/usr/bin/env python3
"""Gera runtime/font.h: glifos 8x16 (Latin-1, 32-255) rasterizados da DejaVu Sans Mono.
A fonte DejaVu é livre (licença Bitstream Vera / domínio público para as mudanças da DejaVu).
Uso: python3 -I tools/gen_font.py [caminho.ttf] > runtime/font.h"""
import sys
from PIL import Image, ImageDraw, ImageFont
path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
font = ImageFont.truetype(path, 13, layout_engine=ImageFont.Layout.BASIC)
print("/* Glifos 8x16 gerados por tools/gen_font.py a partir da DejaVu Sans Mono (licença livre Bitstream Vera). */")
print("#pragma once\n#include <stdint.h>\nstatic const uint8_t g_font[224][16] = {")
for code in range(32, 256):
    im = Image.new("L", (8, 16), 0)
    if not 128 <= code < 160:
        ImageDraw.Draw(im).text((0, 0), chr(code), font=font, fill=255)
    rows = []
    for y in range(16):
        b = 0
        for x in range(8):
            if im.getpixel((x, y)) > 110:
                b |= 0x80 >> x
        rows.append(b)
    print("  {" + ",".join(f"0x{r:02x}" for r in rows) + "},")
print("};")
