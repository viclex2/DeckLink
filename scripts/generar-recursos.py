#!/usr/bin/env python3
"""Regenera src/recursos.h a partir de arte/: pantallas del receptor de PC (JPEG),
icono 64x64 y una fuente (atlas de glifos) para los números y los datos de F1.
Estilo de las pantallas: oscuro, sobrio, con barra superior e indicaciones de teclas
abajo, como la interfaz de la consola.
Necesita: python3-pil, rsvg-convert y la fuente Inter."""
import os, subprocess
from PIL import Image, ImageDraw, ImageFont

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
for tam in (64, 96, 160):
    subprocess.run(["rsvg-convert", "-w", str(tam), "-h", str(tam), "arte/decklink.svg",
                    "-o", f"/tmp/logo{tam}.png"], check=True)

W, H = 1280, 800
S = 2                                   # se dibuja al doble y se reduce: bordes suaves
FUENTES = "/usr/share/fonts/opentype/inter/"
def F(peso, tam):
    return ImageFont.truetype(FUENTES + f"Inter-{peso}.otf", tam * S)

# paleta
FONDO_A = (26, 34, 46)       # arriba
FONDO_B = (11, 14, 19)       # abajo
BARRA = (8, 10, 14)
TARJETA = (29, 35, 44)
BORDE = (47, 55, 67)
TEXTO = (235, 239, 244)
GRIS = (139, 146, 154)
AZUL = (26, 159, 255)
VERDE = (89, 191, 64)
ROJO = (232, 90, 80)

def base(titulo_barra="DeckLink"):
    im = Image.new("RGB", (W * S, H * S))
    d = ImageDraw.Draw(im)
    for y in range(H * S):
        t = y / (H * S)
        d.line([(0, y), (W * S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(FONDO_A, FONDO_B)))
    # barra superior
    d.rectangle([0, 0, W * S, 72 * S], fill=BARRA)
    d.line([(0, 72 * S), (W * S, 72 * S)], fill=BORDE, width=S)
    logo = Image.open("/tmp/logo96.png").convert("RGBA").resize((44 * S, 44 * S), Image.LANCZOS)
    im.paste(logo, (28 * S, 14 * S), logo)
    d.text((86 * S, 36 * S), titulo_barra, font=F("SemiBold", 24), fill=TEXTO, anchor="lm")
    return im, d

def pie(d, pistas):
    """barra inferior con teclas, a la derecha (como las indicaciones de botones)"""
    y0 = (H - 64) * S
    d.rectangle([0, y0, W * S, H * S], fill=BARRA)
    d.line([(0, y0), (W * S, y0)], fill=BORDE, width=S)
    x = (W - 32) * S
    fk, fl = F("Bold", 17), F("Medium", 19)
    for tecla, texto in reversed(pistas):
        wl = d.textlength(texto, font=fl)
        x -= wl
        d.text((x, y0 + 32 * S), texto, font=fl, fill=GRIS, anchor="lm")
        wk = d.textlength(tecla, font=fk) + 20 * S
        x -= 12 * S + wk
        d.rounded_rectangle([x, y0 + 17 * S, x + wk, y0 + 47 * S], radius=6 * S, fill=(58, 66, 78))
        d.text((x + wk / 2, y0 + 32 * S), tecla, font=fk, fill=TEXTO, anchor="mm")
        x -= 36 * S

def tarjeta(d, x0, y0, x1, y1):
    d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=14 * S, fill=TARJETA, outline=BORDE, width=S)

def texto(d, xy, t, fuente, color, anchor="la"):
    d.text((xy[0] * S, xy[1] * S), t, font=fuente, fill=color, anchor=anchor)

def pasos(d, x, y, lista):
    for i, (tit, sub) in enumerate(lista):
        cy = y + i * 78
        d.ellipse([(x) * S, (cy) * S, (x + 36) * S, (cy + 36) * S], fill=AZUL)
        texto(d, (x + 18, cy + 18), str(i + 1), F("Bold", 18), (255, 255, 255), "mm")
        texto(d, (x + 56, cy + 2), tit, F("SemiBold", 22), TEXTO)
        texto(d, (x + 56, cy + 34), sub, F("Regular", 18), GRIS)

def guardar(im, ruta):
    im.resize((W, H), Image.LANCZOS).save(ruta, quality=90)

# --- esperando a la Deck ---
im, d = base()
tarjeta(d, 180, 140, 1100, 610)
texto(d, (240, 190), "Esperando a la Steam Deck", F("Bold", 40), TEXTO)
texto(d, (240, 246), "Se conectará sola en cuanto la detecte.", F("Regular", 22), GRIS)
d.line([(240 * S, 300 * S), (1040 * S, 300 * S)], fill=BORDE, width=S)
pasos(d, 240, 340, [
    ("Conecta el cable USB entre la Steam Deck y este equipo", "Cualquier cable USB‑C de datos sirve; con USB 3 la imagen va en color completo."),
    ("O usa la misma red wifi", "En la Deck: icono DeckLink › Activar la conexión por wifi. Te pedirá un PIN."),
    ("Juega", "Teclado, ratón y sonido van solos a la Deck mientras la imagen se ve aquí."),
])
pie(d, [("F1", "Ver FPS y retraso"), ("Ctrl + Alt + Mayús + Q", "Salir")])
guardar(im, "/tmp/espera.jpg")
im.resize((W, H), Image.LANCZOS).save("arte/espera.png")

