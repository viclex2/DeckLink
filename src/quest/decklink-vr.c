/*
 * DeckLink VR (Meta Quest): la imagen de la Steam Deck en una pantalla gigante,
 * en 2D o en 3D lado a lado (por ejemplo con ReShade SuperDepth3D en la Deck).
 *
 * Es una actividad nativa (NativeActivity + OpenXR) dentro de la misma app de Android.
 * La pantalla se dibuja con capas «quad» de OpenXR: en 3D, el ojo izquierdo ve la mitad
 * izquierda de la imagen y el derecho la mitad derecha. Así la imagen la compone el propio
 * sistema de las gafas, con la mejor nitidez y sin retraso extra.
 *
 * Conexión por wifi (como el PC): escucha las balizas de la Deck, se conecta y recibe
 * JPEG por franjas (se descomprimen en paralelo con AImageDecoder) y el sonido (AAudio).
 * El PIN lo guarda la app normal de DeckLink en el archivo «pin».
 */
#include <android/log.h>
#include <android/imagedecoder.h>
#include <android_native_app_glue.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../protocolo.h"
#include "../recursos.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DeckLinkVR", __VA_ARGS__)

static struct android_app *app;
static atomic_int salir;

/* ======================= estado compartido ======================= */

enum { E_BUSCANDO = 0, E_CONECTADO, E_PIN, E_WIFI_OFF, E_OTRA_VERSION };
static atomic_int estado = E_BUSCANDO;

/* triple búfer: la red escribe en «fondo», lo deja en «listo» y la pantalla lo coge para «uso» */
static pthread_mutex_t mtx_img = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *b_fondo, *b_listo, *b_uso;
static int img_w, img_h;         /* tamaño de b_listo */
static int nuevo;
static size_t b_cap;

/* ajustes (se guardan) */
static struct { int modo3d, invertir, tam; } aj = { 1, 0, 2 };
static const float ANCHO_TAM[4] = { 2.0f, 3.0f, 4.2f, 6.0f };
static const float DIST_TAM[4] = { 2.0f, 2.6f, 3.2f, 4.0f };

static void ruta(char *out, size_t n, const char *archivo)
{
    snprintf(out, n, "%s/%s", app->activity->internalDataPath, archivo);
}

static void cargar_ajustes(void)
{
    char r[512];
    ruta(r, sizeof r, "vr_ajustes");
    FILE *f = fopen(r, "r");
    if (!f) return;
    int m, i, t;
    if (fscanf(f, "%d %d %d", &m, &i, &t) == 3) {
        aj.modo3d = !!m; aj.invertir = !!i;
        aj.tam = t < 0 ? 0 : t > 3 ? 3 : t;
    }
    fclose(f);
}

static void guardar_ajustes(void)
{
    char r[512];
    ruta(r, sizeof r, "vr_ajustes");
    FILE *f = fopen(r, "w");
    if (f) { fprintf(f, "%d %d %d\n", aj.modo3d, aj.invertir, aj.tam); fclose(f); }
}

static int leer_pin(void)
{
    char r[512], v[32] = "";
    ruta(r, sizeof r, "pin");
    FILE *f = fopen(r, "r");
    if (!f) return -1;
    if (!fgets(v, sizeof v, f)) v[0] = 0;
    fclose(f);
    return v[0] ? (int)strtol(v, NULL, 10) : -1;
}

static uint64_t ms_ahora(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ======================= sonido ======================= */

static AAudioStream *audio;

static void audio_abrir(void)
{
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, DL_AUDIO_CANALES);
    AAudioStreamBuilder_setSampleRate(b, DL_AUDIO_HZ);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    if (AAudioStreamBuilder_openStream(b, &audio) != AAUDIO_OK) audio = NULL;
    AAudioStreamBuilder_delete(b);
    if (audio) {
        /* poco colchón: unos 40 ms */
        AAudioStream_setBufferSizeInFrames(audio, DL_AUDIO_HZ / 25);
        AAudioStream_requestStart(audio);
    }
}

static void audio_poner(const void *pcm, uint32_t bytes)
{
    if (!audio) return;
    int32_t frames = (int32_t)(bytes / (2 * DL_AUDIO_CANALES));
    AAudioStream_write(audio, pcm, frames, 0);      /* sin esperar: si no cabe, se pierde */
}

/* ======================= red ======================= */

static int sock = -1;

static int leer_todo(int s, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t k = recv(s, b, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        b += k; n -= (size_t)k;
    }
    return 0;
}

static void enviar(int s, uint16_t tipo, uint16_t codigo, int32_t valor)
{
    dl_input m = { tipo, codigo, valor };
    send(s, &m, sizeof m, MSG_NOSIGNAL);
}

