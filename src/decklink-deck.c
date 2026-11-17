/*
 * decklink-deck v3 — programa de la Steam Deck.
 *
 *   decklink-deck --red      (root, servicio del sistema) crea la "tarjeta de red USB"
 *                            y reparte IP al portátil por DHCP.
 *   decklink-deck            (usuario, servicio de usuario) espera al portátil; mientras
 *                            está conectado captura la pantalla (modo juego o escritorio,
 *                            cambia solo) y recibe teclado/ratón.
 *   decklink-deck --olvidar  borra el permiso de pantalla guardado.
 *
 * Por USB 3 manda la imagen sin comprimir (RGB565). Por USB 2 la manda en JPEG.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/uinput.h>
#include <systemd/sd-bus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/utils/dict.h>
#include <turbojpeg.h>
#include <lz4.h>
#include <spa/param/audio/format-utils.h>
#include "protocolo.h"

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ================================================================== */
/* Utilidades                                                          */
/* ================================================================== */
static int escribir(const char *ruta, const char *val)
{
    int fd = open(ruta, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = write(fd, val, strlen(val));
    close(fd);
    return r < 0 ? -1 : 0;
}

static int leer(const char *ruta, char *buf, size_t n)
{
    int fd = open(ruta, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int primer_udc(char *nombre, size_t n)
{
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    struct dirent *e;
    int ok = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(nombre, n, "%s", e->d_name);
        ok = 0;
        break;
    }
    closedir(d);
    return ok;
}

static int proceso_existe(const char *comm)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    char ruta[300], c[64];
    int si = 0;
    while (!si && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(ruta, sizeof ruta, "/proc/%s/comm", e->d_name);
        if (leer(ruta, c, sizeof c) == 0 && !strcmp(c, comm)) si = 1;
    }
    closedir(d);
    return si;
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

static int send_todo(int s, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(s, p, n, MSG_NOSIGNAL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}

/* ================================================================== */
/* MODO RED (root): gadget USB + servidor DHCP                         */
/* ================================================================== */
#define GADGET "/sys/kernel/config/usb_gadget/decklink"

static void gadget_quitar(void)
{
    if (access(GADGET, F_OK) != 0) return;
    escribir(GADGET "/UDC", "\n");
    unlink(GADGET "/configs/c.1/ncm.usb0");
    unlink(GADGET "/configs/c.1/ecm.usb0");
    rmdir(GADGET "/configs/c.1/strings/0x409");
    rmdir(GADGET "/configs/c.1");
    rmdir(GADGET "/functions/ncm.usb0");
    rmdir(GADGET "/functions/ecm.usb0");
    rmdir(GADGET "/strings/0x409");
    rmdir(GADGET);
}

static volatile sig_atomic_t salir_red = 0;
static void al_terminar(int s) { (void)s; salir_red = 1; }

static int gadget_crear(char *ifname, size_t n, const char *func)
{
    char fdir[256], enlace[256], ruta[300];
    snprintf(fdir, sizeof fdir, GADGET "/functions/%s.usb0", func);
    snprintf(enlace, sizeof enlace, GADGET "/configs/c.1/%s.usb0", func);
    if (system("modprobe libcomposite 2>/dev/null")) { /* puede estar ya integrado */ }
    char udc[128];
    for (int i = 0; i < 50 && primer_udc(udc, sizeof udc) != 0; i++) {
        if (i == 0) escribir("/sys/bus/pci/drivers/dwc3-pci/bind", "0000:04:00.3");
        usleep(100000);
    }
    if (primer_udc(udc, sizeof udc) != 0) {
        LOG("No hay USB en modo dispositivo. Activa 'USB Dual-Role Device = DRD' en la BIOS.");
        return -1;
    }
    gadget_quitar();
    if (mkdir(GADGET, 0755) && errno != EEXIST) { LOG("No puedo crear el gadget (¿configfs?)"); return -1; }
    escribir(GADGET "/idVendor", "0x1d6b");
    escribir(GADGET "/idProduct", "0x0104");
    escribir(GADGET "/bcdDevice", "0x0300");
    escribir(GADGET "/bcdUSB", "0x0300");
    mkdir(GADGET "/strings/0x409", 0755);
    escribir(GADGET "/strings/0x409/manufacturer", "DeckLink");
    escribir(GADGET "/strings/0x409/product", "Steam Deck DeckLink");
    escribir(GADGET "/strings/0x409/serialnumber", "decklink0001");
    mkdir(fdir, 0755);
    snprintf(ruta, sizeof ruta, "%s/dev_addr", fdir);  escribir(ruta, "02:de:c1:00:00:01");
    snprintf(ruta, sizeof ruta, "%s/host_addr", fdir); escribir(ruta, "02:de:c1:00:00:02");
    mkdir(GADGET "/configs/c.1", 0755);
    mkdir(GADGET "/configs/c.1/strings/0x409", 0755);
    escribir(GADGET "/configs/c.1/strings/0x409/configuration", "DeckLink");
    escribir(GADGET "/configs/c.1/MaxPower", "100");
    if (symlink(fdir, enlace) && errno != EEXIST) {
        LOG("No puedo enlazar la función de red");
        return -1;
    }
    if (escribir(GADGET "/UDC", udc)) { LOG("No puedo activar el gadget en %s", udc); return -1; }
    snprintf(ruta, sizeof ruta, "%s/ifname", fdir);
    if (leer(ruta, ifname, n)) snprintf(ifname, n, "usb0");

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "nmcli device set %s managed no >/dev/null 2>&1; "
             "ip addr flush dev %s; ip addr add " DL_IP_DECK "/24 dev %s && ip link set %s up",
             ifname, ifname, ifname, ifname);
    if (system(cmd)) { LOG("No puedo configurar la IP de %s", ifname); return -1; }
    LOG("Red USB lista: %s = " DL_IP_DECK " (tipo %s, controlador %s)", ifname, func, udc);
    return 0;
}

#pragma pack(push, 1)
typedef struct {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16], sname[64], file[128];
    uint32_t magic;
    uint8_t opts[312];
} dhcp_t;
#pragma pack(pop)

/* "ecm" para USB 2 (más fiable), "ncm" para USB 3 (más rápida); NULL si aún no se sabe */
static const char *funcion_para_velocidad(void)
{
    char v[64];
    if (leer("/etc/decklink/usb", v, sizeof v) == 0 && (!strcmp(v, "ecm") || !strcmp(v, "ncm")))
        return !strcmp(v, "ecm") ? "ecm" : "ncm";
    char udc[128], ruta[300];
    if (primer_udc(udc, sizeof udc)) return NULL;
    snprintf(ruta, sizeof ruta, "/sys/class/udc/%s/current_speed", udc);
    if (leer(ruta, v, sizeof v)) return NULL;
    if (!strncmp(v, "super-speed", 11)) return "ncm";
    if (!strcmp(v, "high-speed") || !strcmp(v, "full-speed")) return "ecm";
    return NULL;
}

static const char *funcion_actual = "ncm";

static int servidor_dhcp(const char *ifname)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int uno = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &uno, sizeof uno);
    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0) {
        LOG("DHCP: no puedo usar %s: %s", ifname, strerror(errno));
        return -1;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { LOG("DHCP: puerto 67 ocupado: %s", strerror(errno)); return -1; }
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint32_t ip_srv = inet_addr(DL_IP_DECK), ip_pc = inet_addr(DL_IP_PC);
    LOG("DHCP: repartiendo " DL_IP_PC " al portátil");
    int vigilar = 0;
    while (!salir_red) {
        if (++vigilar >= 2) {
            vigilar = 0;
            const char *f = funcion_para_velocidad();
            if (f && strcmp(f, funcion_actual)) {
                LOG("Cable a %s: cambiando la tarjeta de red USB a %s", !strcmp(f, "ecm") ? "USB 2" : "USB 3", f);
                funcion_actual = f;
                close(s);
                return 1;
            }
        }
        dhcp_t q;
        ssize_t r = recv(s, &q, sizeof q, 0);
        if (r < 240 || q.op != 1 || q.magic != htonl(0x63825363)) continue;
        int tipo = 0;
        uint32_t pedida = 0;
        for (size_t i = 0; i + 1 < (size_t)r - 240 && q.opts[i] != 255;) {
            uint8_t o = q.opts[i];
            if (o == 0) { i++; continue; }
            uint8_t l = q.opts[i + 1];
            if (o == 53 && l >= 1) tipo = q.opts[i + 2];
            if (o == 50 && l == 4) memcpy(&pedida, &q.opts[i + 2], 4);
            i += 2 + l;
        }
        int resp;
        if (tipo == 1) resp = 2;                                   /* DISCOVER -> OFFER */
        else if (tipo == 3) resp = (pedida && pedida != ip_pc) ? 6 : 5;   /* REQUEST -> ACK/NAK */
        else continue;

        dhcp_t p;
        memset(&p, 0, sizeof p);
        p.op = 2; p.htype = q.htype; p.hlen = q.hlen; p.xid = q.xid; p.flags = q.flags;
        memcpy(p.chaddr, q.chaddr, 16);
        p.magic = htonl(0x63825363);
        if (resp != 6) { p.yiaddr = ip_pc; p.siaddr = ip_srv; }
        uint8_t *o = p.opts;
        *o++ = 53; *o++ = 1; *o++ = (uint8_t)resp;
        *o++ = 54; *o++ = 4; memcpy(o, &ip_srv, 4); o += 4;
        if (resp != 6) {
            uint32_t lease = htonl(86400), mask = inet_addr("255.255.255.0");
            *o++ = 51; *o++ = 4; memcpy(o, &lease, 4); o += 4;
            *o++ = 1;  *o++ = 4; memcpy(o, &mask, 4); o += 4;
        }
        *o++ = 255;
        struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = INADDR_BROADCAST };
        sendto(s, &p, 240 + (size_t)(o - p.opts), 0, (struct sockaddr *)&dst, sizeof dst);
        if (resp == 5) LOG("DHCP: portátil conectado al cable");
    }
    close(s);
    return 0;
}

