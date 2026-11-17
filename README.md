<p align="center">
  <img src="arte/decklink-256.png" width="160" alt="Logo de DeckLink">
</p>

<h1 align="center">DeckLink</h1>

<p align="center">
  Usa la <b>pantalla</b>, el <b>teclado</b>, el <b>touchpad</b> y los <b>altavoces</b> de un portátil viejo con tu <b>Steam Deck</b>,<br>
  conectándolos con un simple cable USB. Sin capturadoras, sin wifi y casi sin retardo.
</p>

---

## ¿Qué hace?

Convierte cualquier portátil con Linux en una pantalla para la Steam Deck:

- 🖥️ **Imagen** de la Deck a pantalla completa en el portátil, en **modo juego** y en **modo escritorio** (cambia sola).
- ⌨️ **Teclado y touchpad** del portátil funcionando en la Deck.
- 🔊 **Sonido** de la Deck por los altavoces del portátil.
- 🔌 **Un solo cable USB**:
  - **USB 3:** color completo de 24 bits, sin pérdidas, a 60 fps.
  - **USB 2:** también funciona, con la imagen ligeramente comprimida en JPEG.
- 🖱️ **Sin comandos**: instalador de doble clic e icono en el escritorio de los dos equipos.

## Instalación

Descarga los dos ZIP de la [última release](../../releases/latest):

| Equipo | Archivo |
|---|---|
| Steam Deck | `DeckLink-SteamDeck.zip` |
| Portátil (cualquier Linux de 64 bits) | `DeckLink-Portatil.zip` |

En cada equipo: descomprime el ZIP y haz **doble clic en «Instalar DeckLink»** (si pregunta, elige «Ejecutar»).

### Un paso que solo puedes hacer tú: activar DRD en la BIOS de la Deck

Para que el puerto USB‑C de la Deck pueda funcionar como «dispositivo» (y no solo como «anfitrión»):

1. Apaga la Steam Deck.
2. Mantén pulsados **Volumen +** y **Encendido** hasta oír un pitido.
3. Entra en **Setup Utility → Advanced → USB Configuration**.
4. En **USB Dual-Role Device** elige **DRD** (en vez de XHCI).
5. **Exit → Exit Saving Changes**.

El instalador de la Deck detecta si falta este paso y te lo recuerda.

## Uso

1. Conecta la Deck al portátil con un cable USB **de datos** (USB‑C a USB‑A o USB‑C a USB‑C).
2. En el portátil, doble clic en el icono **DeckLink**.
3. Para salir de la pantalla completa: **Ctrl + Alt + Mayús + Q**.

La Deck queda siempre lista en segundo plano, también en modo juego. La primera vez que conectes en modo escritorio, la Deck pregunta si quieres compartir la pantalla: acepta.
El icono DeckLink de la Deck muestra el estado del cable y permite desactivarlo o desinstalarlo.

<p align="center"><img src="docs/pantalla-espera-e-imagen.png" width="720" alt="Pantalla de espera e imagen de la Deck"></p>

## Cómo funciona

```
 Steam Deck                                              Portátil
┌────────────────────────────┐   cable USB    ┌───────────────────────────┐
│ PipeWire (gamescope / KDE) │ ─────────────▶ │ imagen a pantalla completa│
│ sonido del sistema         │   red USB      │ sonido por los altavoces  │
│ teclado/ratón virtuales    │ ◀───────────── │ teclado y touchpad        │
└────────────────────────────┘ 10.77.0.1 ↔ .2 └───────────────────────────┘
```

- **Red por USB:** la Deck se presenta al portátil como una tarjeta de red USB (*USB gadget* de Linux). Usa **NCM** a velocidad USB 3 y **ECM** a velocidad USB 2, porque con NCM a USB 2 los paquetes del portátil hacia la Deck se quedaban retenidos. La Deck reparte la IP al portátil por DHCP sin ruta por defecto, así que el internet del portátil sigue por su wifi.
- **Imagen:**
  - **Modo juego:** captura la salida de `gamescope` por PipeWire.
  - **Modo escritorio:** pide la pantalla a KDE con el portal de escritorio; el permiso se recuerda.
  - **USB 3:** se manda en color de 24 bits comprimido con **LZ4**, sin pérdidas.
  - **USB 2:** se manda en **JPEG 4:4:4** con calidad que se ajusta sola.
  - En los dos casos va en franjas que se comprimen y descomprimen en paralelo.
  - La Deck no captura el siguiente fotograma hasta que el portátil confirma el anterior, así el retardo nunca se acumula.
- **Sonido:** PCM de 16 bits estéreo a 48 kHz capturado del monitor de la salida de audio. El portátil nunca acumula más de 80 ms.
- **Entrada:** el portátil manda códigos de tecla y movimientos; la Deck los inyecta con `uinput` como un teclado y un ratón USB más.

## Compilar desde el código

```bash
scripts/dependencias.sh   # SDL2 y libjpeg-turbo estáticas (una vez)
scripts/compilar.sh       # build/decklink-deck y build/decklink-portatil
scripts/empaquetar.sh     # dist/DeckLink-SteamDeck.zip y dist/DeckLink-Portatil.zip
```

El programa del portátil va enlazado de forma estática (SDL2, libjpeg-turbo y LZ4 dentro) y carga X11/Wayland y PipeWire/PulseAudio/ALSA al arrancar, así que funciona en casi cualquier distribución con glibc 2.34 o superior.

| Archivo | Qué es |
|---|---|
| `src/decklink-deck.c` | Programa de la Deck: red USB + DHCP (`--red`, como root) y captura/envío (servicio de usuario) |
| `src/decklink-portatil.c` | Programa del portátil: pantalla, sonido y entrada |
| `src/protocolo.h` | Formato de lo que viaja por el cable |
| `paquete/` | Instaladores, panel de la Deck y desinstaladores |
| `arte/` | Logo y pantalla de espera |

## Limitaciones conocidas

- El portátil tiene que usar **Linux** (de momento no hay versión para Windows).
- Con DRD activado, si algún dock o hub USB‑C no funciona en la Deck, vuelve a poner XHCI en la BIOS mientras lo uses.
- Para que los símbolos del teclado salgan bien, la Deck tiene que tener configurada la misma distribución de teclado que el portátil.
- El sonido sigue saliendo también por los altavoces de la Deck.

## Licencia

[MIT](LICENSE)