static int conectar(const char *ip, int ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(DL_PUERTO);
    inet_pton(AF_INET, ip, &a.sin_addr);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd p = { s, POLLOUT, 0 };
        int err = 0;
        socklen_t l = sizeof err;
        if (poll(&p, 1, ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && !err) r = 0;
    }
    if (r < 0) { close(s); return -1; }
    fcntl(s, F_SETFL, fl);
    int uno = 1, buf = 4 << 20;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    struct timeval tv = { 5, 0 };                    /* sin datos en 5 s: se da por perdida */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

typedef struct {
    const uint8_t *src;
    size_t tam;
    uint8_t *dst;
    int w, h, ok;
    pthread_t hilo;
} franja_t;

static void *descomprimir(void *p)
{
    franja_t *f = p;
    AImageDecoder *d = NULL;
    f->ok = 0;
    if (AImageDecoder_createFromBuffer(f->src, f->tam, &d) != ANDROID_IMAGE_DECODER_SUCCESS) return NULL;
    AImageDecoder_setAndroidBitmapFormat(d, ANDROID_BITMAP_FORMAT_RGBA_8888);
    const AImageDecoderHeaderInfo *hi = AImageDecoder_getHeaderInfo(d);
    if (AImageDecoderHeaderInfo_getWidth(hi) == f->w && AImageDecoderHeaderInfo_getHeight(hi) == f->h)
        f->ok = AImageDecoder_decodeImage(d, f->dst, (size_t)f->w * 4, (size_t)f->w * 4 * f->h) == ANDROID_IMAGE_DECODER_SUCCESS;
    AImageDecoder_delete(d);
    return NULL;
}

/* fotograma JPEG por franjas: [n][n tamaños de 4 bytes][JPEG...] -> RGBA */
static int decodificar(const uint8_t *d, uint32_t bytes, int w, int h, uint8_t *dst)
{
    if (bytes < 1) return -1;
    int n = d[0];
    if (n < 1 || n > 16 || bytes < 1u + 4u * n) return -1;
    franja_t f[16];
    const uint8_t *p = d + 1 + 4 * n;
    uint32_t resto = bytes - 1 - 4 * n;
    for (int i = 0; i < n; i++) {
        uint32_t t;
        memcpy(&t, d + 1 + 4 * i, 4);
        if (t > resto) return -1;
        f[i].src = p; f[i].tam = t;
        f[i].w = w; f[i].h = dl_franja_h(i, n, h);
        f[i].dst = dst + (size_t)dl_franja_y0(i, n, h) * w * 4;
        p += t; resto -= t;
    }
    for (int i = 1; i < n; i++)
        if (pthread_create(&f[i].hilo, NULL, descomprimir, &f[i])) { descomprimir(&f[i]); f[i].hilo = 0; }
    descomprimir(&f[0]);
    int ok = f[0].ok;
    for (int i = 1; i < n; i++) {
        if (f[i].hilo) pthread_join(f[i].hilo, NULL);
        ok &= f[i].ok;
    }
    return ok ? 0 : -1;
}

static void *hilo_red(void *arg)
{
    (void)arg;
    int baliza = socket(AF_INET, SOCK_DGRAM, 0);
    int uno = 1;
    setsockopt(baliza, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(DL_PUERTO_BALIZA);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(baliza, (struct sockaddr *)&a, sizeof a) < 0) LOG("No se puede escuchar las balizas: %s", strerror(errno));

    uint8_t *datos = NULL;
    size_t datos_cap = 0;
    char ignorar[64] = "";
    uint64_t ignorar_hasta = 0;

    while (!salir) {
        /* 1) esperar una baliza de la Deck con el wifi activado */
        struct pollfd pf = { baliza, POLLIN, 0 };
        if (poll(&pf, 1, 500) <= 0) continue;
        char msg[160], ip[64];
        struct sockaddr_in de;
        socklen_t l = sizeof de;
        ssize_t k = recvfrom(baliza, msg, sizeof msg - 1, 0, (struct sockaddr *)&de, &l);
        if (k <= 0) continue;
        msg[k] = 0;
        int puerto = 0, wifi = 0;
        if (sscanf(msg, DL_BALIZA " %d %d", &puerto, &wifi) < 2) continue;
        if (!wifi) { atomic_store(&estado, E_WIFI_OFF); continue; }
        inet_ntop(AF_INET, &de.sin_addr, ip, sizeof ip);
        if (!strcmp(ip, ignorar) && ms_ahora() < ignorar_hasta) continue;

        /* 2) conectar */
        int s = conectar(ip, 2000);
        if (s < 0) continue;
        LOG("Conectando con la Deck en %s", ip);
        enviar(s, DL_HOLA, DL_CAP_420, leer_pin());
        sock = s;
        dl_frame_hdr h;
        int conectado = 0;
        while (!salir && leer_todo(s, &h, sizeof h) == 0) {
            if (h.magic != DL_MAGIC || h.bytes > 64u << 20) { atomic_store(&estado, E_OTRA_VERSION); break; }
            if (h.bytes > datos_cap) {
                free(datos);
                datos_cap = h.bytes + (h.bytes >> 2);
                datos = malloc(datos_cap);
                if (!datos) { datos_cap = 0; break; }
            }
            if (h.bytes && leer_todo(s, datos, h.bytes)) break;
            if (h.fmt == DL_FMT_NADA) continue;
            if (h.fmt == DL_FMT_RECHAZO) {
                LOG("La Deck no acepta la conexión (%d)", h.w);
                atomic_store(&estado, h.w == DL_RECHAZO_PIN ? E_PIN : E_WIFI_OFF);
                snprintf(ignorar, sizeof ignorar, "%s", ip);
                ignorar_hasta = ms_ahora() + (h.w == DL_RECHAZO_PIN ? 4000 : 8000);
                break;
            }
            if (!conectado) { conectado = 1; atomic_store(&estado, E_CONECTADO); LOG("Conectado"); }
            if (h.fmt == DL_FMT_AUDIO) { audio_poner(datos, h.bytes); continue; }
            if (h.fmt != DL_FMT_JPEG || !h.w || !h.h) continue;
            enviar(s, DL_ACK, 0, h.reservado[2]);      /* la Deck ya puede mandar el siguiente */
            size_t px = (size_t)h.w * h.h * 4;
            if (px > b_cap) {
                pthread_mutex_lock(&mtx_img);
                free(b_fondo); free(b_listo); free(b_uso);
                b_fondo = calloc(1, px); b_listo = calloc(1, px); b_uso = calloc(1, px);
                b_cap = (b_fondo && b_listo && b_uso) ? px : 0;
                nuevo = 0;
                pthread_mutex_unlock(&mtx_img);
                if (!b_cap) break;
            }
            if (decodificar(datos, h.bytes, h.w, h.h, b_fondo)) continue;
            pthread_mutex_lock(&mtx_img);
            uint8_t *t = b_listo; b_listo = b_fondo; b_fondo = t;
            img_w = h.w; img_h = h.h; nuevo = 1;
            pthread_mutex_unlock(&mtx_img);
        }
        sock = -1;
        close(s);
        if (atomic_load(&estado) == E_CONECTADO) atomic_store(&estado, E_BUSCANDO);
        LOG("Desconectado");
    }
    close(baliza);
    return NULL;
}

/* ======================= panel de texto (menú y avisos) ======================= */

#define UI_W 1024
#define UI_H 640
static uint32_t ui[UI_W * UI_H];         /* RGBA (byte R primero) */

static uint32_t rgba(int r, int g, int b) { return 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }

static void ui_rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < UI_H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < UI_W; i++) ui[j * UI_W + i] = c;
}

