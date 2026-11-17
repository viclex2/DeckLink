/*
 * decklink-portatil — la pantalla de DeckLink, para Linux y Windows.
 * Muestra a pantalla completa la imagen de la Steam Deck, reproduce su sonido
 * y le manda el teclado y el ratón/touchpad.
 *
 * Busca la Deck primero por el cable USB (10.77.0.1) y, si no, por wifi
 * (la Deck avisa con balizas UDP). Por wifi pide el PIN que muestra la Deck.
 *
 * Uso: decklink-portatil [ip_de_la_deck]
 * Salir: Ctrl + Alt + Mayús + Q
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
   typedef SOCKET sock_t;
#  define SOCK_NULO INVALID_SOCKET
#  define cerrar_sock closesocket
#  define MSG_NOSIGNAL 0
#else
#  define _GNU_SOURCE
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
   typedef int sock_t;
#  define SOCK_NULO (-1)
#  define cerrar_sock close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <turbojpeg.h>
#include "terceros/lz4.h"
#include "protocolo.h"
#include "teclas.h"
#include "recursos.h"

/* ------------------------------------------------------------------ */
/* Plataforma                                                          */
/* ------------------------------------------------------------------ */
static void ruta_conf(char *out, size_t n, const char *archivo)
{
#ifdef _WIN32
    const char *b = getenv("APPDATA");
    char dir[600];
    snprintf(dir, sizeof dir, "%s\\DeckLink", b ? b : ".");
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\%s", dir, archivo);
#else
    const char *h = getenv("HOME");
    char dir[600];
    snprintf(dir, sizeof dir, "%s/.config", h ? h : "/tmp");
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.config/decklink", h ? h : "/tmp");
    mkdir(dir, 0700);
    snprintf(out, n, "%s/%s", dir, archivo);
#endif
}

static void preparar_log(void)
{
    char ruta[700];
#ifdef _WIN32
    const char *b = getenv("LOCALAPPDATA");
    char dir[600];
    snprintf(dir, sizeof dir, "%s\\DeckLink", b ? b : ".");
    CreateDirectoryA(dir, NULL);
    snprintf(ruta, sizeof ruta, "%s\\decklink.log", dir);
#else
    if (isatty(1)) return;
    const char *h = getenv("HOME");
    if (!h) return;
    snprintf(ruta, sizeof ruta, "%s/.cache", h);
    mkdir(ruta, 0700);
    snprintf(ruta, sizeof ruta, "%s/.cache/decklink-portatil.log", h);
#endif
    if (freopen(ruta, "w", stdout)) {
#ifdef _WIN32
        _dup2(_fileno(stdout), 2);
#else
        dup2(1, 2);
#endif
    }
    setvbuf(stdout, NULL, _IONBF, 0);
}

static int una_sola_vez(void)
{
#ifdef _WIN32
    CreateMutexA(NULL, TRUE, "DeckLinkPantalla");
    return GetLastError() != ERROR_ALREADY_EXISTS;
#else
    char ruta[256];
    snprintf(ruta, sizeof ruta, "/tmp/decklink-portatil-%u.lock", (unsigned)getuid());
    int fd = open(ruta, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
#endif
}

static void no_bloqueante(sock_t s, int si)
{
#ifdef _WIN32
    u_long v = si ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, si ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
#endif
}

/* conectar con un tiempo máximo (para no quedarse colgado buscando la Deck) */
static sock_t conectar(const char *ip, int ms)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(DL_PUERTO);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return SOCK_NULO;
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_NULO) return SOCK_NULO;
    no_bloqueante(s, 1);
    connect(s, (struct sockaddr *)&a, sizeof a);
    fd_set w, e;
    FD_ZERO(&w); FD_SET(s, &w);
    FD_ZERO(&e); FD_SET(s, &e);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int ok = 0;
    if (select((int)s + 1, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) {
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &l);
        ok = err == 0;
    }
    if (!ok) { cerrar_sock(s); return SOCK_NULO; }
    no_bloqueante(s, 0);
    int uno = 1, rb = 8 << 20;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&uno, sizeof uno);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof rb);
    return s;
}