static int modo_red(void)
{
    signal(SIGTERM, al_terminar);
    signal(SIGINT, al_terminar);
    char ifname[IFNAMSIZ + 1];
    const char *f = funcion_para_velocidad();
    if (f) funcion_actual = f;
    for (;;) {
        if (gadget_crear(ifname, sizeof ifname, funcion_actual)) return 1;
        if (servidor_dhcp(ifname) != 1) break;
        sleep(1);
    }
    gadget_quitar();
    return 0;
}

/* ================================================================== */
/* Teclado y ratón virtuales                                           */
/* ================================================================== */
static int kbd_fd = -1, mouse_fd = -1;
static uint8_t pulsadas[KEY_CNT];

static int crear_uinput(const char *nombre, int raton)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (raton) {
        int botones[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
        for (unsigned i = 0; i < sizeof botones / sizeof *botones; i++)
            ioctl(fd, UI_SET_KEYBIT, botones[i]);
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_X);
        ioctl(fd, UI_SET_RELBIT, REL_Y);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    } else {
        ioctl(fd, UI_SET_EVBIT, EV_REP);
        for (int k = 1; k <= KEY_MICMUTE; k++)
            ioctl(fd, UI_SET_KEYBIT, k);
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x1d6b;
    us.id.product = raton ? 0x7801 : 0x7802;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%s", nombre);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void emitir(int fd, int type, int code, int value)
{
    if (fd < 0) return;
    struct input_event ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].type = type; ev[0].code = code; ev[0].value = value;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof ev) < 0) { /* ignorar */ }
}

