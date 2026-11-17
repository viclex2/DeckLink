/* protocolo.h — lo que viaja por el cable entre la Deck y el portátil (v4) */
#pragma once
#include <stdint.h>

#define DL_PUERTO   47800
#define DL_IP_DECK  "10.77.0.1"
#define DL_IP_PC    "10.77.0.2"
#define DL_MAGIC    0x33464C44u   /* "DLF3" */
#define DL_ACK      0xFFFF        /* portátil -> Deck: "ya tengo el fotograma" */

#define DL_FMT_RGB565  0          /* imagen tal cual, 16 bits por píxel (ya no se usa) */
#define DL_FMT_JPEG    1          /* USB 2: franjas JPEG independientes (color completo 4:4:4) */
#define DL_FMT_AUDIO   2          /* sonido: PCM 16 bits estéreo a 48000 Hz, w = h = 0 */
#define DL_FMT_LZ4     3          /* USB 3: color de 24 bits (B,G,R) en franjas LZ4, sin pérdidas */

#define DL_AUDIO_HZ    48000
#define DL_AUDIO_CANALES 2

#pragma pack(push, 1)
/* Deck -> portátil: cabecera + datos.
 * RGB565: w*h*2 bytes.
 * JPEG y LZ4: [uint8 n][uint32 tam[n]][franja0][franja1]...  (franjas horizontales
 *         iguales, la última se queda con el resto de filas)
 * AUDIO:  muestras PCM; no lleva confirmación */
typedef struct {
    uint32_t magic;
    uint16_t w, h;
    uint8_t  fmt;
    uint8_t  reservado[3];
    uint32_t bytes;
} dl_frame_hdr;

/* portátil -> Deck: un evento de teclado/ratón (códigos evdev de Linux) */
typedef struct {
    uint16_t type, code;
    int32_t  value;
} dl_input;
#pragma pack(pop)

static inline int dl_franja_y0(int i, int n, int h) { return i * (h / n); }
static inline int dl_franja_h(int i, int n, int h) { return i == n - 1 ? h - i * (h / n) : h / n; }
