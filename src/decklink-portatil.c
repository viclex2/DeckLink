/*
 * decklink-portatil — la pantalla de DeckLink, para Linux y Windows.
 * Muestra a pantalla completa la imagen de la Steam Deck, reproduce su sonido
 * y le manda el teclado y el ratón/touchpad.
 *
 * Busca la Deck, por orden:
 *   - Windows: canal USB directo (WinUSB, sin red; Windows instala el driver solo)
 *   - cable USB como tarjeta de red (10.77.0.1 / 10.79.0.1)
 *   - wifi (la Deck avisa con balizas UDP). Por wifi pide el PIN que muestra la Deck.
 *
 * Uso: decklink-portatil [ip_de_la_deck]
 * F1: ver los FPS.  Salir: Ctrl + Alt + Mayús + Q
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
#  include <setupapi.h>
#  include <winusb.h>
#  include <wininet.h>
#  include <shellapi.h>
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
/* Canal con la Deck: socket o, en Windows, el canal USB directo       */
/* ------------------------------------------------------------------ */
typedef struct {
    int usb;
    sock_t s;
#ifdef _WIN32
    HANDLE dev;
    WINUSB_INTERFACE_HANDLE wu;
    UCHAR ep_in, ep_out;
    uint8_t *buf;
    size_t pos, len;
#endif
} canal_t;

#ifdef _WIN32
#define USB_BUF (256 * 1024)