static int es_boton(int k) { return k >= BTN_MISC && k < KEY_OK; }

static void soltar_todo(void)
{
    for (int k = 0; k < KEY_CNT; k++) {
        if (!pulsadas[k]) continue;
        pulsadas[k] = 0;
        emitir(es_boton(k) ? mouse_fd : kbd_fd, EV_KEY, k, 0);
    }
}

/* ================================================================== */
/* Estado compartido: último fotograma y conexión                      */
/* ================================================================== */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static uint8_t  *ultimo;          /* 4 bytes por píxel, sin relleno entre filas */
static size_t    ultimo_cap;
static int       ult_w, ult_h, ult_rgb;
static uint64_t  ult_seq;
static int       en_camino, conexion_viva;
static volatile unsigned long n_capturados;

/* sonido capturado pendiente de enviar */
#define AUDIO_CAP (DL_AUDIO_HZ * DL_AUDIO_CANALES * 2)   /* 1 segundo */
static uint8_t audio_buf[AUDIO_CAP];
static size_t  audio_len;

/* ================================================================== */
/* Captura por PipeWire                                                */
/* ================================================================== */
enum { FUENTE_NINGUNA, FUENTE_JUEGO, FUENTE_ESCRITORIO };
static const char *nombre_fuente[] = { "ninguna", "modo juego", "escritorio" };

static struct pw_thread_loop *pwl;
static struct pw_context *pwctx;
static struct pw_core *core_local, *core_portal;
static struct pw_registry *registro;
static struct spa_hook hook_core, hook_reg, hook_st;
static struct pw_stream *st;
static struct spa_video_info_raw formato;
static volatile uint32_t id_gamescope = SPA_ID_INVALID, id_objetivo = SPA_ID_INVALID;
static volatile int st_fallo, core_caido;
static int fuente_actual = FUENTE_NINGUNA;
static sd_bus *bus_portal;
static char ruta_token[512];

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    (void)data;
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mt, mst;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_format_video_raw_parse(param, &formato);
    LOG("Captura: %ux%u", formato.size.width, formato.size.height);

    uint8_t b[256];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(b, sizeof b);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&bld,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    pw_stream_update_params(st, params, 1);
}

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = NULL, *t;
    while ((t = pw_stream_dequeue_buffer(st))) {
        if (b) pw_stream_queue_buffer(st, b);
        b = t;
    }
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    int w = formato.size.width, h = formato.size.height;
    if (d->data && d->chunk && d->chunk->size > 0 && w > 0 && h > 0) {
        int stride = d->chunk->stride ? d->chunk->stride : w * 4;
        const uint8_t *src = (const uint8_t *)d->data + d->chunk->offset;
        size_t fila = (size_t)w * 4, need = fila * h;
        if ((size_t)stride * (h - 1) + fila <= d->maxsize) {
            pthread_mutex_lock(&mtx);
            if (ultimo_cap < need) { free(ultimo); ultimo = malloc(need); ultimo_cap = need; }
            for (int y = 0; y < h; y++) memcpy(ultimo + fila * y, src + (size_t)stride * y, fila);
            ult_w = w; ult_h = h;
            ult_rgb = formato.format == SPA_VIDEO_FORMAT_RGBx || formato.format == SPA_VIDEO_FORMAT_RGBA;
            ult_seq++;
            n_capturados++;
            pthread_cond_broadcast(&cond);
            pthread_mutex_unlock(&mtx);
        }
    }
    pw_stream_queue_buffer(st, b);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state s, const char *err)
{
    (void)data; (void)old;
    if (s == PW_STREAM_STATE_ERROR) { LOG("Captura: error: %s", err ? err : "?"); st_fallo = 1; }
    else if (s == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING) st_fallo = 1;
}