static int glifo(const char **t)
{
    const unsigned char *s = (const unsigned char *)*t;
    unsigned cp = *s++;
    if (cp >= 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        cp = (cp & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F); s += 2;
    } else if (cp >= 0xC0 && (s[0] & 0xC0) == 0x80) {
        cp = (cp & 0x1F) << 6 | (s[0] & 0x3F); s += 1;
    }
    *t = (const char *)s;
    if (cp >= 32 && cp < 127) return (int)cp - 32;
    for (int i = 95; i < FUENTE_N; i++) if (fuente_cp[i] == cp) return i;
    return -1;
}

/* texto de altura «alto» (px) con la esquina superior en x,y; devuelve el ancho. dibujar = 0: solo medir */
static float ui_texto(float x, float y, float alto, const char *t, uint32_t c, int dibujar)
{
    float e = alto / FUENTE_ALTO, x0 = x;
    int cr = c & 255, cg = c >> 8 & 255, cb = c >> 16 & 255;
    while (*t) {
        int g = glifo(&t);
        if (g < 0) { x += 16 * e; continue; }
        float gw = fuente_w[g] * e;
        if (dibujar) {
            int ix0 = (int)floorf(x), ix1 = (int)ceilf(x + gw), iy0 = (int)floorf(y), iy1 = (int)ceilf(y + alto);
            for (int j = iy0; j < iy1; j++) {
                if (j < 0 || j >= UI_H) continue;
                float sy = (j + 0.5f - y) / e - 0.5f;
                int y0 = (int)floorf(sy);
                float fy = sy - y0;
                for (int i = ix0; i < ix1; i++) {
                    if (i < 0 || i >= UI_W) continue;
                    float sx = (i + 0.5f - x) / e - 0.5f;
                    int x0i = (int)floorf(sx);
                    float fx = sx - x0i;
                    float acc = 0;
                    for (int q = 0; q < 4; q++) {           /* bilineal dentro del glifo */
                        int xx = x0i + (q & 1), yy = y0 + (q >> 1);
                        if (xx < 0 || xx >= fuente_w[g] || yy < 0 || yy >= FUENTE_ALTO) continue;
                        float wq = ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy);
                        acc += wq * fuente_alfa[(fuente_y[g] + yy) * FUENTE_ANCHO + fuente_x[g] + xx];
                    }
                    float a = acc / 255.f;
                    if (a <= 0.004f) continue;
                    uint32_t o = ui[j * UI_W + i];
                    int r = (int)((o & 255) * (1 - a) + cr * a);
                    int gg = (int)((o >> 8 & 255) * (1 - a) + cg * a);
                    int b = (int)((o >> 16 & 255) * (1 - a) + cb * a);
                    ui[j * UI_W + i] = rgba(r, gg, b);
                }
            }
        }
        x += gw;
    }
    return x - x0;
}