static int recv_todo(sock_t s, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        int r = recv(s, p, (int)(n > (1u << 30) ? (1u << 30) : n), 0);
        if (r <= 0) {
#ifndef _WIN32
            if (r < 0 && errno == EINTR) continue;
#endif
            return -1;
        }
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Estado compartido                                                   */
/* ------------------------------------------------------------------ */
static const char *ip_forzada = NULL;
static sock_t sock = SOCK_NULO;
static SDL_mutex *mtx_frame, *mtx_envio;
static SDL_sem *sem_pin;
static SDL_atomic_t pin_cancelado;
static int32_t pin_guardado = -1;

typedef struct { uint8_t *p; size_t cap; } buf_t;
static buf_t frente, fondo, comprimido;
static int fw, fh, ffmt, hay_frame;
static Uint32 EV_FRAME, EV_ESTADO;
static SDL_AudioDeviceID audio_dev;
static uint8_t pulsadas[KEY_CNT];
static volatile unsigned long n_recibidos;

enum { ESTADO_ESPERANDO = 0, ESTADO_CONECTADO = 1, ESTADO_PIDE_PIN = 2 };

static void reservar(buf_t *b, size_t n)
{
    if (b->cap >= n) return;
    free(b->p);
    b->p = malloc(n);
    b->cap = n;
}

static void enviar(uint16_t type, uint16_t code, int32_t value)
{
    dl_input m = { type, code, value };
    SDL_LockMutex(mtx_envio);
    if (sock != SOCK_NULO) send(sock, (const char *)&m, sizeof m, MSG_NOSIGNAL);
    SDL_UnlockMutex(mtx_envio);
}

static void tecla(uint16_t code, int abajo)
{
    if (!code || code >= KEY_CNT) return;
    if (pulsadas[code] == abajo) return;
    pulsadas[code] = (uint8_t)abajo;
    enviar(EV_KEY, code, abajo);
}

static void soltar_todo(void)
{
    for (int k = 0; k < KEY_CNT; k++)
        if (pulsadas[k]) tecla((uint16_t)k, 0);
}

static void avisar(Uint32 tipo, int valor)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = tipo;
    e.user.code = valor;
    SDL_PushEvent(&e);
}

static void cargar_pin(void)
{
    char ruta[700], v[32] = "";
    ruta_conf(ruta, sizeof ruta, "pin");
    FILE *f = fopen(ruta, "r");
    if (!f) return;
    if (fgets(v, sizeof v, f)) pin_guardado = (int32_t)strtol(v, NULL, 10);
    fclose(f);
}

static void guardar_pin(int32_t pin)
{
    char ruta[700];
    ruta_conf(ruta, sizeof ruta, "pin");
    FILE *f = fopen(ruta, "w");
    if (f) { fprintf(f, "%ld\n", (long)pin); fclose(f); }
}

/* ------------------------------------------------------------------ */
/* Descompresión por franjas en paralelo (JPEG o LZ4)                  */
/* ------------------------------------------------------------------ */
#define MAX_FRANJAS 16
typedef struct {
    tjhandle tj;
    const uint8_t *src;
    unsigned long tam;
    uint8_t *dst;
    int w, h, err, lz4;
    uint8_t *bgr;
    size_t bgr_cap;
} franja_t;
static franja_t franjas[MAX_FRANJAS];