static const struct pw_stream_events ev_stream = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void reg_global(void *d, uint32_t id, uint32_t perm, const char *type, uint32_t ver,
                       const struct spa_dict *props)
{
    (void)d; (void)perm; (void)ver;
    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    const char *n = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (n && !strcmp(n, "gamescope")) id_gamescope = id;
}

static void reg_remove(void *d, uint32_t id)
{
    (void)d;
    if (id == id_gamescope) id_gamescope = SPA_ID_INVALID;
    if (id == id_objetivo) st_fallo = 1;
}

static const struct pw_registry_events ev_reg = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = reg_global,
    .global_remove = reg_remove,
};

static void core_error(void *d, uint32_t id, int seq, int res, const char *msg)
{
    (void)d; (void)seq; (void)msg;
    if (id == PW_ID_CORE && res == -EPIPE) core_caido = 1;
}

static const struct pw_core_events ev_core = {
    PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

static void detener_audio(void);

/* conexión permanente con PipeWire para saber si gamescope está activo */
static void asegurar_core_local(void)
{
    pw_thread_loop_lock(pwl);
    if (core_local && core_caido) {
        detener_audio();
        spa_hook_remove(&hook_reg);
        spa_hook_remove(&hook_core);
        pw_proxy_destroy((struct pw_proxy *)registro);
        pw_core_disconnect(core_local);
        core_local = NULL;
        registro = NULL;
        id_gamescope = SPA_ID_INVALID;
    }
    if (!core_local) {
        core_caido = 0;
        core_local = pw_context_connect(pwctx, NULL, 0);
        if (core_local) {
            pw_core_add_listener(core_local, &hook_core, &ev_core, NULL);
            registro = pw_core_get_registry(core_local, PW_VERSION_REGISTRY, 0);
            pw_registry_add_listener(registro, &hook_reg, &ev_reg, NULL);
        }
    }
    pw_thread_loop_unlock(pwl);
}

static int crear_stream(struct pw_core *core, uint32_t nodo, const char *objetivo)
{
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", NULL);
    if (objetivo) pw_properties_set(props, PW_KEY_TARGET_OBJECT, objetivo);
    st = pw_stream_new(core, "decklink", props);
    if (!st) return -1;
    st_fallo = 0;
    pw_stream_add_listener(st, &hook_st, &ev_stream, NULL);

    uint8_t b[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(b, sizeof b);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&bld,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1280, 800), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
    int r = pw_stream_connect(st, PW_DIRECTION_INPUT, nodo,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if (r < 0) {
        LOG("Captura: no puedo conectar: %s", strerror(-r));
        spa_hook_remove(&hook_st);
        pw_stream_destroy(st);
        st = NULL;
        return -1;
    }
    return 0;
}

/* ---------------- sonido: lo que suena en la Deck ---------------- */
static struct pw_stream *st_audio;
static struct spa_hook hook_audio;

static void on_process_audio(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(st_audio);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk && d->chunk->size) {
        const uint8_t *p = (const uint8_t *)d->data + d->chunk->offset;
        size_t n = d->chunk->size;
        if (n > AUDIO_CAP) { p += n - AUDIO_CAP; n = AUDIO_CAP; }
        pthread_mutex_lock(&mtx);
        if (conexion_viva) {
            if (audio_len + n > AUDIO_CAP) {          /* si se acumula, tirar lo más viejo */
                size_t sobra = audio_len + n - AUDIO_CAP;
                memmove(audio_buf, audio_buf + sobra, audio_len - sobra);
                audio_len -= sobra;
            }
            memcpy(audio_buf + audio_len, p, n);
            audio_len += n;
            pthread_cond_broadcast(&cond);
        }
        pthread_mutex_unlock(&mtx);
    }
    pw_stream_queue_buffer(st_audio, b);
}

static const struct pw_stream_events ev_audio = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process_audio,
};

/* se llama con pwl bloqueado */
static void iniciar_audio(void)
{
    if (st_audio || !core_local) return;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true", PW_KEY_NODE_LATENCY, "256/48000",
        PW_KEY_NODE_NAME, "decklink-sonido", NULL);
    st_audio = pw_stream_new(core_local, "DeckLink sonido", props);
    if (!st_audio) return;
    pw_stream_add_listener(st_audio, &hook_audio, &ev_audio, NULL);
    uint8_t b[512];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(b, sizeof b);
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_S16_LE, .rate = DL_AUDIO_HZ, .channels = DL_AUDIO_CANALES);
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info) };
    if (pw_stream_connect(st_audio, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        spa_hook_remove(&hook_audio);
        pw_stream_destroy(st_audio);
        st_audio = NULL;
        return;
    }
    LOG("Capturando el sonido de la Deck");
}