/* párrafo con saltos de línea automáticos; devuelve la y siguiente */
static float ui_parrafo(float x, float y, float ancho, float alto, const char *t, uint32_t c)
{
    char linea[256] = "", prueba[256];
    const char *p = t;
    while (*p) {
        const char *fin = strchr(p, ' ');
        size_t n = fin ? (size_t)(fin - p) : strlen(p);
        char palabra[128];
        if (n >= sizeof palabra) n = sizeof palabra - 1;
        memcpy(palabra, p, n); palabra[n] = 0;
        snprintf(prueba, sizeof prueba, "%s%s%s", linea, linea[0] ? " " : "", palabra);
        if (linea[0] && ui_texto(0, 0, alto, prueba, 0, 0) > ancho) {
            ui_texto(x, y, alto, linea, c, 1);
            y += alto * 1.3f;
            snprintf(linea, sizeof linea, "%s", palabra);
        } else snprintf(linea, sizeof linea, "%s", prueba);
        p += n;
        while (*p == ' ') p++;
    }
    if (linea[0]) { ui_texto(x, y, alto, linea, c, 1); y += alto * 1.3f; }
    return y;
}

#define C_PANEL rgba(23, 26, 33)
#define C_TEXTO rgba(235, 239, 244)
#define C_GRIS rgba(139, 146, 154)
#define C_AZUL rgba(26, 159, 255)
#define C_SEL rgba(61, 68, 80)
#define C_BORDE rgba(47, 55, 67)

enum { OP_IMAGEN = 0, OP_OJOS, OP_TAM, OP_RECOLOCAR, OP_VOLVER, OP_SALIR, OP_N };
static int menu_abierto, menu_sel;
static uint64_t aviso_hasta;     /* aviso breve al conectar */

static void ui_marco(const char *titulo)
{
    ui_rect(0, 0, UI_W, UI_H, C_PANEL);
    ui_rect(0, 0, 6, UI_H, C_AZUL);
    ui_texto(48, 34, 50, titulo, C_TEXTO, 1);
    char v[32];
    snprintf(v, sizeof v, "V%s", DL_VERSION);
    float w = ui_texto(0, 0, 26, v, 0, 0);
    ui_texto(UI_W - 48 - w, 50, 26, v, rgba(103, 112, 123), 1);
    ui_rect(48, 112, UI_W - 96, 2, C_BORDE);
}