static int guid_de_texto(const char *t, GUID *g)
{
    unsigned a, b, c, d[8];
    if (sscanf(t, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c,
               &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return -1;
    g->Data1 = a; g->Data2 = (USHORT)b; g->Data3 = (USHORT)c;
    for (int i = 0; i < 8; i++) g->Data4[i] = (UCHAR)d[i];
    return 0;
}

static void usb_cerrar(canal_t *c)
{
    if (c->wu) WinUsb_Free(c->wu);
    if (c->dev && c->dev != INVALID_HANDLE_VALUE) CloseHandle(c->dev);
    free(c->buf);
    c->wu = NULL; c->dev = NULL; c->buf = NULL;
}

/* abre la interfaz DeckLink que Windows publica con WinUSB (si la Deck está enchufada) */
static int usb_abrir(canal_t *c)
{
    static int avisado;
    GUID g;
    if (guid_de_texto(DL_WINUSB_GUID, &g)) return -1;
    HDEVINFO info = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return -1;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    int r = -1;
    memset(c, 0, sizeof *c);
    for (DWORD i = 0; r && SetupDiEnumDeviceInterfaces(info, NULL, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = malloc(need);
        det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailW(info, &ifd, det, need, NULL, NULL)) {
            c->dev = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
            if (c->dev != INVALID_HANDLE_VALUE && WinUsb_Initialize(c->dev, &c->wu)) {
                USB_INTERFACE_DESCRIPTOR id;
                if (WinUsb_QueryInterfaceSettings(c->wu, 0, &id)) {
                    for (UCHAR k = 0; k < id.bNumEndpoints; k++) {
                        WINUSB_PIPE_INFORMATION pi;
                        if (!WinUsb_QueryPipe(c->wu, 0, k, &pi) || pi.PipeType != UsbdPipeTypeBulk) continue;
                        if (USB_ENDPOINT_DIRECTION_IN(pi.PipeId)) c->ep_in = pi.PipeId; else c->ep_out = pi.PipeId;
                    }
                }
                if (c->ep_in && c->ep_out) r = 0;
            } else if (!avisado) {
                avisado = 1;
                printf("La Deck está enchufada por USB, pero no se puede abrir el canal directo (error %lu)\n",
                       (unsigned long)GetLastError());
            }
            if (r) usb_cerrar(c);
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(info);
    if (r) return -1;

    c->usb = 1;
    c->s = SOCK_NULO;
    c->buf = malloc(USB_BUF);
    /* tirar lo que quedara de una sesión anterior */
    ULONG t = 150, got;
    WinUsb_SetPipePolicy(c->wu, c->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
    Uint32 fin = SDL_GetTicks() + 1500;
    while (SDL_GetTicks() < fin && WinUsb_ReadPipe(c->wu, c->ep_in, c->buf, USB_BUF, &got, NULL) && got) { }
    t = 3000;                                /* la Deck tiene que contestar al saludo */
    WinUsb_SetPipePolicy(c->wu, c->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
    t = 1000;
    WinUsb_SetPipePolicy(c->wu, c->ep_out, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
    return 0;
}
#endif

/* ya contestó: a partir de aquí se espera sin límite (puede haber ratos sin datos) */
static void canal_contesto(canal_t *c)
{
#ifdef _WIN32
    if (c->usb) {
        ULONG t = 0;
        WinUsb_SetPipePolicy(c->wu, c->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof t, &t);
    }
#else
    (void)c;
#endif
}

static int canal_leer(canal_t *c, void *buf, size_t n)
{
#ifdef _WIN32
    if (c->usb) {
        uint8_t *p = buf;
        while (n) {
            if (c->pos >= c->len) {
                ULONG got = 0;
                if (!WinUsb_ReadPipe(c->wu, c->ep_in, c->buf, USB_BUF, &got, NULL)) return -1;
                c->pos = 0; c->len = got;
                continue;
            }
            size_t k = c->len - c->pos < n ? c->len - c->pos : n;
            memcpy(p, c->buf + c->pos, k);
            c->pos += k; p += k; n -= k;
        }
        return 0;
    }
#endif
    return recv_todo(c->s, buf, n);
}

static void canal_escribir(canal_t *c, const void *m, size_t n)
{
#ifdef _WIN32
    if (c->usb) { ULONG k; WinUsb_WritePipe(c->wu, c->ep_out, (PUCHAR)m, (ULONG)n, &k, NULL); return; }
#endif
    send(c->s, (const char *)m, (int)n, MSG_NOSIGNAL);
}

static void canal_cerrar(canal_t *c)
{
#ifdef _WIN32
    if (c->usb) { usb_cerrar(c); return; }
#endif
    cerrar_sock(c->s);
}

/* la cabecera de un mensaje; por USB, si quedaban restos de otra sesión, busca el inicio */
static int leer_cabecera(canal_t *c, dl_frame_hdr *h)
{
    if (canal_leer(c, h, sizeof *h)) return -1;
    if (h->magic == DL_MAGIC || !c->usb) return 0;
    uint8_t *b = (uint8_t *)h;
    for (int i = 0; i < (4 << 20); i++) {
        memmove(b, b + 1, sizeof *h - 1);
        if (canal_leer(c, b + sizeof *h - 1, 1)) return -1;
        if (h->magic == DL_MAGIC) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Estado compartido                                                   */
/* ------------------------------------------------------------------ */
static const char *ip_forzada = NULL;
static canal_t *canal = NULL;
static SDL_mutex *mtx_frame, *mtx_envio;
static SDL_sem *sem_pin;
static SDL_atomic_t pin_cancelado;
static int32_t pin_guardado = -1;

typedef struct { uint8_t *p; size_t cap; } buf_t;
static buf_t frente, fondo, comprimido;
static int fw, fh, ffmt, hay_frame;
static unsigned retraso_deck;        /* ms de la captura a llegar aquí, medido por la Deck */
static Uint64 t_llegada;             /* cuándo llegó el fotograma pendiente de pintar */
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
    if (canal) canal_escribir(canal, &m, sizeof m);
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
    s = conectar(DL_IP_DECK2, 300);         /* segunda tarjeta (cable USB 2, Windows) */
    if (s != SOCK_NULO) { snprintf(ip, n, DL_IP_DECK2); *por_usb = 1; return s; }

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
        canal_t cn;
        memset(&cn, 0, sizeof cn);
#ifdef _WIN32
        if (!ip_forzada && usb_abrir(&cn) == 0) {
            snprintf(ip, sizeof ip, "canal USB directo");
            por_usb = 1;
        } else
#endif
        {
            cn.s = buscar_deck(ip, sizeof ip, &por_usb);
            if (cn.s == SOCK_NULO) continue;
        }

        SDL_LockMutex(mtx_envio); canal = &cn; SDL_UnlockMutex(mtx_envio);
        enviar(DL_HOLA, DL_CAP_LZ4, pin_guardado);
        int avisado = 0, pedir_pin = 0;

        dl_frame_hdr h;
        int primera = 1;
        while (leer_cabecera(&cn, &h) == 0) {
            if (primera) { canal_contesto(&cn); primera = 0; }
            if (h.magic == DL_MAGIC && h.fmt == DL_FMT_NADA && h.bytes <= 64u << 20) {   /* relleno del canal USB */
                reservar(&comprimido, h.bytes);
                if (h.bytes && canal_leer(&cn, comprimido.p, h.bytes)) break;
                continue;
            }
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
                if (canal_leer(&cn, comprimido.p, h.bytes)) break;
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
            if (canal_leer(&cn, comprimido.p, h.bytes)) break;
            enviar(DL_ACK, 0, h.reservado[2]);  /* la Deck ya puede preparar el siguiente (con su número) */
            Uint64 llegada = SDL_GetPerformanceCounter();
            reservar(&fondo, px * 4);
            if (decodificar_franjas(comprimido.p, h.bytes, h.w, h.h, fondo.p, h.fmt == DL_FMT_LZ4)) continue;
            SDL_LockMutex(mtx_frame);
            buf_t t = frente; frente = fondo; fondo = t;
            fw = h.w; fh = h.h; ffmt = h.fmt; hay_frame = 1;
            retraso_deck = h.reservado[0] | (unsigned)h.reservado[1] << 8;
            t_llegada = llegada;
            n_recibidos++;
            SDL_UnlockMutex(mtx_frame);
            avisar(EV_FRAME, 0);
        }

        SDL_LockMutex(mtx_envio); canal = NULL; SDL_UnlockMutex(mtx_envio);
        canal_cerrar(&cn);
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
/* Texto con la fuente de recursos.h (atlas de glifos en escala de grises) */
static SDL_Texture *tex_fuente;

static void preparar_fuente(SDL_Renderer *r)
{
    static uint32_t px[FUENTE_ANCHO * FUENTE_ALTO];
    for (int i = 0; i < FUENTE_ANCHO * FUENTE_ALTO; i++)
        px[i] = (uint32_t)fuente_alfa[i] << 24 | 0xFFFFFF;           /* blanco con alfa */
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, FUENTE_ANCHO, FUENTE_ALTO, 32, FUENTE_ANCHO * 4,
                                                        SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    tex_fuente = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (tex_fuente) {
        SDL_SetTextureBlendMode(tex_fuente, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex_fuente, SDL_ScaleModeLinear);
    }
}

/* dibuja (o solo mide, si r es NULL) texto de altura «alto» con la esquina superior en x,y */
static float texto(SDL_Renderer *r, float x, float y, float alto, const char *t, Uint8 cr, Uint8 cg, Uint8 cb)
{
    float e = alto / FUENTE_ALTO, x0 = x;
    if (r && tex_fuente) SDL_SetTextureColorMod(tex_fuente, cr, cg, cb);
    for (; *t; t++) {
        const char *p = strchr(fuente_glifos, *t);
        if (!p) { x += 16 * e; continue; }
        int g = (int)(p - fuente_glifos);
        if (r && tex_fuente) {
            SDL_Rect src = { fuente_x[g], 0, fuente_w[g], FUENTE_ALTO };
            SDL_FRect dst = { x, y, fuente_w[g] * e, alto };
            SDL_RenderCopyF(r, tex_fuente, &src, &dst);
        }
        x += fuente_w[g] * e;
    }
    return x - x0;
}

/* Datos de la tecla F1, como el indicador de rendimiento de la consola:
   recuadro pequeño arriba a la izquierda, etiqueta en gris y valor en color */
static void dibujar_fps(SDL_Renderer *r, int ancho_img, int alto_img, unsigned fps, unsigned ms)
{
    (void)ancho_img;
    float a = alto_img / 800.0f * 22, m = a * 0.55f;
    char v1[16], v2[24];
    snprintf(v1, sizeof v1, "%u", fps);
    if (ms) snprintf(v2, sizeof v2, "%u ms", ms); else snprintf(v2, sizeof v2, "-");
    float et = texto(NULL, 0, 0, a, "LAT", 0, 0, 0) + m;
    float wv = texto(NULL, 0, 0, a, v2, 0, 0, 0);
    float w1 = texto(NULL, 0, 0, a, v1, 0, 0, 0);
    if (w1 > wv) wv = w1;
    SDL_FRect caja = { m, m, et + wv + 2.5f * m, 2 * a * 1.25f + 1.2f * m };
    SDL_SetRenderDrawColor(r, 8, 10, 14, 190);
    SDL_RenderFillRectF(r, &caja);
    SDL_FRect barra = { caja.x, caja.y, a * 0.16f, caja.h };
    SDL_SetRenderDrawColor(r, 26, 159, 255, 255);
    SDL_RenderFillRectF(r, &barra);
    float x = caja.x + 1.3f * m, y = caja.y + 0.6f * m;
    texto(r, x, y, a, "FPS", 139, 146, 154);
    if (fps >= 55) texto(r, x + et, y, a, v1, 120, 220, 110);
    else if (fps >= 30) texto(r, x + et, y, a, v1, 243, 190, 70);
    else texto(r, x + et, y, a, v1, 232, 90, 80);
    y += a * 1.25f;
    texto(r, x, y, a, "LAT", 139, 146, 154);
    if (!ms) texto(r, x + et, y, a, v2, 139, 146, 154);
    else if (ms <= 25) texto(r, x + et, y, a, v2, 120, 220, 110);
    else if (ms <= 45) texto(r, x + et, y, a, v2, 243, 190, 70);
    else texto(r, x + et, y, a, v2, 232, 90, 80);
}

/* casillas del PIN, sobre la tarjeta de la pantalla del PIN */
static void dibujar_pin(SDL_Renderer *r, const char *cifras)
{
    int n = (int)strlen(cifras);
    int casillas = n < 4 ? 4 : (n < 8 ? n + 1 : 8);
    const int w = 72, h = 96, sep = 18;
    int x0 = (1280 - (casillas * w + (casillas - 1) * sep)) / 2, y = 370;
    for (int i = 0; i < casillas; i++) {
        SDL_Rect c = { x0 + i * (w + sep), y, w, h };
        SDL_SetRenderDrawColor(r, 14, 18, 24, 255);
        SDL_RenderFillRect(r, &c);
        if (i == n) {
            SDL_SetRenderDrawColor(r, 26, 159, 255, 255);
            SDL_Rect c2 = { c.x - 1, c.y - 1, c.w + 2, c.h + 2 };
            SDL_RenderDrawRect(r, &c2);
        } else SDL_SetRenderDrawColor(r, 47, 55, 67, 255);
        SDL_RenderDrawRect(r, &c);
        if (i < n) {
            char d[2] = { cifras[i], 0 };
            float wd = texto(NULL, 0, 0, 64, d, 0, 0, 0);
            texto(r, c.x + (w - wd) / 2, c.y + (h - 64) / 2.0f, 64, d, 235, 239, 244);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Actualizaciones: busca en GitHub una versión más nueva              */
/* ------------------------------------------------------------------ */
static Uint32 EV_ACTUALIZAR;          /* code 0: hay versión nueva; 1: listo para reiniciar; 2: error */
static char act_etiqueta[80];

/* descarga una URL a memoria (NUL al final) */
static int descargar(const char *url, char **out, size_t *n)
{
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap + 1);
    if (!b) return -1;
#ifdef _WIN32
    HINTERNET h = InternetOpenA("DeckLink/" DL_VERSION, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!h) { free(b); return -1; }
    DWORD seg = 20000;
    InternetSetOptionA(h, INTERNET_OPTION_CONNECT_TIMEOUT, &seg, sizeof seg);
    InternetSetOptionA(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &seg, sizeof seg);
    HINTERNET u = InternetOpenUrlA(h, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    int ok = 0;
    if (u) {
        DWORD codigo = 0, l = sizeof codigo;
        HttpQueryInfoA(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &codigo, &l, NULL);
        ok = codigo == 200;
        DWORD r;
        while (ok) {
            if (len == cap) { cap *= 2; char *nb = realloc(b, cap + 1); if (!nb) { ok = 0; break; } b = nb; }
            if (!InternetReadFile(u, b + len, (DWORD)(cap - len), &r)) { ok = 0; break; }
            if (!r) break;
            len += r;
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    if (!ok) { free(b); return -1; }
#else
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "curl -fsSL --max-time 60 -H 'Accept: application/vnd.github+json' '%s' 2>/dev/null"
             " || wget -q -T 60 -O - '%s' 2>/dev/null", url, url);
    FILE *f = popen(cmd, "r");
    if (!f) { free(b); return -1; }
    size_t r;
    for (;;) {
        if (len == cap) { cap *= 2; char *nb = realloc(b, cap + 1); if (!nb) break; b = nb; }
        r = fread(b + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    if (pclose(f) != 0 || !len) { free(b); return -1; }
#endif
    b[len] = 0;
    *out = b;
    *n = len;
    return 0;
}

/* "v2.1-pruebas" o "DeckLink-V.2" -> {2,1,0} */
static void version_de(const char *t, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    while (*t && !(*t >= '0' && *t <= '9')) t++;
    for (int i = 0; i < 3 && *t >= '0' && *t <= '9'; i++) {
        while (*t >= '0' && *t <= '9') v[i] = v[i] * 10 + (*t++ - '0');
        if (*t != '.') break;
        t++;
    }
}

static int comparar_version(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* valor booleano tras "clave": */
static int json_si(const char *p)
{
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return *p == 't';
}

static int hilo_buscar_version(void *arg)
{
    (void)arg;
    char *json;
    size_t n;
    if (descargar("https://api.github.com/repos/" DL_REPO "/releases?per_page=20", &json, &n)) {
        printf("No he podido mirar si hay versiones nuevas (¿sin internet?)\n");
        return 0;
    }
    int actual[3], mejor[3] = { 0, 0, 0 };
    version_de(DL_VERSION, actual);
    char elegida[80] = "";
    for (const char *p = json; (p = strstr(p, "\"tag_name\"")) != NULL;) {
        p = strchr(p + 10, '"');
        if (!p) break;
        const char *ini = p + 1, *fin = strchr(ini, '"');
        if (!fin || fin - ini >= (int)sizeof elegida) break;
        char tag[80];
        memcpy(tag, ini, (size_t)(fin - ini));
        tag[fin - ini] = 0;
        int es_pruebas = json_si(strstr(fin, "\"prerelease\""));
        int es_borrador = json_si(strstr(fin, "\"draft\""));
        p = fin;
        if (es_borrador || (es_pruebas && !DL_PRUEBAS)) continue;
        int v[3];
        version_de(tag, v);
        int c = comparar_version(v, actual);
        /* más nueva; o la misma ya definitiva si esta es de pruebas */
        if (c > 0 || (c == 0 && DL_PRUEBAS && !es_pruebas)) {
            if (!elegida[0] || comparar_version(v, mejor) > 0) {
                memcpy(mejor, v, sizeof mejor);
                snprintf(elegida, sizeof elegida, "%s", tag);
            }
        }
    }
    free(json);
    if (!elegida[0]) { printf("DeckLink %s está al día\n", DL_VERSION); return 0; }

    char ruta[700], omitida[80] = "";
    ruta_conf(ruta, sizeof ruta, "omitir-version");
    FILE *f = fopen(ruta, "r");
    if (f) { if (!fgets(omitida, sizeof omitida, f)) omitida[0] = 0; fclose(f); }
    omitida[strcspn(omitida, "\r\n")] = 0;
    if (!strcmp(omitida, elegida)) { printf("Hay una versión nueva (%s), pero dijiste que no\n", elegida); return 0; }
    printf("Hay una versión nueva de DeckLink: %s\n", elegida);
    snprintf(act_etiqueta, sizeof act_etiqueta, "%s", elegida);
    avisar(EV_ACTUALIZAR, 0);
    return 0;
}

static void omitir_version(void)
{
    char ruta[700];
    ruta_conf(ruta, sizeof ruta, "omitir-version");
    FILE *f = fopen(ruta, "w");
    if (f) { fprintf(f, "%s\n", act_etiqueta); fclose(f); }
}

static char act_nuevo[1024];         /* Linux: programa nuevo ya colocado; Windows: instalador descargado */

static int hilo_actualizar(void *arg)
{
    (void)arg;
    char url[400];
#ifdef _WIN32
    snprintf(url, sizeof url, "https://github.com/" DL_REPO "/releases/download/%s/DeckLink-Windows-Instalador.exe",
             act_etiqueta);
    char *d; size_t n;
    if (descargar(url, &d, &n) || n < 100000 || d[0] != 'M' || d[1] != 'Z') { avisar(EV_ACTUALIZAR, 2); return 0; }
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(act_nuevo, sizeof act_nuevo, "%sDeckLink-Actualizacion.exe", tmp);
    FILE *f = fopen(act_nuevo, "wb");
    int ok = f && fwrite(d, 1, n, f) == n;
    if (f) fclose(f);
    free(d);
    avisar(EV_ACTUALIZAR, ok ? 1 : 2);
#else
    snprintf(url, sizeof url, "https://github.com/" DL_REPO "/releases/download/%s/DeckLink-Linux.zip", act_etiqueta);
    char *d; size_t n;
    if (descargar(url, &d, &n) || n < 100000 || d[0] != 'P' || d[1] != 'K') { avisar(EV_ACTUALIZAR, 2); return 0; }
    char dir[] = "/tmp/decklink-actualizacion-XXXXXX";
    int ok = 0;
    if (mkdtemp(dir)) {
        char zip[600], cmd[6144], exe[1024];
        snprintf(zip, sizeof zip, "%s/DeckLink-Linux.zip", dir);
        FILE *f = fopen(zip, "wb");
        int esc = f && fwrite(d, 1, n, f) == n;
        if (f) fclose(f);
        ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (esc && l > 0) {
            exe[l] = 0;
            snprintf(cmd, sizeof cmd,
                     "cd '%s' && { unzip -oq DeckLink-Linux.zip || bsdtar -xf DeckLink-Linux.zip || "
                     "python3 -c 'import zipfile;zipfile.ZipFile(\"DeckLink-Linux.zip\").extractall()'; } >/dev/null 2>&1 && "
                     "test -s DeckLink-Linux/archivos/decklink-portatil && "
                     "cp -f DeckLink-Linux/archivos/decklink-portatil '%s.nuevo' && chmod 755 '%s.nuevo' && "
                     "mv -f '%s.nuevo' '%s'", dir, exe, exe, exe, exe);
            ok = system(cmd) == 0;
            snprintf(act_nuevo, sizeof act_nuevo, "%s", exe);
        }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd)) { }
    }
    free(d);
    avisar(EV_ACTUALIZAR, ok ? 1 : 2);
#endif
    return 0;
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

    preparar_fuente(ren);
    SDL_Texture *tex_espera = textura_jpeg(ren, espera_jpg, sizeof espera_jpg);
    SDL_Texture *tex_pin = textura_jpeg(ren, pin_jpg, sizeof pin_jpg);
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0, tfmt = -1;
    int estado = ESTADO_ESPERANDO, viendo = 0;
    char cifras[9] = "";
    mtx_frame = SDL_CreateMutex();
    mtx_envio = SDL_CreateMutex();
    sem_pin = SDL_CreateSemaphore(0);
    EV_FRAME = SDL_RegisterEvents(3);
    EV_ESTADO = EV_FRAME + 1;
    EV_ACTUALIZAR = EV_FRAME + 2;
    SDL_Texture *tex_act = textura_jpeg(ren, actualizar_jpg, sizeof actualizar_jpg);
    SDL_Texture *tex_bajando = textura_jpeg(ren, actualizando_jpg, sizeof actualizando_jpg);
    SDL_Texture *tex_act_error = textura_jpeg(ren, act_error_jpg, sizeof act_error_jpg);
    enum { ACT_NO = 0, ACT_PREGUNTA, ACT_BAJANDO, ACT_ERROR };
    int act = ACT_NO, act_pendiente = 0;
    Uint32 t_inicio = SDL_GetTicks();
    int ver_fps = 0;
    unsigned fps_valor = 0, fps_cuenta = 0, ms_valor = 0;
    double ms_local = 0;          /* de llegar el fotograma a verse en pantalla */
    Uint64 llegada_pintada = 0;
    Uint32 t_fps = SDL_GetTicks();

    printf("DeckLink — buscando la Steam Deck (cable USB o wifi)\n");
    SDL_DetachThread(SDL_CreateThread(hilo_red, "red", NULL));
    printf("DeckLink %s%s\n", DL_VERSION, DL_PRUEBAS ? " (pruebas)" : "");
    SDL_DetachThread(SDL_CreateThread(hilo_buscar_version, "version", NULL));

    float acc_y = 0, acc_x = 0;
    unsigned long n_pintados = 0, rec0 = 0;
    Uint32 t_est = SDL_GetTicks();
    SDL_Event e;
    int seguir = 1, repintar = 1;
    while (seguir) {
        if (repintar) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (act != ACT_NO) {
                SDL_RenderSetLogicalSize(ren, 1280, 800);
                SDL_Texture *t = act == ACT_PREGUNTA ? tex_act : act == ACT_BAJANDO ? tex_bajando : tex_act_error;
                if (t) SDL_RenderCopy(ren, t, NULL, NULL);
                if (act == ACT_PREGUNTA) {
                    /* el número de la versión nueva, en grande */
                    int v[3];
                    char txt[40];
                    version_de(act_etiqueta, v);
                    if (v[2]) snprintf(txt, sizeof txt, "%d.%d.%d", v[0], v[1], v[2]);
                    else snprintf(txt, sizeof txt, "%d.%d", v[0], v[1]);
                    char vtxt[48];
                    snprintf(vtxt, sizeof vtxt, "%s", txt);
                    float w = texto(NULL, 0, 0, 120, vtxt, 0, 0, 0);
                    texto(ren, (1280 - w) / 2, 300, 120, vtxt, 26, 159, 255);
                    char tengo[48];
                    snprintf(tengo, sizeof tengo, "tienes la %s", DL_VERSION);
                    w = texto(NULL, 0, 0, 26, tengo, 0, 0, 0);
                    texto(ren, (1280 - w) / 2, 440, 26, tengo, 139, 146, 154);
                }
            } else if (estado == ESTADO_PIDE_PIN) {
                SDL_RenderSetLogicalSize(ren, 1280, 800);
                if (tex_pin) SDL_RenderCopy(ren, tex_pin, NULL, NULL);
                dibujar_pin(ren, cifras);
            } else if (viendo && tex) {
                SDL_RenderSetLogicalSize(ren, tw, th);
                SDL_RenderCopy(ren, tex, NULL, NULL);
                if (ver_fps) dibujar_fps(ren, tw, th, fps_valor, ms_valor);
            } else if (tex_espera) {
                SDL_RenderSetLogicalSize(ren, 1280, 800);
                SDL_RenderCopy(ren, tex_espera, NULL, NULL);
            }
            SDL_RenderPresent(ren);
            if (llegada_pintada) {
                double ms = (double)(SDL_GetPerformanceCounter() - llegada_pintada) * 1000.0 / SDL_GetPerformanceFrequency();
                ms_local = ms_local ? ms_local * 0.9 + ms * 0.1 : ms;
                llegada_pintada = 0;
            }
            repintar = 0;
            if (viendo) n_pintados++;
        }
        /* FPS: fotogramas nuevos mostrados en el último segundo */
        if (SDL_GetTicks() - t_fps >= 1000) {
            Uint32 dt = SDL_GetTicks() - t_fps;
            unsigned nuevo = (unsigned)((fps_cuenta * 1000u + dt / 2) / dt);
            if (ver_fps && nuevo != fps_valor && viendo) repintar = 1;
            fps_valor = nuevo;
            unsigned ms_nuevo = retraso_deck ? (unsigned)(retraso_deck + ms_local + 0.5) : 0;
            if (ver_fps && ms_nuevo != ms_valor && viendo) repintar = 1;
            ms_valor = ms_nuevo;
            fps_cuenta = 0;
            t_fps = SDL_GetTicks();
            if (repintar) continue;
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
                if (e.key.keysym.scancode == SDL_SCANCODE_F1) {      /* F1: ver/ocultar los FPS */
                    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                        ver_fps = !ver_fps;
                        printf("FPS en pantalla: %s\n", ver_fps ? "sí" : "no");
                        repintar = 1;
                    }
                    break;
                }
                if (act != ACT_NO) {                                 /* pantalla de actualización */
                    if (e.type != SDL_KEYDOWN || e.key.repeat) break;
                    SDL_Keycode k = e.key.keysym.sym;
                    if (act == ACT_PREGUNTA && (k == SDLK_s || k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
                        printf("Actualizando a %s...\n", act_etiqueta);
                        act = ACT_BAJANDO;
                        SDL_DetachThread(SDL_CreateThread(hilo_actualizar, "actualizar", NULL));
                    } else if (act == ACT_PREGUNTA && (k == SDLK_n || k == SDLK_ESCAPE)) {
                        printf("No se actualiza a %s (no se volverá a preguntar por esta versión)\n", act_etiqueta);
                        omitir_version();
                        act = ACT_NO;
                    } else if (act == ACT_ERROR) {
                        act = ACT_NO;
                    }
                    repintar = 1;
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
                if (e.type == EV_ACTUALIZAR) {
                    if (e.user.code == 0) {
                        /* preguntar al abrir o cuando no se está jugando; si no, al desconectar */
                        if (estado != ESTADO_CONECTADO || SDL_GetTicks() - t_inicio < 20000) act = ACT_PREGUNTA;
                        else act_pendiente = 1;
                    } else if (e.user.code == 1) {
                        soltar_todo();
                        enviar(DL_ADIOS, 0, 0);
#ifdef _WIN32
                        /* el instalador cierra DeckLink, instala sin preguntar y lo vuelve a abrir */
                        INT_PTR r = (INT_PTR)ShellExecuteA(NULL, "open", act_nuevo, "/S", NULL, SW_SHOWNORMAL);
                        if (r > 32) { printf("Instalador lanzado\n"); seguir = 0; }
                        else { printf("No se pudo abrir el instalador (%d)\n", (int)r); act = ACT_ERROR; }
#else
                        printf("Actualizado: reiniciando DeckLink\n");
                        SDL_Quit();
                        execv(act_nuevo, argv);
                        printf("No se pudo reiniciar: %s\n", strerror(errno));
                        _exit(0);
#endif
                    } else {
                        printf("No se pudo descargar la versión nueva\n");
                        act = ACT_ERROR;
                    }
                    repintar = 1;
                } else if (e.type == EV_ESTADO) {
                    estado = e.user.code;
                    if (estado != ESTADO_CONECTADO && act_pendiente) { act_pendiente = 0; act = ACT_PREGUNTA; }
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
                        fps_cuenta++;
                        llegada_pintada = t_llegada;
                        if (estado == ESTADO_CONECTADO) { viendo = 1; repintar = 1; }
                    }
                    SDL_UnlockMutex(mtx_frame);
                }
            }
        } while (seguir && SDL_PollEvent(&e));
    }

    soltar_todo();
    enviar(DL_ADIOS, 0, 0);
    SDL_Delay(50);
    printf("Saliendo\n");
    SDL_Quit();
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);
#endif
}