# --- PIN (las casillas las dibuja el programa, centradas en y=450) ---
im, d = base()
tarjeta(d, 240, 150, 1040, 520)
texto(d, (640, 210), "La Steam Deck pide un PIN", F("Bold", 40), TEXTO, "mm")
texto(d, (640, 268), "Míralo en la Deck: icono DeckLink del escritorio.", F("Regular", 22), GRIS, "mm")
texto(d, (640, 300), "Escríbelo con el teclado y pulsa Intro.", F("Regular", 22), GRIS, "mm")
pie(d, [("Intro", "Conectar"), ("Esc", "Cancelar"), ("Ctrl + Alt + Mayús + Q", "Salir")])
guardar(im, "/tmp/pin.jpg")

# --- hay versión nueva (el número lo pone el programa, centrado en y=390) ---
im, d = base("DeckLink · Actualización")
tarjeta(d, 180, 130, 1100, 650)
texto(d, (640, 210), "Hay una versión nueva de DeckLink", F("Bold", 40), TEXTO, "mm")
texto(d, (640, 520), "Se descarga de GitHub, se instala y DeckLink se vuelve a abrir solo.", F("Regular", 21), GRIS, "mm")
texto(d, (640, 556), "Recuerda instalar la misma versión en la Steam Deck (icono DeckLink › Buscar actualizaciones).", F("Regular", 18), GRIS, "mm")
pie(d, [("S", "Actualizar ahora"), ("N", "Ahora no")])
guardar(im, "/tmp/actualizar.jpg")

im, d = base("DeckLink · Actualización")
tarjeta(d, 180, 130, 1100, 650)
texto(d, (640, 330), "Actualizando DeckLink…", F("Bold", 40), TEXTO, "mm")
texto(d, (640, 400), "Descargando la versión nueva. DeckLink se cerrará y se volverá a abrir solo.", F("Regular", 21), GRIS, "mm")
texto(d, (640, 440), "En Windows, acepta el aviso de permisos que aparece.", F("Regular", 21), AZUL, "mm")
pie(d, [])
guardar(im, "/tmp/actualizando.jpg")

im, d = base("DeckLink · Actualización")
tarjeta(d, 180, 130, 1100, 650)
texto(d, (640, 330), "No se ha podido actualizar", F("Bold", 40), ROJO, "mm")
texto(d, (640, 400), "Comprueba la conexión a internet y vuelve a abrir DeckLink.", F("Regular", 21), GRIS, "mm")
pie(d, [("Cualquier tecla", "Seguir")])
guardar(im, "/tmp/act_error.jpg")

# --- fuente para el programa: atlas de glifos en escala de grises (alfa) ---
GLIFOS = "0123456789.,:+- FPSmsLATENCIRETRASOabcdefghijklnopqrtuvwxyz/"
GLIFOS = "".join(dict.fromkeys(GLIFOS))     # sin repetidos
ALTO = 64
fa = ImageFont.truetype(FUENTES + "Inter-SemiBold.otf", 52)
asc, desc = fa.getmetrics()
xs, ws = [], []
x = 0
for c in GLIFOS:
    w = int(round(fa.getlength(c)))
    xs.append(x); ws.append(max(w, 1))
    x += max(w, 1) + 2
atlas = Image.new("L", (x, ALTO), 0)
da = ImageDraw.Draw(atlas)
for c, gx in zip(GLIFOS, xs):
    da.text((gx, (ALTO - (asc + desc)) // 2), c, font=fa, fill=255)

def arr(nombre, datos, tipo="unsigned char"):
    return f"static const {tipo} {nombre}[{len(datos)}] = {{{','.join(str(b) for b in datos)}}};\n"
icono = Image.open("/tmp/logo64.png").convert("RGBA")
with open("src/recursos.h", "w") as f:
    f.write("/* generado por scripts/generar-recursos.py: pantallas (JPEG), icono 64x64 RGBA y fuente */\n#pragma once\n")
    f.write(arr("espera_jpg", open("/tmp/espera.jpg", "rb").read()))
    f.write(arr("icono_rgba", icono.tobytes()))
    f.write(arr("pin_jpg", open("/tmp/pin.jpg", "rb").read()))
    f.write(arr("actualizar_jpg", open("/tmp/actualizar.jpg", "rb").read()))
    f.write(arr("actualizando_jpg", open("/tmp/actualizando.jpg", "rb").read()))
    f.write(arr("act_error_jpg", open("/tmp/act_error.jpg", "rb").read()))
    f.write(f"#define FUENTE_ANCHO {atlas.width}\n#define FUENTE_ALTO {ALTO}\n")
    f.write('static const char fuente_glifos[] = "' + GLIFOS + '";\n')
    f.write(arr("fuente_x", xs, "unsigned short"))
    f.write(arr("fuente_w", ws, "unsigned char"))
    f.write(arr("fuente_alfa", atlas.tobytes()))
print("src/recursos.h regenerado")
