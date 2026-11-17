#!/usr/bin/env python3
"""Regenera src/recursos.h (pantalla de espera en JPEG e icono 64x64) a partir de arte/.
Necesita: python3-pil y rsvg-convert."""
import os, subprocess
from PIL import Image, ImageDraw, ImageFont

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
subprocess.run(["rsvg-convert", "-w", "280", "-h", "280", "arte/decklink.svg", "-o", "/tmp/logo280.png"], check=True)
subprocess.run(["rsvg-convert", "-w", "64", "-h", "64", "arte/decklink.svg", "-o", "/tmp/logo64.png"], check=True)

W, H = 1280, 800
img = Image.new("RGB", (W, H))
d = ImageDraw.Draw(img)
for y in range(H):
    t = y / H
    d.line([(0, y), (W, y)], fill=(int(0x12 + 0x10 * t), int(0x15 - 3 * t), int(0x2b + 0x12 * t)))
logo = Image.open("/tmp/logo280.png").convert("RGBA")
img.paste(logo, ((W - 280) // 2, 110), logo)
B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
def centro(txt, y, f, col):
    w = d.textlength(txt, font=f)
    d.text(((W - w) / 2, y), txt, font=f, fill=col)
centro("DeckLink", 410, ImageFont.truetype(B, 64), (240, 243, 255))
centro("Esperando a la Steam Deck…", 500, ImageFont.truetype(R, 34), (170, 200, 255))
centro("Conecta el cable USB entre la Steam Deck y este portátil.", 572, ImageFont.truetype(R, 26), (200, 205, 225))
centro("Se conectará solo en cuanto la detecte.", 610, ImageFont.truetype(R, 26), (200, 205, 225))
centro("Salir:  Ctrl + Alt + Mayús + Q", 710, ImageFont.truetype(B, 24), (255, 190, 110))
img.save("/tmp/espera.jpg", quality=88)
img.save("arte/espera.png")

def arr(nombre, datos):
    return f"static const unsigned char {nombre}[{len(datos)}] = {{{','.join(str(b) for b in datos)}}};\n"
icono = Image.open("/tmp/logo64.png").convert("RGBA")
with open("src/recursos.h", "w") as f:
    f.write("/* generado por scripts/generar-recursos.py: pantalla de espera (JPEG) e icono 64x64 RGBA */\n#pragma once\n")
    f.write(arr("espera_jpg", open("/tmp/espera.jpg", "rb").read()))
    f.write(arr("icono_rgba", icono.tobytes()))
print("src/recursos.h regenerado")