static void ui_dibujar(void)
{
    static const char *const NOM_TAM[4] = { "Pequeña", "Mediana", "Grande", "Cine" };
    if (menu_abierto) {
        ui_marco("DeckLink VR");
        const char *tit[OP_N] = { "Imagen", "Ojos", "Tamaño", "Recolocar delante de mí", "Volver al juego", "Salir de DeckLink VR" };
        const char *val[OP_N] = { aj.modo3d ? "3D lado a lado" : "2D", aj.invertir ? "Invertidos" : "Normal",
                                  NOM_TAM[aj.tam], NULL, NULL, NULL };
        float y = 130;
        for (int i = 0; i < OP_N; i++) {
            float alto = 62;
            if (i == OP_RECOLOCAR) y += 10;
            if (i == menu_sel) {
                ui_rect(36, (int)y, UI_W - 72, (int)alto, C_SEL);
                ui_rect(36, (int)y, 6, (int)alto, C_AZUL);
            }
            uint32_t ct = (i == OP_OJOS && !aj.modo3d) ? C_GRIS : C_TEXTO;
            ui_texto(60, y + 13, 34, tit[i], ct, 1);
            if (val[i]) {
                char b[64];
                snprintf(b, sizeof b, i == menu_sel ? "‹  %s  ›" : "%s", val[i]);
                float w = ui_texto(0, 0, 32, b, 0, 0);
                ui_texto(UI_W - 60 - w, y + 14, 32, b, (i == OP_OJOS && !aj.modo3d) ? C_GRIS : C_AZUL, 1);
            }
            y += alto + 6;
        }
        ui_rect(48, UI_H - 76, UI_W - 96, 2, C_BORDE);
        ui_texto(48, UI_H - 56, 26, "Palanca: elegir y cambiar   ·   A / gatillo: aceptar   ·   B: cerrar", C_GRIS, 1);
        return;
    }
    int e = atomic_load(&estado);
    if (e == E_CONECTADO) {                      /* aviso breve al conectar */
        ui_marco("Conectado a la Steam Deck");
        float y = ui_parrafo(48, 140, UI_W - 96, 32,
                             "Pulsa B (o el botón ≡) para el menú: 2D o 3D, tamaño de la pantalla y recolocar.", C_TEXTO);
        ui_parrafo(48, y + 20, UI_W - 96, 28,
                   "Para ver en 3D, activa en la Deck un efecto «lado a lado» (por ejemplo ReShade SuperDepth3D) "
                   "o el modo 3D lado a lado del juego.", C_GRIS);
        return;
    }
    const char *t = "Buscando la Steam Deck", *p1, *p2;
    p1 = "En la Deck: icono DeckLink › Activar la conexión por wifi.";
    p2 = "Las gafas y la Deck tienen que estar en la misma red wifi.";
    if (e == E_PIN) {
        t = "La Deck pide un PIN";
        p1 = "Sal de la VR, abre DeckLink normal, conéctate una vez por wifi y escribe el PIN que sale en la Deck.";
        p2 = "Después vuelve a «Ver en VR»: ya no lo pedirá.";
    } else if (e == E_WIFI_OFF) {
        t = "Wifi desactivado en la Deck";
        p1 = "En la Deck: icono DeckLink › Activar la conexión por wifi.";
        p2 = "La VR solo puede conectarse por wifi.";
    } else if (e == E_OTRA_VERSION) {
        t = "Otra versión en la Deck";
        p1 = "Actualiza DeckLink en la Deck y en las gafas a la misma versión.";
        p2 = "";
    }
    ui_marco(t);
    float y = ui_parrafo(48, 150, UI_W - 96, 34, p1, C_TEXTO);
    ui_parrafo(48, y + 16, UI_W - 96, 30, p2, C_GRIS);
    ui_rect(48, UI_H - 76, UI_W - 96, 2, C_BORDE);
    ui_texto(48, UI_H - 56, 26, "B o ≡: menú", C_GRIS, 1);
}

/* ======================= EGL ======================= */

static EGLDisplay dpy;
static EGLConfig cfg;
static EGLContext ctx;
static EGLSurface pbuf;

static int egl_iniciar(void)
{
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) return -1;
    const EGLint atr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                           EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT_KHR */,
                           EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(dpy, atr, &cfg, 1, &n) || n < 1) return -1;
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT) return -1;
    const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    pbuf = eglCreatePbufferSurface(dpy, cfg, pa);
    if (!eglMakeCurrent(dpy, pbuf, pbuf, ctx)) return -1;
    return 0;
}

/* ======================= OpenXR ======================= */

#define XRC(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOG("%s falló: %d", #x, (int)r_); return -1; } } while (0)

static XrInstance inst;
static XrSystemId sistema;
static XrSession ses;
static XrSpace local, vista;
static int64_t formato;
static int ses_en_marcha, ses_enfocada;

typedef struct {
    XrSwapchain sc;
    XrSwapchainImageOpenGLESKHR img[8];
    uint32_t n;
    int w, h, listo;
} cadena_t;
static cadena_t c_img, c_ui;

static XrActionSet acciones;
static XrAction a_aceptar, a_atras, a_menu, a_palanca;

/* posición de la pantalla y del panel (espacio LOCAL) */
static XrPosef pose_pantalla, pose_panel;

static int cadena_crear(cadena_t *c, int w, int h)
{
    if (c->sc) { xrDestroySwapchain(c->sc); c->sc = XR_NULL_HANDLE; }
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = formato;
    ci.sampleCount = 1;
    ci.width = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRC(xrCreateSwapchain(ses, &ci, &c->sc));
    for (int i = 0; i < 8; i++) { c->img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR; c->img[i].next = NULL; }
    XRC(xrEnumerateSwapchainImages(c->sc, 8, &c->n, (XrSwapchainImageBaseHeader *)c->img));
    c->w = w; c->h = h; c->listo = 0;
    return 0;
}

