/*
 * decklink-portatil v3 — programa del portátil.
 * Muestra a pantalla completa la imagen de la Steam Deck que llega por el cable USB
 * y le manda el teclado y el touchpad/ratón del portátil.
 * Mientras no hay conexión muestra una pantalla de espera y reintenta solo.
 *
 * Salir: Ctrl + Alt + Mayús + Q
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <SDL2/SDL.h>
#include <turbojpeg.h>
#include <lz4.h>
#include "protocolo.h"
#include "teclas.h"
#include "recursos.h"

static const char *ip_deck = DL_IP_DECK;
static int sock = -1;
static SDL_mutex *mtx_frame, *mtx_envio;

/* fotogramas: "fondo" lo rellena la red, "frente" lo pinta la pantalla */
typedef struct { uint8_t *p; size_t cap; } buf_t;
static buf_t frente, fondo, comprimido;
static int fw, fh, ffmt, hay_frame;
static Uint32 EV_FRAME, EV_ESTADO;
static SDL_AudioDeviceID audio_dev;
static uint8_t pulsadas[KEY_CNT];
static volatile unsigned long n_recibidos;

static void reservar(buf_t *b, size_t n)
{
    if (b->cap >= n) return;
    free(b->p);
    b->p = malloc(n);
    b->cap = n;
}

static int recv_todo(int s, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(s, p, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}

static void enviar(uint16_t type, uint16_t code, int32_t value)
{
    dl_input m = { type, code, value };
    SDL_LockMutex(mtx_envio);
    if (sock >= 0) send(sock, &m, sizeof m, MSG_NOSIGNAL);
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

/* ---------------- descompresión JPEG por franjas en paralelo ---------------- */
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

static void *descomprimir_franja(void *a)
{
    franja_t *f = a;
    size_t n = (size_t)f->w * f->h;
    if (!f->tam) { memset(f->dst, 0, n * 4); return NULL; }
    if (!f->lz4) {
        f->err = tjDecompress2(f->tj, f->src, f->tam, f->dst, f->w, f->w * 4, f->h,
                               TJPF_BGRX, TJFLAG_FASTDCT);
        return NULL;
    }
    if (f->bgr_cap < n * 3) { free(f->bgr); f->bgr = malloc(n * 3); f->bgr_cap = n * 3; }
    int r = LZ4_decompress_safe((const char *)f->src, (char *)f->bgr, (int)f->tam, (int)(n * 3));
    if (r != (int)(n * 3)) { f->err = 1; memset(f->dst, 0, n * 4); return NULL; }
    const uint8_t *s = f->bgr;
    uint32_t *o = (uint32_t *)f->dst;
    for (size_t i = 0; i < n; i++, s += 3)
        o[i] = 0xFF000000u | ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
    return NULL;
}

static int decodificar_franjas(const uint8_t *datos, uint32_t bytes, int w, int h, uint8_t *dst, int lz4)
{
    if (bytes < 1) return -1;
    int n = datos[0];
    if (n < 1 || n > MAX_FRANJAS || bytes < 1u + 4u * n) return -1;
    const uint8_t *p = datos + 1 + 4 * n;
    uint32_t resto = bytes - 1 - 4 * n;
    pthread_t th[MAX_FRANJAS];
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
    for (int i = 1; i < n; i++) pthread_create(&th[i], NULL, descomprimir_franja, &franjas[i]);
    descomprimir_franja(&franjas[0]);
    for (int i = 1; i < n; i++) pthread_join(th[i], NULL);
    return 0;
}

/* ---------------- red ---------------- */
static int hilo_red(void *arg)
{
    (void)arg;
    for (;;) {
        int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DL_PUERTO) };
        inet_pton(AF_INET, ip_deck, &a.sin_addr);
        if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
            close(s);
            SDL_Delay(700);
            continue;
        }
        int uno = 1, rb = 8 << 20;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
        SDL_LockMutex(mtx_envio); sock = s; SDL_UnlockMutex(mtx_envio);
        printf("Conectado a la Steam Deck\n");
        avisar(EV_ESTADO, 1);

        dl_frame_hdr h;
        while (recv_todo(s, &h, sizeof h) == 0) {
            if (h.magic != DL_MAGIC || h.bytes > 64u << 20 ||
                (h.fmt != DL_FMT_AUDIO && (!h.w || !h.h))) {
                fprintf(stderr, "La Deck tiene otra versión de DeckLink. Instala la misma versión en los dos.\n");
                break;
            }
            if (h.fmt == DL_FMT_AUDIO) {
                reservar(&comprimido, h.bytes);
                if (recv_todo(s, comprimido.p, h.bytes)) break;
                if (audio_dev) {
                    /* poco retraso: si se acumula más de 80 ms, empezar de nuevo */
                    if (SDL_GetQueuedAudioSize(audio_dev) > DL_AUDIO_HZ * 4 * 80 / 1000)
                        SDL_ClearQueuedAudio(audio_dev);
                    SDL_QueueAudio(audio_dev, comprimido.p, h.bytes);
                }
                continue;
            }
            size_t px = (size_t)h.w * h.h;
            if (h.fmt == DL_FMT_RGB565) {
                if (h.bytes != px * 2) break;
                reservar(&fondo, px * 2);
                if (recv_todo(s, fondo.p, h.bytes)) break;
                enviar(DL_ACK, 0, 0);
            } else if (h.fmt == DL_FMT_JPEG || h.fmt == DL_FMT_LZ4) {
                reservar(&comprimido, h.bytes);
                if (recv_todo(s, comprimido.p, h.bytes)) break;
                enviar(DL_ACK, 0, 0);        /* la Deck ya puede preparar el siguiente */
                reservar(&fondo, px * 4);
                if (decodificar_franjas(comprimido.p, h.bytes, h.w, h.h, fondo.p, h.fmt == DL_FMT_LZ4)) continue;
            } else {
                break;
            }
            SDL_LockMutex(mtx_frame);
            buf_t t = frente; frente = fondo; fondo = t;
            fw = h.w; fh = h.h; ffmt = h.fmt; hay_frame = 1;
            n_recibidos++;
            SDL_UnlockMutex(mtx_frame);
            avisar(EV_FRAME, 0);
        }

        SDL_LockMutex(mtx_envio); sock = -1; SDL_UnlockMutex(mtx_envio);
        close(s);
        printf("Desconectado de la Steam Deck\n");
        avisar(EV_ESTADO, 0);
        SDL_Delay(500);
    }
    return 0;
}