/* se llama con pwl bloqueado */
static void detener_audio(void)
{
    if (!st_audio) return;
    spa_hook_remove(&hook_audio);
    pw_stream_destroy(st_audio);
    st_audio = NULL;
}

/* ---------------- portal de escritorio (KDE) ---------------- */
typedef struct {
    int listo;
    uint32_t codigo;
    char sesion[256];
    char token[256];
    uint32_t nodo;
} respuesta_t;

static int on_response(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)e;
    respuesta_t *r = ud;
    sd_bus_message_read(m, "u", &r->codigo);
    if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char *k;
            sd_bus_message_read(m, "s", &k);
            if (!strcmp(k, "session_handle")) {
                const char *s;
                sd_bus_message_read(m, "v", "s", &s);
                snprintf(r->sesion, sizeof r->sesion, "%s", s);
            } else if (!strcmp(k, "restore_token")) {
                const char *s;
                sd_bus_message_read(m, "v", "s", &s);
                snprintf(r->token, sizeof r->token, "%s", s);
            } else if (!strcmp(k, "streams")) {
                sd_bus_message_enter_container(m, 'v', "a(ua{sv})");
                sd_bus_message_enter_container(m, 'a', "(ua{sv})");
                if (sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0) {
                    sd_bus_message_read(m, "u", &r->nodo);
                    sd_bus_message_skip(m, "a{sv}");
                    sd_bus_message_exit_container(m);
                }
                while (sd_bus_message_skip(m, "(ua{sv})") > 0) {}
                sd_bus_message_exit_container(m);
                sd_bus_message_exit_container(m);
            } else {
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    r->listo = 1;
    return 0;
}

static void opc(sd_bus_message *m, const char *k, char tipo, const void *v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", k);
    if (tipo == 's') sd_bus_message_append(m, "v", "s", (const char *)v);
    else if (tipo == 'u') sd_bus_message_append(m, "v", "u", *(const uint32_t *)v);
    else sd_bus_message_append(m, "v", "b", *(const int *)v);
    sd_bus_message_close_container(m);
}