static void cadena_subir(cadena_t *c, const void *px)
{
    uint32_t i;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(c->sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(c->sc, &wi);
    glBindTexture(GL_TEXTURE_2D, c->img[i].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c->w, c->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFlush();
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(c->sc, &ri);
    c->listo = 1;
}

static XrPath camino(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

static int accion(XrAction *a, const char *nombre, const char *visible, XrActionType tipo)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = tipo;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", nombre);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", visible);
    XRC(xrCreateAction(acciones, &ci, a));
    return 0;
}

static int xr_acciones(void)
{
    XrActionSetCreateInfo ai = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(ai.actionSetName, sizeof ai.actionSetName, "decklink");
    snprintf(ai.localizedActionSetName, sizeof ai.localizedActionSetName, "DeckLink");
    XRC(xrCreateActionSet(inst, &ai, &acciones));
    if (accion(&a_aceptar, "aceptar", "Aceptar", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        accion(&a_atras, "atras", "Atrás", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        accion(&a_menu, "menu", "Menú", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        accion(&a_palanca, "palanca", "Palanca", XR_ACTION_TYPE_VECTOR2F_INPUT)) return -1;

    XrActionSuggestedBinding t[] = {
        { a_aceptar, camino("/user/hand/right/input/a/click") },
        { a_aceptar, camino("/user/hand/left/input/x/click") },
        { a_aceptar, camino("/user/hand/right/input/trigger/value") },
        { a_aceptar, camino("/user/hand/left/input/trigger/value") },
        { a_atras, camino("/user/hand/right/input/b/click") },
        { a_atras, camino("/user/hand/left/input/y/click") },
        { a_menu, camino("/user/hand/left/input/menu/click") },
        { a_palanca, camino("/user/hand/right/input/thumbstick") },
        { a_palanca, camino("/user/hand/left/input/thumbstick") },
    };
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = camino("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = sizeof t / sizeof t[0];
    sb.suggestedBindings = t;
    if (XR_FAILED(xrSuggestInteractionProfileBindings(inst, &sb))) LOG("Sin controles Touch");

    XrActionSuggestedBinding s[] = {
        { a_aceptar, camino("/user/hand/right/input/select/click") },
        { a_aceptar, camino("/user/hand/left/input/select/click") },
        { a_menu, camino("/user/hand/left/input/menu/click") },
    };
    XrInteractionProfileSuggestedBinding sb2 = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb2.interactionProfile = camino("/interaction_profiles/khr/simple_controller");
    sb2.countSuggestedBindings = sizeof s / sizeof s[0];
    sb2.suggestedBindings = s;
    xrSuggestInteractionProfileBindings(inst, &sb2);

    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &acciones;
    XRC(xrAttachSessionActionSets(ses, &at));
    return 0;
}

static int xr_iniciar(void)
{
    PFN_xrInitializeLoaderKHR iniciar_cargador = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&iniciar_cargador);
    if (iniciar_cargador) {
        XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM = app->activity->vm;
        li.applicationContext = app->activity->clazz;
        iniciar_cargador((const XrLoaderInitInfoBaseHeaderKHR *)&li);
    }
    const char *ext[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfoAndroidKHR ia = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    ia.applicationVM = app->activity->vm;
    ia.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    ci.next = &ia;
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "DeckLink VR");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "DeckLink");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = ext;
    XRC(xrCreateInstance(&ci, &inst));

    XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO };
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRC(xrGetSystem(inst, &sg, &sistema));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR req = NULL;
    XRC(xrGetInstanceProcAddr(inst, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req));
    XrGraphicsRequirementsOpenGLESKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XRC(req(inst, sistema, &gr));

    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    gb.display = dpy;
    gb.config = cfg;
    gb.context = ctx;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = sistema;
    XRC(xrCreateSession(inst, &sci, &ses));

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRC(xrCreateReferenceSpace(ses, &rs, &local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRC(xrCreateReferenceSpace(ses, &rs, &vista));

    /* formato: sRGB si se puede (los colores salen como en la pantalla de la Deck) */
    uint32_t n = 0;
    int64_t f[64];
    XRC(xrEnumerateSwapchainFormats(ses, 64, &n, f));
    formato = 0;
    for (uint32_t i = 0; i < n; i++) if (f[i] == 0x8C43 /* GL_SRGB8_ALPHA8 */) formato = f[i];
    if (!formato) for (uint32_t i = 0; i < n; i++) if (f[i] == 0x8058 /* GL_RGBA8 */) formato = f[i];
    if (!formato && n) formato = f[0];

    if (cadena_crear(&c_ui, UI_W, UI_H)) return -1;
    return xr_acciones();
}

/* sitúa una pose a «dist» metros delante de la cabeza, mirando hacia ella (solo giro horizontal) */
static XrPosef delante(XrTime t, float dist, float bajar)
{
    XrPosef p = { { 0, 0, 0, 1 }, { 0, -bajar, -dist } };
    XrSpaceLocation l = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(vista, local, t, &l)) ||
        !(l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return p;
    XrQuaternionf q = l.pose.orientation;
    /* adelante = q * (0,0,-1) */
    float fx = -2 * (q.x * q.z + q.w * q.y);
    float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float m = sqrtf(fx * fx + fz * fz);
    if (m < 1e-3f) { fx = 0; fz = -1; } else { fx /= m; fz /= m; }
    float yaw = atan2f(-fx, -fz);
    p.orientation.y = sinf(yaw / 2);
    p.orientation.w = cosf(yaw / 2);
    p.position.x = l.pose.position.x + fx * dist;
    p.position.y = l.pose.position.y - bajar;
    p.position.z = l.pose.position.z + fz * dist;
    return p;
}

/* la pantalla conserva su centro de giro: al cambiar de tamaño, se aleja o se acerca */
static XrPosef pose_base;          /* delante de la cabeza a 1 m (al recolocar) */
static void colocar_pantalla(void)
{
    /* desde la posición base (a 1 m), mover a DIST_TAM a lo largo de su eje */
    float d = DIST_TAM[aj.tam] - 1.0f;
    XrQuaternionf q = pose_base.orientation;
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    pose_pantalla = pose_base;
    pose_pantalla.position.x += fx * d;
    pose_pantalla.position.z += fz * d;
}

static int boton(XrAction a)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ses, &gi, &s))) return 0;
    return s.isActive && s.changedSinceLastSync && s.currentState;
}

