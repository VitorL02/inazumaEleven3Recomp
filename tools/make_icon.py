"""Gera win/ie3.ico: ícone próprio do projeto (bola com raio), desenhado aqui, sem nada do jogo.
uso: python3 tools/make_icon.py"""
import math
from PIL import Image, ImageDraw

def draw(n):
    S = 4 * n  # desenha grande e reduz (antialias)
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = S * 0.2
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=(18, 64, 160, 255))
    d.rounded_rectangle([S * .04, S * .04, S * .96, S * .96], radius=r * .85, outline=(90, 160, 255, 255), width=max(1, S // 40))
    cx, cy, br = S * .5, S * .54, S * .32
    d.ellipse([cx - br, cy - br, cx + br, cy + br], fill=(250, 250, 250, 255), outline=(20, 30, 60, 255), width=max(1, S // 32))
    pr = br * .38
    pts = [(cx + pr * math.sin(a), cy - pr * math.cos(a)) for a in [i * 2 * math.pi / 5 for i in range(5)]]
    d.polygon(pts, fill=(20, 30, 60, 255))
    for x, y in pts:  # costuras até a borda
        dx, dy = x - cx, y - cy
        d.line([(x, y), (cx + dx / pr * br, cy + dy / pr * br)], fill=(20, 30, 60, 255), width=max(1, S // 40))
    bolt = [(.62, .04), (.40, .42), (.53, .42), (.36, .80), (.70, .34), (.56, .34), (.74, .04)]
    bolt = [(x * S, y * S) for x, y in bolt]
    d.polygon(bolt, fill=(255, 205, 40, 255), outline=(120, 70, 0, 255))
    return im.resize((n, n), Image.LANCZOS)

big = draw(256)
big.save("win/ie3.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
big.save("win/ie3_icon.png")
print("win/ie3.ico ok")