static int portal_llamar(const char *metodo, const char *sesion, int con_parent,
                         const char *token_restaurar, respuesta_t *r)
{
    static int n = 0;
    char token[32], ruta[256], unico[128];
    const char *u;
    sd_bus_get_unique_name(bus_portal, &u);
    snprintf(unico, sizeof unico, "%s", u + 1);
    for (char *p = unico; *p; p++) if (*p == '.') *p = '_';
    snprintf(token, sizeof token, "decklink%d", ++n);
    snprintf(ruta, sizeof ruta, "/org/freedesktop/portal/desktop/request/%s/%s", unico, token);

    memset(r, 0, sizeof *r);
    sd_bus_slot *slot = NULL;
    sd_bus_match_signal(bus_portal, &slot, "org.freedesktop.portal.Desktop", ruta,
                        "org.freedesktop.portal.Request", "Response", on_response, r);

    sd_bus_message *m = NULL, *rep = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message_new_method_call(bus_portal, &m, "org.freedesktop.portal.Desktop",
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.ScreenCast", metodo);
    if (sesion) sd_bus_message_append(m, "o", sesion);
    if (con_parent) sd_bus_message_append(m, "s", "");
    sd_bus_message_open_container(m, 'a', "{sv}");
    opc(m, "handle_token", 's', token);
    if (!strcmp(metodo, "CreateSession")) {
        opc(m, "session_handle_token", 's', "decklinksesion");
    } else if (!strcmp(metodo, "SelectSources")) {
        uint32_t tipos = 1, cursor = 2, persist = 2;
        int multiple = 0;
        opc(m, "types", 'u', &tipos);
        opc(m, "multiple", 'b', &multiple);
        opc(m, "cursor_mode", 'u', &cursor);
        opc(m, "persist_mode", 'u', &persist);
        if (token_restaurar && *token_restaurar) opc(m, "restore_token", 's', token_restaurar);
    }
    sd_bus_message_close_container(m);

    int ret = sd_bus_call(bus_portal, m, 10 * 1000000ULL, &err, &rep);
    sd_bus_message_unref(m);
    if (ret < 0) {
        LOG("Portal %s: %s", metodo, err.message ? err.message : strerror(-ret));
        sd_bus_error_free(&err);
        sd_bus_slot_unref(slot);
        return -1;
    }
    sd_bus_message_unref(rep);
    time_t limite = time(NULL) + 180;
    while (!r->listo && conexion_viva && time(NULL) < limite) {
        if (sd_bus_process(bus_portal, NULL) > 0) continue;
        sd_bus_wait(bus_portal, 500000);
    }
    sd_bus_slot_unref(slot);
    if (!r->listo) { LOG("Portal %s: sin respuesta", metodo); return -1; }
    if (r->codigo != 0) {
        LOG("Portal %s: %s", metodo, r->codigo == 1 ? "cancelado en la Deck" : "error");
        return r->codigo == 1 ? -2 : -1;
    }
    return 0;
}

static int iniciar_escritorio(void)
{
    if (sd_bus_open_user(&bus_portal) < 0) { LOG("No puedo abrir D-Bus"); return -1; }

    char token[256] = "";
    FILE *f = fopen(ruta_token, "r");
    if (f) {
        if (!fgets(token, sizeof token, f)) token[0] = 0;
        token[strcspn(token, "\n")] = 0;
        fclose(f);
    }

    respuesta_t r;
    int e;
    char sesion[256];
    if ((e = portal_llamar("CreateSession", NULL, 0, NULL, &r))) goto fallo;
    snprintf(sesion, sizeof sesion, "%s", r.sesion);
    if ((e = portal_llamar("SelectSources", sesion, 0, token, &r))) goto fallo;
    if (!*token) LOG("Pidiendo permiso para compartir la pantalla (acéptalo en la Deck)");
    if ((e = portal_llamar("Start", sesion, 1, NULL, &r))) goto fallo;
    if (!r.nodo) { LOG("El portal no devolvió pantalla"); e = -1; goto fallo; }
    if (*r.token) {
        f = fopen(ruta_token, "w");
        if (f) { fprintf(f, "%s\n", r.token); fclose(f); }
    }

    {
        sd_bus_message *m = NULL, *rep = NULL;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message_new_method_call(bus_portal, &m, "org.freedesktop.portal.Desktop",
                                       "/org/freedesktop/portal/desktop",
                                       "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote");
        sd_bus_message_append(m, "o", sesion);
        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_close_container(m);
        int ret = sd_bus_call(bus_portal, m, 10 * 1000000ULL, &err, &rep);
        sd_bus_message_unref(m);
        if (ret < 0) { LOG("Portal OpenPipeWireRemote: %s", err.message); sd_bus_error_free(&err); e = -1; goto fallo; }
        int fd;
        sd_bus_message_read(rep, "h", &fd);
        int fd2 = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        sd_bus_message_unref(rep);

        pw_thread_loop_lock(pwl);
        core_portal = pw_context_connect_fd(pwctx, fd2, NULL, 0);
        int ok = core_portal && crear_stream(core_portal, r.nodo, NULL) == 0;
        if (ok) id_objetivo = r.nodo;
        pw_thread_loop_unlock(pwl);
        if (!ok) { e = -1; goto fallo; }
    }
    return 0;

fallo:
    if (bus_portal) { sd_bus_flush_close_unref(bus_portal); bus_portal = NULL; }
    return e;
}

static int iniciar_juego(void)
{
    pw_thread_loop_lock(pwl);
    int ok = core_local && crear_stream(core_local, PW_ID_ANY, "gamescope") == 0;
    if (ok) id_objetivo = id_gamescope;
    pw_thread_loop_unlock(pwl);
    return ok ? 0 : -1;
}

static void detener_fuente(void)
{
    pw_thread_loop_lock(pwl);
    if (st) {
        spa_hook_remove(&hook_st);
        pw_stream_destroy(st);
        st = NULL;
    }
    if (core_portal) { pw_core_disconnect(core_portal); core_portal = NULL; }
    id_objetivo = SPA_ID_INVALID;
    pw_thread_loop_unlock(pwl);
    if (bus_portal) { sd_bus_flush_close_unref(bus_portal); bus_portal = NULL; }
    fuente_actual = FUENTE_NINGUNA;
}

static int fuente_deseada(void)
{
    if (id_gamescope != SPA_ID_INVALID) return FUENTE_JUEGO;
    if (proceso_existe("kwin_wayland") || proceso_existe("kwin_x11")) return FUENTE_ESCRITORIO;
    return FUENTE_NINGUNA;
}

/* mientras el portátil está conectado: elegir y mantener la captura */
static void *hilo_gestor(void *arg)
{
    (void)arg;
    time_t esperar_hasta = 0;
    for (;;) {
        pthread_mutex_lock(&mtx);
        int viva = conexion_viva;
        pthread_mutex_unlock(&mtx);
        if (!viva) break;

        asegurar_core_local();
        pw_thread_loop_lock(pwl);
        iniciar_audio();
        pw_thread_loop_unlock(pwl);
        int quiero = fuente_deseada();
        if ((quiero != fuente_actual || st_fallo) && time(NULL) >= esperar_hasta) {
            if (fuente_actual != FUENTE_NINGUNA || st_fallo) detener_fuente();
            if (quiero != FUENTE_NINGUNA) {
                LOG("Capturando: %s", nombre_fuente[quiero]);
                int r = quiero == FUENTE_JUEGO ? iniciar_juego() : iniciar_escritorio();
                if (r == 0) fuente_actual = quiero;
                else esperar_hasta = time(NULL) + (r == -2 ? 30 : 3);
            }
        }
        usleep(300000);
    }
    detener_fuente();
    pw_thread_loop_lock(pwl);
    detener_audio();
    pw_thread_loop_unlock(pwl);
    return NULL;
}

/* ================================================================== */
/* Codificación                                                        */
/* ================================================================== */
#define MAX_FRANJAS 4
typedef struct {
    tjhandle tj;
    const uint8_t *src;
    int w, h, pf, q, rgb;
    uint8_t *out;
    unsigned long tam, cap;
    uint8_t *bgr;                 /* para LZ4: la franja en 3 bytes por píxel */
    size_t bgr_cap;
    int err;
} franja_t;
static franja_t franjas[MAX_FRANJAS];

static void *comprimir_franja(void *a)
{
    franja_t *f = a;
    unsigned long need = tjBufSize(f->w, f->h, TJSAMP_444);
    if (f->cap < need) { tjFree(f->out); f->out = tjAlloc((int)need); f->cap = need; }
    f->tam = f->cap;
    f->err = tjCompress2(f->tj, f->src, f->w, f->w * 4, f->h, f->pf, &f->out, &f->tam,
                         TJSAMP_444, f->q, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
    return NULL;
}

static void *lz4_franja(void *a)
{
    franja_t *f = a;
    size_t n = (size_t)f->w * f->h;
    if (f->bgr_cap < n * 3) { free(f->bgr); f->bgr = malloc(n * 3); f->bgr_cap = n * 3; }
    const uint8_t *s = f->src;
    uint8_t *o = f->bgr;
    if (f->rgb) for (size_t i = 0; i < n; i++, s += 4, o += 3) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; }
    else        for (size_t i = 0; i < n; i++, s += 4, o += 3) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; }
    unsigned long need = (unsigned long)LZ4_compressBound((int)(n * 3));
    if (f->cap < need) { tjFree(f->out); f->out = tjAlloc((int)need); f->cap = need; }
    int r = LZ4_compress_default((const char *)f->bgr, (char *)f->out, (int)(n * 3), (int)f->cap);
    f->err = r <= 0;
    f->tam = r > 0 ? (unsigned long)r : 0;
    return NULL;
}