static int ui_sucio = 1;

static void cambiar(int dir)
{
    switch (menu_sel) {
    case OP_IMAGEN: aj.modo3d = !aj.modo3d; break;
    case OP_OJOS: if (aj.modo3d) aj.invertir = !aj.invertir; break;
    case OP_TAM: aj.tam = (aj.tam + (dir < 0 ? 3 : 1)) % 4; colocar_pantalla(); break;
    default: return;
    }
    guardar_ajustes();
}

static void entrada(XrTime t)
{
    if (!ses_enfocada) return;
    XrActiveActionSet aa = { acciones, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aa;
    if (XR_FAILED(xrSyncActions(ses, &si))) return;

    int abrir = boton(a_menu) | boton(a_atras);
    int ok = boton(a_aceptar);
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a_palanca;
    XrActionStateVector2f pv = { XR_TYPE_ACTION_STATE_VECTOR2F };
    xrGetActionStateVector2f(ses, &gi, &pv);
    static int previo;                                    /* dirección de la palanca (con histéresis) */
    int dir = 0;
    if (pv.isActive) {
        float x = pv.currentState.x, y = pv.currentState.y;
        if (y > 0.7f) dir = 1; else if (y < -0.7f) dir = 2;
        else if (x > 0.7f) dir = 3; else if (x < -0.7f) dir = 4;
        else if (fabsf(x) > 0.3f || fabsf(y) > 0.3f) dir = previo;
    }
    int pulso = dir != previo ? dir : 0;
    previo = dir;

    if (!menu_abierto) {
        if (abrir) {
            menu_abierto = 1;
            menu_sel = OP_IMAGEN;
            pose_panel = delante(t, 1.3f, 0.15f);
            aviso_hasta = 0;
            ui_sucio = 1;
        }
        return;
    }
    if (abrir) { menu_abierto = 0; ui_sucio = 1; return; }
    if (pulso == 1) menu_sel = (menu_sel + OP_N - 1) % OP_N;
    else if (pulso == 2) menu_sel = (menu_sel + 1) % OP_N;
    else if (pulso == 3) cambiar(1);
    else if (pulso == 4) cambiar(-1);
    if (ok) {
        if (menu_sel <= OP_TAM) cambiar(1);
        else if (menu_sel == OP_RECOLOCAR) {
            pose_base = delante(t, 1.0f, 0);
            colocar_pantalla();
            pose_panel = delante(t, 1.3f, 0.15f);
        } else if (menu_sel == OP_VOLVER) menu_abierto = 0;
        else if (menu_sel == OP_SALIR) {
            atomic_store(&salir, 1);
        }
    }
    if (pulso || ok) ui_sucio = 1;
}

static void xr_eventos(void)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *c = (XrEventDataSessionStateChanged *)&ev;
            LOG("Sesión: estado %d", (int)c->state);
            if (c->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(ses, &bi))) ses_en_marcha = 1;
            } else if (c->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(ses);
                ses_en_marcha = 0;
            } else if (c->state == XR_SESSION_STATE_EXITING || c->state == XR_SESSION_STATE_LOSS_PENDING) {
                atomic_store(&salir, 1);
            }
            ses_enfocada = c->state == XR_SESSION_STATE_FOCUSED;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            atomic_store(&salir, 1);
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