static int descomprimir_franja(void *a)
{
    franja_t *f = a;
    size_t n = (size_t)f->w * f->h;
    if (!f->tam) { memset(f->dst, 0, n * 4); return 0; }
    if (!f->lz4) {
        f->err = tjDecompress2(f->tj, f->src, f->tam, f->dst, f->w, f->w * 4, f->h, TJPF_BGRX, TJFLAG_FASTDCT);
        return 0;
    }
    if (f->bgr_cap < n * 3) { free(f->bgr); f->bgr = malloc(n * 3); f->bgr_cap = n * 3; }
    int r = LZ4_decompress_safe((const char *)f->src, (char *)f->bgr, (int)f->tam, (int)(n * 3));
    if (r != (int)(n * 3)) { f->err = 1; memset(f->dst, 0, n * 4); return 0; }
    const uint8_t *s = f->bgr;
    uint32_t *o = (uint32_t *)f->dst;
    for (size_t i = 0; i < n; i++, s += 3)
        o[i] = 0xFF000000u | ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
    return 0;
}

static int decodificar_franjas(const uint8_t *datos, uint32_t bytes, int w, int h, uint8_t *dst, int lz4)
{
    if (bytes < 1) return -1;
    int n = datos[0];
    if (n < 1 || n > MAX_FRANJAS || bytes < 1u + 4u * n) return -1;
    const uint8_t *p = datos + 1 + 4 * n;
    uint32_t resto = bytes - 1 - 4 * n;
    for (int i = 0; i < n; i++) {
        uint32_t t;
        memcpy(&t, datos + 1 + 4 * i, 4);
        if (t > resto) return -1;
        franja_t *f = &franjas[i];
        if (!f->tj) f->tj = tjInitDecompress();
        f->src = p; f->tam = t;
        f->w = w; f->h = dl_franja_h(i, n, h);
        f->dst = dst + (size_t)dl_franja_y0(i, n, h) * w * 4;
        f->err = 0;
        f->lz4 = lz4;
        p += t; resto -= t;
    }
    SDL_Thread *th[MAX_FRANJAS];
    for (int i = 1; i < n; i++) th[i] = SDL_CreateThread(descomprimir_franja, "franja", &franjas[i]);
    descomprimir_franja(&franjas[0]);
    for (int i = 1; i < n; i++) SDL_WaitThread(th[i], NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Buscar la Deck: cable USB primero, luego balizas por wifi           */
/* ------------------------------------------------------------------ */
static sock_t baliza_sock = SOCK_NULO;
static char ignorar_ip[64];
static Uint32 ignorar_hasta;

static sock_t buscar_deck(char *ip, size_t n, int *por_usb)
{
    if (ip_forzada) {
        snprintf(ip, n, "%s", ip_forzada);
        *por_usb = !strcmp(ip_forzada, DL_IP_DECK);
        sock_t s = conectar(ip_forzada, 1500);
        if (s == SOCK_NULO) SDL_Delay(700);
        return s;
    }
    sock_t s = conectar(DL_IP_DECK, 500);
    if (s != SOCK_NULO) { snprintf(ip, n, DL_IP_DECK); *por_usb = 1; return s; }

    if (baliza_sock == SOCK_NULO) {
        baliza_sock = socket(AF_INET, SOCK_DGRAM, 0);
        int uno = 1;
        setsockopt(baliza_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&uno, sizeof uno);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(DL_PUERTO_BALIZA);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(baliza_sock, (struct sockaddr *)&a, sizeof a) < 0) {
            cerrar_sock(baliza_sock);
            baliza_sock = SOCK_NULO;
            SDL_Delay(1000);
            return SOCK_NULO;
        }
    }
    Uint32 fin = SDL_GetTicks() + 1500;
    while (SDL_GetTicks() < fin) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(baliza_sock, &r);
        struct timeval tv = { 0, 300000 };
        if (select((int)baliza_sock + 1, &r, NULL, NULL, &tv) <= 0) continue;
        char msg[160];
        struct sockaddr_in de;
        socklen_t l = sizeof de;
        int k = recvfrom(baliza_sock, msg, sizeof msg - 1, 0, (struct sockaddr *)&de, &l);
        if (k <= 0) continue;
        msg[k] = 0;
        int puerto = 0, wifi = 0;
        char nombre[64] = "";
        if (sscanf(msg, DL_BALIZA " %d %d %63s", &puerto, &wifi, nombre) < 2 || !wifi) continue;
        char quien[64];
        inet_ntop(AF_INET, &de.sin_addr, quien, sizeof quien);
        if (!strcmp(quien, ignorar_ip) && SDL_GetTicks() < ignorar_hasta) continue;
        s = conectar(quien, 2000);
        if (s != SOCK_NULO) {
            printf("Steam Deck encontrada por wifi: %s (%s)\n", nombre, quien);
            snprintf(ip, n, "%s", quien);
            *por_usb = 0;
            return s;
        }
    }
    return SOCK_NULO;
}