/* ================================================================== */
/* Conexión con el portátil                                            */
/* ================================================================== */
static void *hilo_entrada(void *arg)
{
    int s = (int)(intptr_t)arg;
    dl_input m;
    while (recv_todo(s, &m, sizeof m) == 0) {
        if (m.type == DL_ACK) {
            pthread_mutex_lock(&mtx);
            if (en_camino > 0) en_camino--;
            pthread_cond_broadcast(&cond);
            pthread_mutex_unlock(&mtx);
        } else if (m.type == EV_REL) {
            emitir(mouse_fd, EV_REL, m.code, m.value);
        } else if (m.type == EV_KEY && m.code < KEY_CNT) {
            pulsadas[m.code] = m.value != 0;
            emitir(es_boton(m.code) ? mouse_fd : kbd_fd, EV_KEY, m.code, m.value);
        }
    }
    pthread_mutex_lock(&mtx);
    conexion_viva = 0;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mtx);
    shutdown(s, SHUT_RDWR);
    return NULL;
}

/* 1 = USB 2 (JPEG), 0 = USB 3 (sin comprimir) */
static int elegir_modo(const char *dir_conf, int forzado)
{
    if (forzado >= 0) return forzado;
    char ruta[600], v[32];
    snprintf(ruta, sizeof ruta, "%s/modo", dir_conf);
    if (leer(ruta, v, sizeof v) == 0) {
        if (!strcmp(v, "usb2")) return 1;
        if (!strcmp(v, "usb3")) return 0;
    }
    char udc[128];
    if (primer_udc(udc, sizeof udc) == 0) {
        snprintf(ruta, sizeof ruta, "/sys/class/udc/%s/current_speed", udc);
        if (leer(ruta, v, sizeof v) == 0) {
            LOG("Velocidad del cable: %s", v);
            if (!strcmp(v, "high-speed") || !strcmp(v, "full-speed")) return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    int forzado = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--red")) return modo_red();
        if (!strcmp(argv[i], "--dhcp-prueba") && i + 1 < argc) { signal(SIGTERM, al_terminar); return servidor_dhcp(argv[i + 1]); }
        if (!strcmp(argv[i], "--usb2")) forzado = 1;
        if (!strcmp(argv[i], "--usb3")) forzado = 0;
    }

    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char dir[400];
    snprintf(dir, sizeof dir, "%s/.config/decklink", home);
    mkdir(dir, 0700);
    snprintf(ruta_token, sizeof ruta_token, "%s/permiso-pantalla", dir);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--olvidar")) { unlink(ruta_token); LOG("Permiso borrado"); return 0; }

    pw_init(&argc, &argv);
    pwl = pw_thread_loop_new("decklink-captura", NULL);
    pwctx = pw_context_new(pw_thread_loop_get_loop(pwl), NULL, 0);
    pw_thread_loop_start(pwl);

    for (int i = 0; i < MAX_FRANJAS; i++) franjas[i].tj = tjInitCompress();

    kbd_fd   = crear_uinput("DeckLink teclado", 0);
    mouse_fd = crear_uinput("DeckLink raton", 1);
    if (kbd_fd < 0 || mouse_fd < 0) LOG("AVISO: sin teclado/ratón virtuales (/dev/uinput)");

    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int uno = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DL_PUERTO),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        LOG("No puedo abrir el puerto %d: %s", DL_PUERTO, strerror(errno));
        return 1;
    }
    LOG("DeckLink listo. Esperando al portátil...");

    uint8_t *copia = NULL;
    size_t copia_cap = 0;

    for (;;) {
        int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
        int usb2 = elegir_modo(dir, forzado);
        LOG("Portátil conectado (%s)", usb2 ? "modo USB 2: JPEG" : "modo USB 3: color completo sin pérdidas");

        pthread_mutex_lock(&mtx);
        en_camino = 0;
        audio_len = 0;
        conexion_viva = 1;
        uint64_t enviado = ult_seq;      /* esperar a un fotograma nuevo de esta sesión */
        pthread_mutex_unlock(&mtx);

        pthread_t th_in, th_gestor;
        pthread_create(&th_in, NULL, hilo_entrada, (void *)(intptr_t)c);
        pthread_create(&th_gestor, NULL, hilo_gestor, NULL);

        int calidad = 90;
        unsigned long n_enviados = 0, cap0 = n_capturados;
        time_t t_est = time(NULL);
        uint8_t audio_envio[AUDIO_CAP];
        for (;;) {
            pthread_mutex_lock(&mtx);
            while (conexion_viva && audio_len == 0 && (en_camino > 0 || ult_seq == enviado)) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;
                pthread_cond_timedwait(&cond, &mtx, &ts);
            }
            if (time(NULL) - t_est >= 30) {
                LOG("Últimos 30 s: %lu fotogramas enviados (%.0f fps)%s", n_enviados, n_enviados / 30.0,
                    n_capturados == cap0 ? ", pantalla sin cambios" : "");
                n_enviados = 0; cap0 = n_capturados; t_est = time(NULL);
            }
            if (!conexion_viva) { pthread_mutex_unlock(&mtx); break; }

            /* el sonido va primero: es poco y no puede esperar */
            if (audio_len) {
                size_t n = audio_len & ~(size_t)3;
                memcpy(audio_envio, audio_buf, n);
                memmove(audio_buf, audio_buf + n, audio_len - n);
                audio_len -= n;
                pthread_mutex_unlock(&mtx);
                if (!n) continue;
                dl_frame_hdr ha = { DL_MAGIC, 0, 0, DL_FMT_AUDIO, {0}, (uint32_t)n };
                if (send_todo(c, &ha, sizeof ha) || send_todo(c, audio_envio, n)) break;
                continue;
            }

            int w = ult_w, h = ult_h, rgb = ult_rgb;
            size_t n = (size_t)w * h;
            if (copia_cap < n * 4) { free(copia); copia = malloc(n * 4); copia_cap = n * 4; }
            memcpy(copia, ultimo, n * 4);
            enviado = ult_seq;
            en_camino++;
            pthread_mutex_unlock(&mtx);

            int nf = h >= 64 ? MAX_FRANJAS : 1;
            pthread_t th[MAX_FRANJAS];
            for (int i = 0; i < nf; i++) {
                franja_t *f = &franjas[i];
                f->src = copia + (size_t)dl_franja_y0(i, nf, h) * w * 4;
                f->w = w; f->h = dl_franja_h(i, nf, h);
                f->pf = rgb ? TJPF_RGBX : TJPF_BGRX;
                f->rgb = rgb;
                f->q = calidad;
                pthread_create(&th[i], NULL, usb2 ? comprimir_franja : lz4_franja, f);
            }
            uint8_t cab[1 + 4 * MAX_FRANJAS];
            uint32_t total = 1 + 4 * nf;
            cab[0] = (uint8_t)nf;
            for (int i = 0; i < nf; i++) {
                pthread_join(th[i], NULL);
                uint32_t t = franjas[i].err ? 0 : (uint32_t)franjas[i].tam;
                memcpy(cab + 1 + 4 * i, &t, 4);
                total += t;
            }
            dl_frame_hdr hd = { DL_MAGIC, (uint16_t)w, (uint16_t)h, usb2 ? DL_FMT_JPEG : DL_FMT_LZ4, {0}, total };
            int err = send_todo(c, &hd, sizeof hd) || send_todo(c, cab, 1 + 4 * nf);
            for (int i = 0; i < nf && !err; i++)
                if (!franjas[i].err) err = send_todo(c, franjas[i].out, franjas[i].tam);
            if (usb2) {
                /* calidad alta pero sin pasarse de lo que aguanta USB 2 (~450 KB por fotograma) */
                if (total > 450000 && calidad > 70) calidad -= 3;
                else if (total < 280000 && calidad < 95) calidad += 1;
            }
            if (err) break;
            n_enviados++;
        }

        shutdown(c, SHUT_RDWR);
        pthread_join(th_in, NULL);
        pthread_join(th_gestor, NULL);
        close(c);
        soltar_todo();
        LOG("Portátil desconectado");
    }
}