static void fotograma(void)
{
    XrFrameWaitInfo fw = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(ses, &fw, &fs))) return;
    XrFrameBeginInfo fb = { XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame(ses, &fb))) return;

    static int colocada;
    if (!colocada) {                       /* la primera vez, delante de la cabeza */
        pose_base = delante(fs.predictedDisplayTime, 1.0f, 0);
        colocar_pantalla();
        pose_panel = delante(fs.predictedDisplayTime, 1.5f, 0.1f);
        colocada = 1;
    }
    entrada(fs.predictedDisplayTime);

    /* imagen nueva de la Deck */
    pthread_mutex_lock(&mtx_img);
    int hay = nuevo, w = img_w, h = img_h;
    if (hay) { uint8_t *t = b_uso; b_uso = b_listo; b_listo = t; nuevo = 0; }
    pthread_mutex_unlock(&mtx_img);
    if (hay) {
        if (!c_img.sc || c_img.w != w || c_img.h != h) cadena_crear(&c_img, w, h);
        if (c_img.sc) cadena_subir(&c_img, b_uso);
    }

    /* el panel: avisos cuando no hay imagen, el menú o el aviso breve al conectar */
    static int estado_previo = -1;
    int e = atomic_load(&estado);
    if (e != estado_previo) {
        if (e == E_CONECTADO) aviso_hasta = ms_ahora() + 6000;
        estado_previo = e;
        ui_sucio = 1;
    }
    int ver_panel = menu_abierto || e != E_CONECTADO || ms_ahora() < aviso_hasta || !c_img.listo;
    if (ver_panel && ui_sucio) {
        ui_dibujar();
        cadena_subir(&c_ui, ui);
        ui_sucio = 0;
    }
    if (!ver_panel) ui_sucio = 1;

    XrCompositionLayerQuad capas[3];
    const XrCompositionLayerBaseHeader *lista[3];
    uint32_t n = 0;
    if (fs.shouldRender && c_img.listo && e == E_CONECTADO) {
        float ancho = ANCHO_TAM[aj.tam];
        float alto = ancho * (float)c_img.h / (float)c_img.w;   /* 16:10: en 3D cada mitad se ve a lo ancho */
        int vistas = aj.modo3d ? 2 : 1;
        for (int i = 0; i < vistas; i++) {
            XrCompositionLayerQuad *q = &capas[n];
            memset(q, 0, sizeof *q);
            q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            q->space = local;
            q->subImage.swapchain = c_img.sc;
            q->pose = pose_pantalla;
            q->size.width = ancho;
            q->size.height = alto;
            if (vistas == 1) {
                q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q->subImage.imageRect.extent.width = c_img.w;
                q->subImage.imageRect.extent.height = c_img.h;
            } else {
                int izq = (i == 0) != aj.invertir;        /* qué mitad ve este ojo */
                q->eyeVisibility = i == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q->subImage.imageRect.offset.x = izq ? 0 : c_img.w / 2;
                q->subImage.imageRect.extent.width = c_img.w / 2;
                q->subImage.imageRect.extent.height = c_img.h;
            }
            lista[n] = (const XrCompositionLayerBaseHeader *)q;
            n++;
        }
    }
    if (fs.shouldRender && ver_panel && c_ui.listo) {
        XrCompositionLayerQuad *q = &capas[n];
        memset(q, 0, sizeof *q);
        q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        q->space = local;
        q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q->subImage.swapchain = c_ui.sc;
        q->subImage.imageRect.extent.width = UI_W;
        q->subImage.imageRect.extent.height = UI_H;
        q->pose = pose_panel;
        q->size.width = 0.9f;
        q->size.height = 0.9f * UI_H / UI_W;
        lista[n] = (const XrCompositionLayerBaseHeader *)q;
        n++;
    }
    XrFrameEndInfo fe = { XR_TYPE_FRAME_END_INFO };
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = n;
    fe.layers = lista;
    xrEndFrame(ses, &fe);
}

/* ======================= principal ======================= */

static void al_comando(struct android_app *a, int32_t cmd)
{
    (void)a;
    if (cmd == APP_CMD_DESTROY) atomic_store(&salir, 1);
}

void android_main(struct android_app *a)
{
    app = a;
    a->onAppCmd = al_comando;
    LOG("DeckLink VR %s", DL_VERSION);
    cargar_ajustes();
    if (egl_iniciar()) { LOG("EGL no disponible"); ANativeActivity_finish(a->activity); }
    else if (xr_iniciar()) { LOG("OpenXR no disponible"); ANativeActivity_finish(a->activity); }
    else {
        audio_abrir();
        pthread_t red;
        pthread_create(&red, NULL, hilo_red, NULL);
        pthread_detach(red);
    }
    while (!a->destroyRequested) {
        int ev;
        struct android_poll_source *src;
        int espera = (ses_en_marcha && !salir) ? 0 : 100;
        while (ALooper_pollOnce(espera, NULL, &ev, (void **)&src) >= 0) {
            if (src) src->process(a, src);
            if (a->destroyRequested) break;
            espera = 0;
        }
        if (a->destroyRequested) break;
        if (salir) {
            static int terminado;
            if (!terminado) { terminado = 1; ANativeActivity_finish(a->activity); }
            continue;
        }
        if (!inst) continue;
        xr_eventos();
        if (ses_en_marcha) fotograma();
    }
    atomic_store(&salir, 1);
    int s = sock;
    if (s >= 0) shutdown(s, SHUT_RDWR);
    if (audio) { AAudioStream_requestStop(audio); AAudioStream_close(audio); audio = NULL; }
    if (ses) xrDestroySession(ses);
    if (inst) xrDestroyInstance(inst);
    LOG("Fin");
}