/* ------------------------------------------------------------------ */
/* Hilo de red                                                         */
/* ------------------------------------------------------------------ */
static int hilo_red(void *arg)
{
    (void)arg;
    for (;;) {
        char ip[64];
        int por_usb = 0;
        sock_t s = buscar_deck(ip, sizeof ip, &por_usb);
        if (s == SOCK_NULO) continue;

        SDL_LockMutex(mtx_envio); sock = s; SDL_UnlockMutex(mtx_envio);
        enviar(DL_HOLA, DL_CAP_LZ4, pin_guardado);
        int avisado = 0, pedir_pin = 0;

        dl_frame_hdr h;
        while (recv_todo(s, &h, sizeof h) == 0) {
            if (h.magic != DL_MAGIC || h.bytes > 64u << 20 ||
                (h.fmt != DL_FMT_AUDIO && h.fmt != DL_FMT_RECHAZO && (!h.w || !h.h))) {
                fprintf(stderr, "La Deck tiene otra versión de DeckLink. Instala la misma versión en los dos.\n");
                SDL_Delay(2000);
                break;
            }
            if (h.fmt == DL_FMT_RECHAZO) {
                if (h.w == DL_RECHAZO_PIN) {
                    printf("La Deck pide el PIN\n");
                    pedir_pin = 1;
                } else {
                    printf("La Deck tiene desactivada la conexión por wifi\n");
                    snprintf(ignorar_ip, sizeof ignorar_ip, "%s", ip);
                    ignorar_hasta = SDL_GetTicks() + 10000;
                }
                break;
            }
            if (!avisado) {
                printf("Conectado a la Steam Deck por %s (%s)\n", por_usb ? "cable USB" : "wifi", ip);
                avisar(EV_ESTADO, ESTADO_CONECTADO);
                avisado = 1;
            }
            if (h.fmt == DL_FMT_AUDIO) {
                reservar(&comprimido, h.bytes);
                if (recv_todo(s, comprimido.p, h.bytes)) break;
                if (audio_dev) {
                    /* poco retraso: si se acumula más de 100 ms, empezar de nuevo */
                    if (SDL_GetQueuedAudioSize(audio_dev) > DL_AUDIO_HZ * 4 * 100 / 1000)
                        SDL_ClearQueuedAudio(audio_dev);
                    SDL_QueueAudio(audio_dev, comprimido.p, h.bytes);
                }
                continue;
            }
            size_t px = (size_t)h.w * h.h;
            if (h.fmt != DL_FMT_JPEG && h.fmt != DL_FMT_LZ4) break;
            reservar(&comprimido, h.bytes);
            if (recv_todo(s, comprimido.p, h.bytes)) break;
            enviar(DL_ACK, 0, 0);             /* la Deck ya puede preparar el siguiente */
            reservar(&fondo, px * 4);
            if (decodificar_franjas(comprimido.p, h.bytes, h.w, h.h, fondo.p, h.fmt == DL_FMT_LZ4)) continue;
            SDL_LockMutex(mtx_frame);
            buf_t t = frente; frente = fondo; fondo = t;
            fw = h.w; fh = h.h; ffmt = h.fmt; hay_frame = 1;
            n_recibidos++;
            SDL_UnlockMutex(mtx_frame);
            avisar(EV_FRAME, 0);
        }

        SDL_LockMutex(mtx_envio); sock = SOCK_NULO; SDL_UnlockMutex(mtx_envio);
        cerrar_sock(s);
        if (avisado) printf("Desconectado de la Steam Deck\n");
        avisar(EV_ESTADO, ESTADO_ESPERANDO);
        if (pedir_pin) {
            avisar(EV_ESTADO, ESTADO_PIDE_PIN);
            SDL_SemWait(sem_pin);            /* hasta que se escriba el PIN (o se cancele) */
            if (SDL_AtomicGet(&pin_cancelado)) {
                snprintf(ignorar_ip, sizeof ignorar_ip, "%s", ip);
                ignorar_hasta = SDL_GetTicks() + 30000;
            }
        } else {
            SDL_Delay(500);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dibujar cifras del PIN (siete segmentos)                            */
/* ------------------------------------------------------------------ */
static void dibujar_cifra(SDL_Renderer *r, int x, int y, int cifra)
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const int w = 70, t = 9;
    SDL_Rect rs[7] = {
        { x + 14, y + 10, w - 28, t },       /* a */
        { x + w - 18, y + 14, t, 36 },       /* b */
        { x + w - 18, y + 54, t, 36 },       /* c */
        { x + 14, y + 86, w - 28, t },       /* d */
        { x + 9, y + 54, t, 36 },            /* e */
        { x + 9, y + 14, t, 36 },            /* f */
        { x + 14, y + 48, w - 28, t },       /* g */
    };
    for (int i = 0; i < 7; i++)
        if (seg[cifra] & (1 << i)) SDL_RenderFillRect(r, &rs[i]);
}

static void dibujar_pin(SDL_Renderer *r, const char *cifras)
{
    int n = (int)strlen(cifras);
    int casillas = n < 4 ? 4 : (n < 8 ? n + 1 : 8);
    const int w = 70, h = 105, sep = 22;
    int x0 = (1280 - (casillas * w + (casillas - 1) * sep)) / 2, y = 450;
    for (int i = 0; i < casillas; i++) {
        SDL_Rect c = { x0 + i * (w + sep), y, w, h };
        SDL_SetRenderDrawColor(r, 255, 255, 255, 40);
        SDL_RenderFillRect(r, &c);
        SDL_SetRenderDrawColor(r, i == n ? 255 : 120, i == n ? 190 : 130, i == n ? 110 : 170, 255);
        SDL_RenderDrawRect(r, &c);
        if (i < n) {
            SDL_SetRenderDrawColor(r, 240, 243, 255, 255);
            dibujar_cifra(r, c.x, c.y + 4, cifras[i] - '0');
        }
    }
}

static SDL_Texture *textura_jpeg(SDL_Renderer *ren, const unsigned char *jpg, unsigned long tam)
{
    SDL_Texture *t = NULL;
    tjhandle tj = tjInitDecompress();
    int w, h, ss, cs;
    if (tjDecompressHeader3(tj, jpg, tam, &w, &h, &ss, &cs) == 0) {
        uint8_t *px = malloc((size_t)w * h * 4);
        tjDecompress2(tj, jpg, tam, px, w, w * 4, h, TJPF_BGRX, 0);
        t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL_UpdateTexture(t, NULL, px, w * 4);
        free(px);
    }
    tjDestroy(tj);
    return t;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) ip_forzada = argv[i];
    preparar_log();
    if (!una_sola_vez()) { printf("DeckLink ya está abierto\n"); return 0; }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SDL_SetMainReady();
    cargar_pin();

#ifndef _WIN32
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "decklink", 0);
    setenv("SDL_VIDEO_X11_WMCLASS", "decklink", 0);
#endif
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "DeckLink");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Sonido de la Steam Deck");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec quiero, tengo;
        SDL_zero(quiero);
        quiero.freq = DL_AUDIO_HZ;
        quiero.format = AUDIO_S16LSB;
        quiero.channels = DL_AUDIO_CANALES;
        quiero.samples = 512;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &quiero, &tengo, 0);
        if (audio_dev) {
            SDL_PauseAudioDevice(audio_dev, 0);
            printf("Sonido: %s\n", SDL_GetCurrentAudioDriver());
        }
    }
    if (!audio_dev) printf("Sin sonido: %s\n", SDL_GetError());

    SDL_Window *win = SDL_CreateWindow("DeckLink", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 800, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Surface *ico = SDL_CreateRGBSurfaceWithFormatFrom((void *)icono_rgba, 64, 64, 32, 64 * 4,
                                                          SDL_PIXELFORMAT_RGBA32);
    if (ico) { SDL_SetWindowIcon(win, ico); SDL_FreeSurface(ico); }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    SDL_Texture *tex_espera = textura_jpeg(ren, espera_jpg, sizeof espera_jpg);
    SDL_Texture *tex_pin = textura_jpeg(ren, pin_jpg, sizeof pin_jpg);
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0, tfmt = -1;
    int estado = ESTADO_ESPERANDO, viendo = 0;
    char cifras[9] = "";
    mtx_frame = SDL_CreateMutex();
    mtx_envio = SDL_CreateMutex();
    sem_pin = SDL_CreateSemaphore(0);
    EV_FRAME = SDL_RegisterEvents(2);
    EV_ESTADO = EV_FRAME + 1;

    printf("DeckLink — buscando la Steam Deck (cable USB o wifi)\n");
    SDL_DetachThread(SDL_CreateThread(hilo_red, "red", NULL));

    float acc_y = 0, acc_x = 0;
    unsigned long n_pintados = 0, rec0 = 0;
    Uint32 t_est = SDL_GetTicks();
    SDL_Event e;
    int seguir = 1, repintar = 1;
    while (seguir) {
        if (repintar) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (estado == ESTADO_PIDE_PIN) {
                SDL_RenderSetLogicalSize(ren, 1280, 800);
                if (tex_pin) SDL_RenderCopy(ren, tex_pin, NULL, NULL);
                dibujar_pin(ren, cifras);
            } else if (viendo && tex) {
                SDL_RenderSetLogicalSize(ren, tw, th);
                SDL_RenderCopy(ren, tex, NULL, NULL);
            } else if (tex_espera) {
                SDL_RenderSetLogicalSize(ren, 1280, 800);
                SDL_RenderCopy(ren, tex_espera, NULL, NULL);
            }
            SDL_RenderPresent(ren);
            repintar = 0;
            if (viendo) n_pintados++;
        }
        if (estado == ESTADO_CONECTADO && SDL_GetTicks() - t_est >= 30000) {
            printf("Últimos 30 s: recibidos %lu, pintados %lu\n", n_recibidos - rec0, n_pintados);
            rec0 = n_recibidos; n_pintados = 0; t_est = SDL_GetTicks();
        }
        if (!SDL_WaitEventTimeout(&e, 500)) { if (!viendo) repintar = 1; continue; }
        do {
            switch (e.type) {
            case SDL_QUIT:
                seguir = 0;
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (e.key.repeat && estado != ESTADO_PIDE_PIN) break;
                SDL_Keymod m = SDL_GetModState();
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_Q &&
                    (m & KMOD_CTRL) && (m & KMOD_ALT) && (m & KMOD_SHIFT)) {
                    seguir = 0;
                    break;
                }
                if (estado == ESTADO_PIDE_PIN) {
                    if (e.type != SDL_KEYDOWN) break;
                    SDL_Keycode k = e.key.keysym.sym;
                    int cifra = -1;
                    if (k >= SDLK_0 && k <= SDLK_9) cifra = k - SDLK_0;
                    else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) cifra = k - SDLK_KP_1 + 1;
                    else if (k == SDLK_KP_0) cifra = 0;
                    size_t n = strlen(cifras);
                    if (cifra >= 0 && n < 8) { cifras[n] = (char)('0' + cifra); cifras[n + 1] = 0; }
                    else if (k == SDLK_BACKSPACE && n) cifras[n - 1] = 0;
                    else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && n >= 4) {
                        pin_guardado = (int32_t)strtol(cifras, NULL, 10);
                        guardar_pin(pin_guardado);
                        SDL_AtomicSet(&pin_cancelado, 0);
                        estado = ESTADO_ESPERANDO;
                        cifras[0] = 0;
                        SDL_SemPost(sem_pin);
                    } else if (k == SDLK_ESCAPE) {
                        SDL_AtomicSet(&pin_cancelado, 1);
                        estado = ESTADO_ESPERANDO;
                        cifras[0] = 0;
                        SDL_SemPost(sem_pin);
                    }
                    repintar = 1;
                    break;
                }
                if (estado == ESTADO_CONECTADO) tecla(mapa[e.key.keysym.scancode], e.type == SDL_KEYDOWN);
                break;
            }
            case SDL_MOUSEMOTION:
                if (estado != ESTADO_CONECTADO) break;
                if (e.motion.xrel) enviar(EV_REL, REL_X, e.motion.xrel);
                if (e.motion.yrel) enviar(EV_REL, REL_Y, e.motion.yrel);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                static const uint16_t b[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };
                if (estado == ESTADO_CONECTADO && e.button.button < 6)
                    tecla(b[e.button.button], e.type == SDL_MOUSEBUTTONDOWN);
                break;
            }
            case SDL_MOUSEWHEEL: {
                if (estado != ESTADO_CONECTADO) break;
                float s = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
                acc_y += e.wheel.preciseY * s;
                acc_x += e.wheel.preciseX * s;
                while (acc_y >= 1) { enviar(EV_REL, REL_WHEEL, 1); acc_y -= 1; }
                while (acc_y <= -1) { enviar(EV_REL, REL_WHEEL, -1); acc_y += 1; }
                while (acc_x >= 1) { enviar(EV_REL, REL_HWHEEL, 1); acc_x -= 1; }
                while (acc_x <= -1) { enviar(EV_REL, REL_HWHEEL, -1); acc_x += 1; }
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) soltar_todo();
                repintar = 1;
                break;
            default:
                if (e.type == EV_ESTADO) {
                    estado = e.user.code;
                    memset(pulsadas, 0, sizeof pulsadas);
                    viendo = 0;
                    /* solo capturamos ratón y teclado mientras hay conexión */
                    SDL_SetRelativeMouseMode(estado == ESTADO_CONECTADO ? SDL_TRUE : SDL_FALSE);
                    SDL_SetWindowGrab(win, estado == ESTADO_CONECTADO ? SDL_TRUE : SDL_FALSE);
                    if (estado == ESTADO_CONECTADO) t_est = SDL_GetTicks();
                    repintar = 1;
                } else if (e.type == EV_FRAME) {
                    SDL_LockMutex(mtx_frame);
                    if (hay_frame) {
                        if (!tex || tw != fw || th != fh || tfmt != ffmt) {
                            if (tex) SDL_DestroyTexture(tex);
                            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
                            tw = fw; th = fh; tfmt = ffmt;
                            printf("Imagen de la Deck: %dx%d (%s)\n", fw, fh,
                                   ffmt == DL_FMT_LZ4 ? "color completo sin pérdidas" : "JPEG");
                        }
                        SDL_UpdateTexture(tex, NULL, frente.p, fw * 4);
                        hay_frame = 0;
                        if (estado == ESTADO_CONECTADO) { viendo = 1; repintar = 1; }
                    }
                    SDL_UnlockMutex(mtx_frame);
                }
            }
        } while (seguir && SDL_PollEvent(&e));
    }

    soltar_todo();
    SDL_Delay(50);
    printf("Saliendo\n");
    SDL_Quit();
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);
#endif
}
