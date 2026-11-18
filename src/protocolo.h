/* protocolo.h — lo que viaja entre la Steam Deck y la pantalla (portátil, PC o Android) */
#pragma once
#include <stdint.h>

/* versión de DeckLink (las apps de PC buscan versiones nuevas en GitHub) */
#define DL_VERSION      "2.18"
#define DL_PRUEBAS      0         /* 1 = versión de pruebas: también avisa de otras de pruebas */
#define DL_REPO         "viclex2/DeckLink"

#define DL_PUERTO       47800     /* TCP: imagen, sonido y entrada */
#define DL_PUERTO_BALIZA 47801    /* UDP: la Deck avisa de que está en la red */
#define DL_IP_DECK      "10.77.0.1"   /* por cable USB (tarjeta principal) */
#define DL_IP_PC        "10.77.0.2"
#define DL_IP_DECK2     "10.79.0.1"   /* por cable USB 2: segunda tarjeta (NCM, para Windows y Android) */
#define DL_IP_PC2       "10.79.0.2"
#define DL_MAGIC        0x33464C44u   /* "DLF3" */
#define DL_BALIZA       "DECKLINK1"

/* tipos especiales en dl_input (pantalla -> Deck) */
#define DL_ACK          0xFFFF    /* "ya tengo el fotograma" */
#define DL_HOLA         0xFFFE    /* primer mensaje: code = capacidades, value = PIN (-1 si no hay) */
#define DL_ADIOS        0xFFFD    /* la pantalla se cierra (para el canal USB directo) */

#define DL_CAP_LZ4      1         /* sabe descomprimir LZ4 (si no, siempre JPEG) */
#define DL_CAP_420  4   /* JPEG 4:2:0: menos datos y se descomprime antes (móviles) */
#define DL_CAP_FRANJAS 8 /* acepta DL_FMT_FRANJA: cada franja llega sola en cuanto está lista */
#define DL_CAP_ABS      2         /* manda posiciones absolutas (pantalla táctil) */

/* formatos (Deck -> pantalla) */
#define DL_FMT_RGB565   0         /* (ya no se usa) */
#define DL_FMT_JPEG     1         /* franjas JPEG independientes (4:4:4) */
#define DL_FMT_AUDIO    2         /* PCM 16 bits estéreo 48000 Hz; w = h = 0; sin confirmación */
#define DL_FMT_LZ4      3         /* color de 24 bits (B,G,R) en franjas LZ4, sin pérdidas */
#define DL_FMT_RECHAZO  4         /* la Deck no acepta la conexión; w = motivo; luego cierra */
#define DL_FMT_NADA     5         /* relleno sin datos útiles (canal USB directo); se ignora */
#define DL_FMT_FRANJA   6         /* una franja JPEG suelta: [índice][total] + JPEG; w,h del fotograma
                                     entero; reservado = retraso (2 bytes) y número de fotograma */

#define DL_RECHAZO_WIFI_OFF 1     /* la conexión por wifi está desactivada en la Deck */
#define DL_RECHAZO_PIN      2     /* falta el PIN o no es correcto */

#define DL_AUDIO_HZ      48000
#define DL_AUDIO_CANALES 2

#define DL_ABS_MAX       65535    /* rango de EV_ABS ABS_X / ABS_Y */

/* Canal USB directo (sin red), para Android: interfaz "vendor" con dos endpoints bulk */
#define DL_USB_VID       0x1d6b
#define DL_USB_PID       0x0104
/* Windows: WinUSB se instala solo para el canal directo y lo publica con este GUID */
#define DL_WINUSB_GUID "{6E3A1B7C-4D2F-4C8B-9A51-DEC1EC0D0001}"
#define DL_FFS_DIR       "/dev/ffs-decklink"

#pragma pack(push, 1)
/* Deck -> pantalla: cabecera + datos.
 * JPEG y LZ4: [uint8 n][uint32 tam[n]][franja0][franja1]...  (franjas horizontales
 *             iguales, la última se queda con el resto de filas)
 * AUDIO:      muestras PCM */
typedef struct {
    uint32_t magic;
    uint16_t w, h;
    uint8_t  fmt;
    uint8_t  reservado[3];
    uint32_t bytes;
} dl_frame_hdr;

/* pantalla -> Deck: evento de teclado/ratón (códigos evdev de Linux) o mensaje especial */
typedef struct {
    uint16_t type, code;
    int32_t  value;
} dl_input;
#pragma pack(pop)

static inline int dl_franja_y0(int i, int n, int h) { return i * (h / n); }
static inline int dl_franja_h(int i, int n, int h) { return i == n - 1 ? h - i * (h / n) : h / n; }
