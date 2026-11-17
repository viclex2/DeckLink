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
centro("F1: ver los FPS   ·   Salir:  Ctrl + Alt + Mayús + Q", 710, ImageFont.truetype(B, 24), (255, 190, 110))
img.save("/tmp/espera.jpg", quality=88)
img.save("arte/espera.png")

# pantalla para escribir el PIN (los recuadros de las cifras los dibuja el programa)
pin = Image.new("RGB", (W, H))
d = ImageDraw.Draw(pin)
for y in range(H):
    t = y / H
    d.line([(0, y), (W, y)], fill=(int(0x12 + 0x10 * t), int(0x15 - 3 * t), int(0x2b + 0x12 * t)))
logo140 = logo.resize((140, 140))
pin.paste(logo140, ((W - 140) // 2, 60), logo140)
centro("La Steam Deck pide un PIN", 230, ImageFont.truetype(B, 46), (240, 243, 255))
centro("Míralo en la Deck: icono DeckLink del escritorio.", 305, ImageFont.truetype(R, 26), (200, 205, 225))
centro("Escribe el PIN con el teclado y pulsa Intro.", 345, ImageFont.truetype(R, 26), (200, 205, 225))
centro("Esc: cancelar   ·   Salir: Ctrl + Alt + Mayús + Q", 690, ImageFont.truetype(B, 22), (255, 190, 110))
pin.save("/tmp/pin.jpg", quality=88)

# pantallas de actualización (el número de versión lo dibuja el programa)
def fondo():
    im = Image.new("RGB", (W, H))
    dd = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        dd.line([(0, y), (W, y)], fill=(int(0x12 + 0x10 * t), int(0x15 - 3 * t), int(0x2b + 0x12 * t)))
    im.paste(logo140, ((W - 140) // 2, 60), logo140)
    return im, dd

act, d = fondo()
centro("Hay una versión nueva de DeckLink", 235, ImageFont.truetype(B, 46), (240, 243, 255))
centro("Pulsa  S  para actualizar ahora", 520, ImageFont.truetype(B, 34), (140, 255, 160))
centro("Pulsa  N  para seguir con esta (no se volverá a preguntar por esta versión)", 580, ImageFont.truetype(R, 24), (200, 205, 225))
centro("Recuerda instalar también la misma versión en la Steam Deck.", 690, ImageFont.truetype(R, 22), (255, 190, 110))
act.save("/tmp/actualizar.jpg", quality=88)

baj, d = fondo()
centro("Actualizando DeckLink…", 300, ImageFont.truetype(B, 46), (240, 243, 255))
centro("Descargando la versión nueva de GitHub.", 390, ImageFont.truetype(R, 28), (200, 205, 225))
centro("DeckLink se cerrará y se volverá a abrir solo.", 440, ImageFont.truetype(R, 28), (200, 205, 225))
centro("(En Windows, acepta el aviso de permisos que aparece.)", 520, ImageFont.truetype(R, 24), (255, 190, 110))
baj.save("/tmp/actualizando.jpg", quality=88)

err, d = fondo()
centro("No se ha podido actualizar", 300, ImageFont.truetype(B, 46), (255, 150, 140))
centro("Comprueba la conexión a internet y vuelve a abrir DeckLink.", 390, ImageFont.truetype(R, 28), (200, 205, 225))
centro("Pulsa cualquier tecla para seguir.", 470, ImageFont.truetype(B, 28), (240, 243, 255))
err.save("/tmp/act_error.jpg", quality=88)

def arr(nombre, datos):
    return f"static const unsigned char {nombre}[{len(datos)}] = {{{','.join(str(b) for b in datos)}}};\n"
icono = Image.open("/tmp/logo64.png").convert("RGBA")
with open("src/recursos.h", "w") as f:
    f.write("/* generado por scripts/generar-recursos.py: pantallas de espera y de PIN (JPEG) e icono 64x64 RGBA */\n#pragma once\n")
    f.write(arr("espera_jpg", open("/tmp/espera.jpg", "rb").read()))
    f.write(arr("icono_rgba", icono.tobytes()))
    f.write(arr("pin_jpg", open("/tmp/pin.jpg", "rb").read()))
    f.write(arr("actualizar_jpg", open("/tmp/actualizar.jpg", "rb").read()))
    f.write(arr("actualizando_jpg", open("/tmp/actualizando.jpg", "rb").read()))
    f.write(arr("act_error_jpg", open("/tmp/act_error.jpg", "rb").read()))
print("src/recursos.h regenerado")