static void preparar_log(void)
{
    if (isatty(1)) return;
    const char *home = getenv("HOME");
    if (!home) return;
    char ruta[512];
    snprintf(ruta, sizeof ruta, "%s/.cache", home);
    mkdir(ruta, 0700);
    snprintf(ruta, sizeof ruta, "%s/.cache/decklink-portatil.log", home);
    if (freopen(ruta, "w", stdout)) dup2(1, 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
}

static int una_sola_vez(void)
{
    char ruta[256];
    snprintf(ruta, sizeof ruta, "/tmp/decklink-portatil-%u.lock", (unsigned)getuid());
    int fd = open(ruta, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) ip_deck = argv[i];
    preparar_log();
    if (!una_sola_vez()) { printf("DeckLink ya está abierto\n"); return 0; }

    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "decklink", 0);
    setenv("SDL_VIDEO_X11_WMCLASS", "decklink", 0);
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "DeckLink");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Sonido de la Steam Deck");
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

    /* pantalla de espera */
    SDL_Texture *tex_espera = NULL;
    {
        tjhandle tj = tjInitDecompress();
        int w, h, ss, cs;
        if (tjDecompressHeader3(tj, espera_jpg, sizeof espera_jpg, &w, &h, &ss, &cs) == 0) {
            uint8_t *px = malloc((size_t)w * h * 4);
            tjDecompress2(tj, espera_jpg, sizeof espera_jpg, px, w, w * 4, h, TJPF_BGRX, 0);
            tex_espera = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
            SDL_UpdateTexture(tex_espera, NULL, px, w * 4);
            free(px);
        }
        tjDestroy(tj);
    }

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0, tfmt = -1;
    int conectado = 0, viendo = 0;
    mtx_frame = SDL_CreateMutex();
    mtx_envio = SDL_CreateMutex();
    EV_FRAME = SDL_RegisterEvents(2);
    EV_ESTADO = EV_FRAME + 1;

    printf("DeckLink (portátil) — esperando a la Steam Deck en %s:%d\n", ip_deck, DL_PUERTO);
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
            if (viendo && tex) {
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
        if (conectado && SDL_GetTicks() - t_est >= 30000) {
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
                if (e.key.repeat) break;
                SDL_Keymod m = SDL_GetModState();
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_Q &&
                    (m & KMOD_CTRL) && (m & KMOD_ALT) && (m & KMOD_SHIFT)) {
                    seguir = 0;
                    break;
                }
                if (conectado) tecla(mapa[e.key.keysym.scancode], e.type == SDL_KEYDOWN);
                break;
            }
            case SDL_MOUSEMOTION:
                if (!conectado) break;
                if (e.motion.xrel) enviar(EV_REL, REL_X, e.motion.xrel);
                if (e.motion.yrel) enviar(EV_REL, REL_Y, e.motion.yrel);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                static const uint16_t b[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };
                if (conectado && e.button.button < 6) tecla(b[e.button.button], e.type == SDL_MOUSEBUTTONDOWN);
                break;
            }
            case SDL_MOUSEWHEEL: {
                if (!conectado) break;
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
                    conectado = e.user.code;
                    memset(pulsadas, 0, sizeof pulsadas);
                    viendo = 0;
                    /* solo capturamos ratón y teclado mientras hay conexión */
                    SDL_SetRelativeMouseMode(conectado ? SDL_TRUE : SDL_FALSE);
                    SDL_SetWindowGrab(win, conectado ? SDL_TRUE : SDL_FALSE);
                    repintar = 1;
                } else if (e.type == EV_FRAME) {
                    SDL_LockMutex(mtx_frame);
                    if (hay_frame) {
                        int fmt_sdl = ffmt == DL_FMT_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB8888;
                        if (!tex || tw != fw || th != fh || tfmt != ffmt) {
                            if (tex) SDL_DestroyTexture(tex);
                            tex = SDL_CreateTexture(ren, fmt_sdl, SDL_TEXTUREACCESS_STREAMING, fw, fh);
                            tw = fw; th = fh; tfmt = ffmt;
                            printf("Imagen de la Deck: %dx%d (%s)\n", fw, fh,
                                   ffmt == DL_FMT_LZ4 ? "USB 3, color completo sin pérdidas" :
                                   ffmt == DL_FMT_JPEG ? "USB 2, JPEG" : "16 bits");
                        }
                        SDL_UpdateTexture(tex, NULL, frente.p, fw * (ffmt == DL_FMT_RGB565 ? 2 : 4));
                        hay_frame = 0;
                        viendo = 1;
                        repintar = 1;
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
    _exit(0);
}
