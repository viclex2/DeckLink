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
#include <poll.h>
#include <sys/uio.h>
#include <limits.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/uinput.h>
#include <linux/usb/functionfs.h>
#include <sys/mount.h>
#include <pwd.h>
#include <endian.h>
#include <systemd/sd-bus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/utils/dict.h>
#include <turbojpeg.h>
#include "terceros/lz4.h"
#include <spa/param/audio/format-utils.h>
#include "protocolo.h"
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

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

static uint64_t ahora_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* conexión por wifi: activada si existe el archivo "wifi" con un PIN de 4 a 8 cifras */
static int wifi_pin(const char *dir_conf, int32_t *pin)
{
    char ruta[600], v[32];
    snprintf(ruta, sizeof ruta, "%s/wifi", dir_conf);
    if (leer(ruta, v, sizeof v)) return 0;
    int32_t n = 0;
    int cifras = 0;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (*p - '0');
        cifras++;
    }
    if (cifras < 4 || cifras > 8) return 0;
    *pin = n;
    return 1;
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

/* Una "red" por cada tarjeta USB que presenta la Deck */
typedef struct {
    const char *func;          /* "ncm.usb0", "ecm.usb0", "ncm.usb1" */
    const char *mac_deck, *mac_equipo;
    const char *ip_deck, *ip_equipo;
    char ifname[IFNAMSIZ + 1];
} red_t;

/* ---- canal USB directo (FunctionFS): para Android, que no usa la red USB ---- */
static int ffs_ep0 = -1;
static uid_t ffs_uid = 1000;
static gid_t ffs_gid = 1000;

/* Descriptores "Microsoft OS": Windows instala WinUSB para el canal directo sin pedir drivers */
struct os_cab {
    __u8 interface;
    __le32 dwLength;
    __le16 bcdVersion, wIndex;
    __le16 wCount;          /* compat: bCount + reservado */
} __attribute__((packed));
#define NOMBRE_PROP "DeviceInterfaceGUID"

static const struct {
    struct usb_functionfs_descs_head_v2 cab;
    __le32 n_fs, n_hs, n_ss, n_os;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio entra, sale;
    } __attribute__((packed)) fs, hs;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio entra;
        struct usb_ss_ep_comp_descriptor entra_c;
        struct usb_endpoint_descriptor_no_audio sale;
        struct usb_ss_ep_comp_descriptor sale_c;
    } __attribute__((packed)) ss;
    struct {
        struct os_cab cab;
        __u8 bFirstInterfaceNumber, Reserved1;
        char CompatibleID[8], SubCompatibleID[8];
        __u8 Reserved2[6];
    } __attribute__((packed)) compat;
    struct {
        struct os_cab cab;
        __le32 dwSize, dwPropertyDataType;
        __le16 wPropertyNameLength;
        char nombre[sizeof NOMBRE_PROP];
        __le32 dwPropertyDataLength;
        char dato[sizeof DL_WINUSB_GUID];
    } __attribute__((packed)) prop;
} __attribute__((packed)) ffs_desc = {
    .cab = {
        .magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
        .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC | FUNCTIONFS_HAS_MS_OS_DESC,
        .length = sizeof ffs_desc,
    },
    .n_fs = 3, .n_hs = 3, .n_ss = 5, .n_os = 2,
#define INTF { .bLength = sizeof(struct usb_interface_descriptor), .bDescriptorType = USB_DT_INTERFACE, \
               .bNumEndpoints = 2, .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .bInterfaceSubClass = 0x44, \
               .bInterfaceProtocol = 0x4c, .iInterface = 1 }
#define EP(dir, mp) { .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT, \
               .bEndpointAddress = (dir), .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = (mp) }
#define COMP { .bLength = USB_DT_SS_EP_COMP_SIZE, .bDescriptorType = USB_DT_SS_ENDPOINT_COMP }
    .fs = { INTF, EP(1 | USB_DIR_IN, 64), EP(2 | USB_DIR_OUT, 64) },
    .hs = { INTF, EP(1 | USB_DIR_IN, 512), EP(2 | USB_DIR_OUT, 512) },
    .ss = { INTF, EP(1 | USB_DIR_IN, 1024), COMP, EP(2 | USB_DIR_OUT, 1024), COMP },
    .compat = {
        .cab = { .interface = 0, .dwLength = 11 + 24, .bcdVersion = 0x0100, .wIndex = 4, .wCount = 1 },
        .bFirstInterfaceNumber = 0, .Reserved1 = 0, .CompatibleID = "WINUSB",
    },
    /* el kernel pasa el nombre y el dato a UTF-16 al mandarlos a Windows */
    .prop = {
        .cab = { .interface = 0, .dwLength = 11 + 14 + sizeof NOMBRE_PROP + sizeof DL_WINUSB_GUID,
                 .bcdVersion = 0x0100, .wIndex = 5, .wCount = 1 },
        .dwSize = 14 + sizeof NOMBRE_PROP + sizeof DL_WINUSB_GUID,
        .dwPropertyDataType = 1,            /* REG_SZ */
        .wPropertyNameLength = sizeof NOMBRE_PROP,
        .nombre = NOMBRE_PROP,
        .dwPropertyDataLength = sizeof DL_WINUSB_GUID,
        .dato = DL_WINUSB_GUID,
    },
#undef INTF
#undef EP
#undef COMP
};

static const struct {
    struct usb_functionfs_strings_head cab;
    struct { __le16 code; const char s1[sizeof "DeckLink"]; } __attribute__((packed)) es;
} __attribute__((packed)) ffs_str = {
    .cab = { .magic = FUNCTIONFS_STRINGS_MAGIC, .length = sizeof ffs_str, .str_count = 1, .lang_count = 1 },
    .es = { 0x0409, "DeckLink" },
};

/* atiende ep0: no esperamos peticiones de control, pero hay que contestarlas */
static void *hilo_ep0(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct usb_functionfs_event ev[4];
    for (;;) {
        ssize_t r = read(fd, ev, sizeof ev);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++) {
            if (ev[i].type != FUNCTIONFS_SETUP) continue;
            if (ev[i].u.setup.bRequestType & USB_DIR_IN) { if (write(fd, "", 0) < 0) { } }
            else { char b[8]; if (read(fd, b, 0) < 0) { } }
        }
    }
    return NULL;
}

static void ffs_quitar(void)
{
    if (ffs_ep0 >= 0) { close(ffs_ep0); ffs_ep0 = -1; }
    umount2(DL_FFS_DIR, MNT_DETACH);
}

/* crea la función, monta FunctionFS para el usuario de la Deck y escribe los descriptores */
static int ffs_preparar(void)
{
    /* versión sin los descriptores de Windows, por si este kernel no los acepta */
    uint8_t sin_os[sizeof ffs_desc];
    size_t n_sin = 0;
    {
        struct usb_functionfs_descs_head_v2 c = ffs_desc.cab;
        size_t trozo = offsetof(__typeof__(ffs_desc), compat) - offsetof(__typeof__(ffs_desc), fs);
        c.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC;
        c.length = (__le32)(sizeof c + 12 + trozo);
        memcpy(sin_os, &c, sizeof c);                       n_sin = sizeof c;
        memcpy(sin_os + n_sin, &ffs_desc.n_fs, 12);         n_sin += 12;
        memcpy(sin_os + n_sin, &ffs_desc.fs, trozo);        n_sin += trozo;
    }
    for (int intento = 0; intento < 2; intento++) {
        ffs_quitar();
        if (mkdir(GADGET "/functions/ffs.decklink", 0755) && errno != EEXIST) return -1;
        mkdir(DL_FFS_DIR, 0755);
        char opts[64];
        snprintf(opts, sizeof opts, "uid=%u,gid=%u", (unsigned)ffs_uid, (unsigned)ffs_gid);
        if (mount("decklink", DL_FFS_DIR, "functionfs", 0, opts)) { LOG("FunctionFS: no se puede montar: %s", strerror(errno)); return -1; }
        ffs_ep0 = open(DL_FFS_DIR "/ep0", O_RDWR | O_CLOEXEC);
        if (ffs_ep0 < 0) return -1;
        const void *d = intento ? (const void *)sin_os : (const void *)&ffs_desc;
        size_t nd = intento ? n_sin : sizeof ffs_desc;
        if (write(ffs_ep0, d, nd) == (ssize_t)nd && write(ffs_ep0, &ffs_str, sizeof ffs_str) == (ssize_t)sizeof ffs_str)
            break;
        LOG("FunctionFS: descriptores rechazados%s: %s", intento ? "" : " (con los de Windows)", strerror(errno));
        ffs_quitar();
        if (intento) return -1;
    }
    pthread_t th;
    pthread_create(&th, NULL, hilo_ep0, (void *)(intptr_t)ffs_ep0);
    pthread_detach(th);
    return 0;
}

static void gadget_quitar(void)
{
    static const char *funcs[] = { "ncm.usb0", "ecm.usb0", "ncm.usb1" };
    char r[300];
    if (access(GADGET, F_OK) != 0) return;
    escribir(GADGET "/UDC", "\n");
    unlink(GADGET "/os_desc/c.1");
    unlink(GADGET "/configs/c.1/ffs.decklink");
    ffs_quitar();
    rmdir(GADGET "/functions/ffs.decklink");
    for (unsigned i = 0; i < 3; i++) {
        snprintf(r, sizeof r, GADGET "/configs/c.1/%s", funcs[i]);
        unlink(r);
    }
    rmdir(GADGET "/configs/c.1/strings/0x409");
    rmdir(GADGET "/configs/c.1");
    for (unsigned i = 0; i < 3; i++) {
        snprintf(r, sizeof r, GADGET "/functions/%s", funcs[i]);
        rmdir(r);
    }
    rmdir(GADGET "/strings/0x409");
    rmdir(GADGET);
}

static volatile sig_atomic_t salir_red = 0;
static void al_terminar(int s) { (void)s; salir_red = 1; }

/*
 * modo "ncm"  (USB 3): una tarjeta NCM -> 10.77.0.x. La usan Linux, Windows y Android.
 * modo "dual" (USB 2): ECM -> 10.77.0.x (Linux; con NCM a USB 2 se atascaba) y
 *                      NCM -> 10.79.0.x (Windows no tiene driver ECM; Android usa la que pueda).
 * Las MAC del lado del equipo son "globales" (no locales) para que Android llame a la
 * tarjeta eth0/eth1 y la use como Ethernet.
 * La tarjeta NCM lleva la etiqueta WINNCM para que Windows 10/11 cargue su driver solo.
 */
static int gadget_crear(const char *modo, red_t *redes, int *nredes)
{
    int n = 0;
    if (!strcmp(modo, "dual")) {
        redes[n++] = (red_t){ "ecm.usb0", "02:de:c1:00:00:01", "04:de:c1:00:00:02", DL_IP_DECK, DL_IP_PC, "" };
        redes[n++] = (red_t){ "ncm.usb1", "02:de:c1:00:01:01", "04:de:c1:00:01:02", DL_IP_DECK2, DL_IP_PC2, "" };
    } else {
        redes[n++] = (red_t){ "ncm.usb0", "02:de:c1:00:00:01", "04:de:c1:00:00:02", DL_IP_DECK, DL_IP_PC, "" };
    }
    *nredes = n;

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
    escribir(GADGET "/bcdDevice", "0x0403");   /* versión nueva: Windows vuelve a leer los descriptores */
    escribir(GADGET "/bcdUSB", "0x0300");
    escribir(GADGET "/bDeviceClass", "0xEF");      /* "varias funciones": así lo espera Windows */
    escribir(GADGET "/bDeviceSubClass", "0x02");
    escribir(GADGET "/bDeviceProtocol", "0x01");
    escribir(GADGET "/os_desc/use", "1");
    escribir(GADGET "/os_desc/b_vendor_code", "0xcd");
    escribir(GADGET "/os_desc/qw_sign", "MSFT100");
    mkdir(GADGET "/strings/0x409", 0755);
    escribir(GADGET "/strings/0x409/manufacturer", "DeckLink");
    escribir(GADGET "/strings/0x409/product", "Steam Deck DeckLink");
    escribir(GADGET "/strings/0x409/serialnumber", "decklink0002");
    mkdir(GADGET "/configs/c.1", 0755);
    mkdir(GADGET "/configs/c.1/strings/0x409", 0755);
    escribir(GADGET "/configs/c.1/strings/0x409/configuration", "DeckLink");
    escribir(GADGET "/configs/c.1/MaxPower", "100");

    char fdir[300], ruta[400];
    for (int i = 0; i < n; i++) {
        snprintf(fdir, sizeof fdir, GADGET "/functions/%s", redes[i].func);
        mkdir(fdir, 0755);
        snprintf(ruta, sizeof ruta, "%s/dev_addr", fdir);  escribir(ruta, redes[i].mac_deck);
        snprintf(ruta, sizeof ruta, "%s/host_addr", fdir); escribir(ruta, redes[i].mac_equipo);
        if (!strncmp(redes[i].func, "ncm", 3)) {
            snprintf(ruta, sizeof ruta, "%s/os_desc/interface.ncm/compatible_id", fdir);
            escribir(ruta, "WINNCM");
        }
        snprintf(ruta, sizeof ruta, GADGET "/configs/c.1/%s", redes[i].func);
        if (symlink(fdir, ruta) && errno != EEXIST) { LOG("No puedo enlazar %s", redes[i].func); return -1; }
    }
    /* canal USB directo para Android (si falla, la red sigue funcionando igual) */
    if (ffs_preparar() == 0) {
        if (symlink(GADGET "/functions/ffs.decklink", GADGET "/configs/c.1/ffs.decklink") && errno != EEXIST)
            LOG("No puedo enlazar el canal USB directo");
        else
            LOG("Canal USB directo para Android listo");
    }
    if (symlink(GADGET "/configs/c.1", GADGET "/os_desc/c.1") && errno != EEXIST) { /* sin Windows, da igual */ }
    if (escribir(GADGET "/UDC", udc)) { LOG("No puedo activar el gadget en %s", udc); return -1; }

    for (int i = 0; i < n; i++) {
        snprintf(ruta, sizeof ruta, GADGET "/functions/%s/ifname", redes[i].func);
        if (leer(ruta, redes[i].ifname, sizeof redes[i].ifname)) snprintf(redes[i].ifname, sizeof redes[i].ifname, "usb%d", i);
        char cmd[512];
        snprintf(cmd, sizeof cmd,
                 "nmcli device set %s managed no >/dev/null 2>&1; "
                 "ip addr flush dev %s; ip addr add %s/24 dev %s && ip link set %s up",
                 redes[i].ifname, redes[i].ifname, redes[i].ip_deck, redes[i].ifname, redes[i].ifname);
        if (system(cmd)) { LOG("No puedo configurar la IP de %s", redes[i].ifname); return -1; }
        LOG("Red USB lista: %s = %s (%s)", redes[i].ifname, redes[i].ip_deck, redes[i].func);
    }
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

/* "ncm" con USB 3, "dual" con USB 2; NULL si aún no hay nada conectado */
static const char *modo_para_velocidad(void)
{
    char v[64];
    if (leer("/etc/decklink/usb", v, sizeof v) == 0 && (!strcmp(v, "ncm") || !strcmp(v, "dual")))
        return !strcmp(v, "ncm") ? "ncm" : "dual";
    char udc[128], ruta[300];
    if (primer_udc(udc, sizeof udc)) return NULL;
    snprintf(ruta, sizeof ruta, "/sys/class/udc/%s/current_speed", udc);
    if (leer(ruta, v, sizeof v)) return NULL;
    if (!strncmp(v, "super-speed", 11)) return "ncm";
    if (!strcmp(v, "high-speed") || !strcmp(v, "full-speed")) return "dual";
    return NULL;
}

static const char *modo_actual = "ncm";

static void responder_dhcp(int s, const dhcp_t *q, ssize_t r, const red_t *red)
{
    uint32_t ip_srv = inet_addr(red->ip_deck), ip_cli = inet_addr(red->ip_equipo);
    int tipo = 0;
    uint32_t pedida = 0;
    for (size_t i = 0; i + 1 < (size_t)r - 240 && q->opts[i] != 255;) {
        uint8_t o = q->opts[i];
        if (o == 0) { i++; continue; }
        uint8_t l = q->opts[i + 1];
        if (o == 53 && l >= 1) tipo = q->opts[i + 2];
        if (o == 50 && l == 4) memcpy(&pedida, &q->opts[i + 2], 4);
        i += 2 + l;
    }
    int resp;
    if (tipo == 1) resp = 2;                                          /* DISCOVER -> OFFER */
    else if (tipo == 3) resp = (pedida && pedida != ip_cli) ? 6 : 5;  /* REQUEST -> ACK/NAK */
    else return;

    dhcp_t p;
    memset(&p, 0, sizeof p);
    p.op = 2; p.htype = q->htype; p.hlen = q->hlen; p.xid = q->xid; p.flags = q->flags;
    memcpy(p.chaddr, q->chaddr, 16);
    p.magic = htonl(0x63825363);
    if (resp != 6) { p.yiaddr = ip_cli; p.siaddr = ip_srv; }
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
    if (resp == 5) LOG("DHCP: equipo conectado por %s (%s)", red->ifname, red->ip_equipo);
}

/* Reparte IP en cada tarjeta. Devuelve 1 si hay que rehacer el gadget (cambió la velocidad). */
static int servidor_dhcp(red_t *redes, int n)
{
    int s[4];
    for (int i = 0; i < n; i++) {
        s[i] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int uno = 1;
        setsockopt(s[i], SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
        setsockopt(s[i], SOL_SOCKET, SO_BROADCAST, &uno, sizeof uno);
        if (setsockopt(s[i], SOL_SOCKET, SO_BINDTODEVICE, redes[i].ifname, strlen(redes[i].ifname) + 1) < 0) {
            LOG("DHCP: no puedo usar %s: %s", redes[i].ifname, strerror(errno));
            return -1;
        }
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(s[i], (struct sockaddr *)&a, sizeof a) < 0) { LOG("DHCP: puerto 67 ocupado: %s", strerror(errno)); return -1; }
    }
    time_t revisar = time(NULL) + 2;
    int ret = 0;
    while (!salir_red) {
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = 0;
        for (int i = 0; i < n; i++) { FD_SET(s[i], &fs); if (s[i] > maxfd) maxfd = s[i]; }
        struct timeval tv = { 1, 0 };
        if (select(maxfd + 1, &fs, NULL, NULL, &tv) > 0) {
            for (int i = 0; i < n; i++) {
                if (!FD_ISSET(s[i], &fs)) continue;
                dhcp_t q;
                ssize_t r = recv(s[i], &q, sizeof q, 0);
                if (r >= 240 && q.op == 1 && q.magic == htonl(0x63825363)) responder_dhcp(s[i], &q, r, &redes[i]);
            }
        }
        if (time(NULL) >= revisar) {
            revisar = time(NULL) + 2;
            const char *m = modo_para_velocidad();
            if (m && strcmp(m, modo_actual)) {
                LOG("Cable a %s: cambiando a %s", !strcmp(m, "dual") ? "USB 2" : "USB 3",
                    !strcmp(m, "dual") ? "dos tarjetas (ECM + NCM)" : "una tarjeta NCM");
                modo_actual = m;
                ret = 1;
                break;
            }
        }
    }
    for (int i = 0; i < n; i++) close(s[i]);
    return ret;
}

static int modo_red(const char *usuario)
{
    struct passwd *pw = getpwnam(usuario ? usuario : "deck");
    if (pw) { ffs_uid = pw->pw_uid; ffs_gid = pw->pw_gid; }
    signal(SIGTERM, al_terminar);
    signal(SIGINT, al_terminar);
    red_t redes[4];
    int n;
    const char *m = modo_para_velocidad();
    if (m) modo_actual = m;
    for (;;) {
        if (gadget_crear(modo_actual, redes, &n)) return 1;
        if (servidor_dhcp(redes, n) != 1) break;
        sleep(1);
    }
    gadget_quitar();
    return 0;
}

/* ================================================================== */
/* Teclado y ratón virtuales                                           */
/* ================================================================== */
static int kbd_fd = -1, mouse_fd = -1, abs_fd = -1;
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

/* "ratón" de posición absoluta: lo usan las pantallas táctiles (Android) */
static int crear_uinput_abs(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    struct uinput_abs_setup ax;
    memset(&ax, 0, sizeof ax);
    ax.absinfo.maximum = DL_ABS_MAX;
    ax.code = ABS_X; ioctl(fd, UI_ABS_SETUP, &ax);
    ax.code = ABS_Y; ioctl(fd, UI_ABS_SETUP, &ax);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x1d6b;
    us.id.product = 0x7803;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "DeckLink tactil");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
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
        if (es_boton(k)) emitir(abs_fd, EV_KEY, k, 0);
    }
}

/* ================================================================== */
/* Estado compartido: último fotograma y conexión                      */
/* ================================================================== */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static uint8_t  *ultimo;          /* 4 bytes por píxel, sin relleno entre filas */
static size_t    ultimo_cap;
static uint8_t  *trasero;         /* donde se copia la captura nueva, sin bloquear a nadie */
static size_t    trasero_cap;
static double    copia_ms;        /* lo que tarda en copiarse cada captura (media) */
static volatile unsigned fps_captura;
static int       ult_w, ult_h, ult_rgb;
static uint64_t  ult_seq;
static uint64_t  ult_t_cap;      /* cuándo llegó la última captura (para medir el retraso) */
static int       en_camino, conexion_viva;
static int       hola_recibido, cliente_caps = DL_CAP_LZ4;
static int32_t   cliente_pin = -1;
static volatile uint64_t t_ack_us;     /* momento de la última confirmación del cliente */
#define EN_CAMINO_MAX 2                /* fotogramas enviados sin confirmar: la Deck prepara uno mientras viaja otro */
static uint64_t  envio_t[EN_CAMINO_MAX + 2], envio_cap[EN_CAMINO_MAX + 2];
static uint8_t   envio_id[EN_CAMINO_MAX + 2];   /* número de fotograma (1..255) que la pantalla devuelve en el ACK */
static uint8_t   sig_id = 1;
static double    retraso_ms;           /* de la captura a la confirmación de la pantalla, media móvil */
static int       envio_ini, envio_n;
static double    ciclo_medido_ms;      /* de enviar a confirmar, media móvil */
static unsigned long ciclos_medidos;
static pthread_t hilo_principal;
static volatile uint64_t usb_escribiendo_desde;   /* 0 = no se está escribiendo en el canal USB */
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
static volatile int dmabuf_mal;      /* la vía rápida (DMA-BUF) falló: usar la normal */
/* búfer DMA-BUF (escritorio por la vía rápida): lo mapeamos nosotros */
typedef struct { void *p; size_t n; } mapa_t;
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
    int dmabuf = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
    LOG("Captura: %ux%u%s", formato.size.width, formato.size.height, dmabuf ? " (vía rápida de la gráfica)" : "");

    uint8_t b[256];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(b, sizeof b);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&bld,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        /* todos los búferes que deje gamescope (8): si otro programa (p. ej. la grabación
           de partidas de Steam) se queda alguno, gamescope sigue teniendo dónde pintar */
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 8),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(dmabuf ? (1 << SPA_DATA_DmaBuf)
                                                                    : (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    pw_stream_update_params(st, params, 1);
}

/* Copia desde la memoria de la captura. gamescope da la imagen en memoria de la
   gráfica sin caché: leerla con memcpy es lentísimo (~20 ms por imagen, limita a
   ~50 fps). Con lecturas "streaming" (MOVNTDQA, como hace Mesa) es varias veces más rápido. */
__attribute__((target("sse4.1")))
static void copiar_streaming(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m128i a = _mm_stream_load_si128((__m128i *)(src + i));
        __m128i b = _mm_stream_load_si128((__m128i *)(src + i + 16));
        __m128i c = _mm_stream_load_si128((__m128i *)(src + i + 32));
        __m128i d = _mm_stream_load_si128((__m128i *)(src + i + 48));
        _mm_store_si128((__m128i *)(dst + i), a);
        _mm_store_si128((__m128i *)(dst + i + 16), b);
        _mm_store_si128((__m128i *)(dst + i + 32), c);
        _mm_store_si128((__m128i *)(dst + i + 48), d);
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
}

static void copiar_rapido(uint8_t *dst, const uint8_t *src, size_t n)
{
    static int sse41 = -1;
    if (sse41 < 0) sse41 = __builtin_cpu_supports("sse4.1");
    if (sse41 && !((uintptr_t)dst & 15) && !((uintptr_t)src & 15)) copiar_streaming(dst, src, n);
    else memcpy(dst, src, n);
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
    const uint8_t *base = d->data;
    mapa_t *mapa = b->user_data;
    int sincro = 0;
    if (!base && mapa) {
        base = (const uint8_t *)mapa->p + d->mapoffset;
        struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        sincro = ioctl((int)d->fd, DMA_BUF_IOCTL_SYNC, &s) == 0;   /* espera a que la gráfica acabe */
    }
    if (base && d->chunk && (d->chunk->size > 0 || mapa) && w > 0 && h > 0) {
        int stride = d->chunk->stride ? d->chunk->stride : w * 4;
        const uint8_t *src = base + d->chunk->offset;
        size_t fila = (size_t)w * 4, need = fila * h;
        if ((size_t)stride * (h - 1) + fila <= d->maxsize) {
            /* copiar fuera del cerrojo, en el búfer de atrás */
            if (trasero_cap < need) {
                free(trasero);
                trasero = aligned_alloc(64, (need + 63) & ~(size_t)63);
                trasero_cap = trasero ? need : 0;
                if (!trasero) { pw_stream_queue_buffer(st, b); return; }
            }
            uint64_t t0 = ahora_us();
            if ((size_t)stride == fila) copiar_rapido(trasero, src, need);
            else for (int y = 0; y < h; y++) copiar_rapido(trasero + fila * y, src + (size_t)stride * y, fila);
            uint64_t t1 = ahora_us();
            if (sincro) {
                struct dma_buf_sync s = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
                ioctl((int)d->fd, DMA_BUF_IOCTL_SYNC, &s);
                sincro = 0;
            }
            pw_stream_queue_buffer(st, b);       /* devolverlo cuanto antes a gamescope */
            b = NULL;

            static unsigned n_seg;
            static uint64_t t_seg;
            n_seg++;
            if (!t_seg) t_seg = t1;
            if (t1 - t_seg >= 1000000) { fps_captura = (unsigned)(n_seg * 1000000.0 / (t1 - t_seg) + 0.5); n_seg = 0; t_seg = t1; }

            pthread_mutex_lock(&mtx);
            uint8_t *x = ultimo; size_t xc = ultimo_cap;
            ultimo = trasero; ultimo_cap = trasero_cap;
            trasero = x; trasero_cap = xc;
            copia_ms = copia_ms ? copia_ms * 0.95 + (t1 - t0) / 1000.0 * 0.05 : (t1 - t0) / 1000.0;
            /* la vía rápida del escritorio solo vale si leer el búfer es rápido: si la gráfica lo
               dejó en su propia memoria, leerlo es lentísimo (decenas de ms). Entonces, la normal. */
            static unsigned lentas;
            if (mapa) {
                if ((t1 - t0) > 5000) lentas++; else if (lentas) lentas--;
                if (lentas > 20 && !dmabuf_mal) {
                    dmabuf_mal = 1;
                    st_fallo = 1;
                    LOG("La vía rápida del escritorio es lenta aquí (%.1f ms por imagen): vuelvo a la normal", (t1 - t0) / 1000.0);
                }
            } else lentas = 0;
            ult_w = w; ult_h = h;
            ult_rgb = formato.format == SPA_VIDEO_FORMAT_RGBx || formato.format == SPA_VIDEO_FORMAT_RGBA;
            ult_seq++;
            ult_t_cap = t0;              /* el retraso se cuenta desde que llegó la captura */
            n_capturados++;
            pthread_cond_broadcast(&cond);
            pthread_mutex_unlock(&mtx);
        }
    }
    if (sincro) {
        struct dma_buf_sync s = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl((int)d->fd, DMA_BUF_IOCTL_SYNC, &s);
    }
    if (b) pw_stream_queue_buffer(st, b);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state s, const char *err)
{
    (void)data; (void)old;
    if (s == PW_STREAM_STATE_ERROR) { LOG("Captura: error: %s", err ? err : "?"); st_fallo = 1; }
    else if (s == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING) st_fallo = 1;
}


static void on_add_buffer(void *data, struct pw_buffer *b)
{
    (void)data;
    struct spa_data *d = &b->buffer->datas[0];
    b->user_data = NULL;
    if (d->type != SPA_DATA_DmaBuf || d->data) return;
    size_t n = (size_t)d->mapoffset + d->maxsize;
    void *p = mmap(NULL, n, PROT_READ, MAP_SHARED, (int)d->fd, 0);
    if (p == MAP_FAILED) { LOG("Captura: no puedo leer el búfer de la gráfica: %s", strerror(errno)); dmabuf_mal = 1; st_fallo = 1; return; }
    mapa_t *m = malloc(sizeof *m);
    m->p = p; m->n = n;
    b->user_data = m;
}

static void on_remove_buffer(void *data, struct pw_buffer *b)
{
    (void)data;
    mapa_t *m = b->user_data;
    if (m) { munmap(m->p, m->n); free(m); b->user_data = NULL; }
}

static const struct pw_stream_events ev_stream = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .param_changed = on_param_changed,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
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

/* dmabuf: ofrecer primero la vía rápida de la gráfica (búfer lineal), y si no, la normal */
static int crear_stream(struct pw_core *core, uint32_t nodo, const char *objetivo, int dmabuf)
{
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", NULL);
    if (objetivo) pw_properties_set(props, PW_KEY_TARGET_OBJECT, objetivo);
    st = pw_stream_new(core, "decklink", props);
    if (!st) return -1;
    st_fallo = 0;
    pw_stream_add_listener(st, &hook_st, &ev_stream, NULL);

    uint8_t b[2048];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(b, sizeof b);
    const struct spa_pod *params[2];
    int np = 0;
    if (dmabuf) {
        struct spa_pod_frame f[2];
        spa_pod_builder_push_object(&bld, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&bld,
            SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
                &SPA_RECTANGLE(1280, 800), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
            0);
        /* solo búferes lineales (DRM_FORMAT_MOD_LINEAR = 0): así se pueden leer tal cual */
        spa_pod_builder_prop(&bld, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(&bld, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&bld, 0);
        spa_pod_builder_long(&bld, 0);
        spa_pod_builder_pop(&bld, &f[1]);
        params[np++] = spa_pod_builder_pop(&bld, &f[0]);
    }
    params[np++] = spa_pod_builder_add_object(&bld,
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
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, np);
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

    /* Decirle al portal quién somos: KDE recuerda el permiso de pantalla por app.
       (Con portales antiguos esto no existe; entonces vale el nombre del servicio.) */
    {
        sd_bus_message *m = NULL, *rep = NULL;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        if (sd_bus_message_new_method_call(bus_portal, &m, "org.freedesktop.portal.Desktop",
                                           "/org/freedesktop/portal/desktop",
                                           "org.freedesktop.host.portal.Registry", "Register") >= 0) {
            sd_bus_message_append(m, "s", "io.github.viclex2.decklink");
            sd_bus_message_open_container(m, 'a', "{sv}");
            sd_bus_message_close_container(m);
            if (sd_bus_call(bus_portal, m, 5 * 1000000ULL, &err, &rep) >= 0) sd_bus_message_unref(rep);
            sd_bus_error_free(&err);
            sd_bus_message_unref(m);
        }
    }

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
        int ok = core_portal && crear_stream(core_portal, r.nodo, NULL, !dmabuf_mal) == 0;
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
    int ok = core_local && crear_stream(core_local, PW_ID_ANY, "gamescope", 0) == 0;
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
/* La captura sigue en marcha 60 s después de desconectar: si la pantalla vuelve
   (app reabierta, cable recolocado) la imagen sale al instante. */
static void *hilo_gestor(void *arg)
{
    (void)arg;
    time_t esperar_hasta = 0, activa_hasta = 0, t_fuente = 0, t_ult_cap = 0;
    unsigned long cap_vista = 0;
    int capturando = 0, viva_antes = 0;
    for (;;) {
        pthread_mutex_lock(&mtx);
        int viva = conexion_viva;
        unsigned long cap = n_capturados;
        pthread_mutex_unlock(&mtx);
        time_t ahora = time(NULL);
        if (cap != cap_vista) { cap_vista = cap; t_ult_cap = ahora; }

        if (viva && !viva_antes) {
            t_ult_cap = ahora;
            /* modo juego: una captura nueva en cada conexión (como en las primeras versiones) */
            if (fuente_actual == FUENTE_JUEGO) detener_fuente();
        }
        viva_antes = viva;
        if (viva) activa_hasta = ahora + 60;

        if (!viva) {
            if (fuente_actual == FUENTE_JUEGO) detener_fuente();
            if (capturando && ahora >= activa_hasta) {
                /* escritorio: en pausa, sin cerrarla, para que el permiso de pantalla siga valiendo */
                pw_thread_loop_lock(pwl);
                if (st) pw_stream_set_active(st, false);
                detener_audio();
                pw_thread_loop_unlock(pwl);
                capturando = 0;
                LOG("Captura en pausa (no hay pantalla conectada)");
            }
            usleep(300000);
            continue;
        }
        if (!capturando) {
            pw_thread_loop_lock(pwl);
            if (st) pw_stream_set_active(st, true);
            pw_thread_loop_unlock(pwl);
            capturando = 1;
        }

        asegurar_core_local();
        pw_thread_loop_lock(pwl);
        iniciar_audio();
        pw_thread_loop_unlock(pwl);
        int quiero = fuente_deseada();

        /* modo juego: gamescope manda imagen sin parar; si deja de llegar, rehacer la captura
           (es lo que antes se arreglaba reiniciando el servicio) */
        if (fuente_actual == FUENTE_JUEGO && ahora - t_ult_cap >= 3 && ahora - t_fuente >= 3) {
            LOG("El modo juego no está dando imagen: reinicio la captura");
            st_fallo = 1;
        }
        /* escritorio: si al empezar no llega ni una imagen, volver a empezar */
        if (fuente_actual == FUENTE_ESCRITORIO && t_ult_cap < t_fuente && ahora - t_fuente >= 5) {
            LOG("El escritorio no da imagen todavía: reinicio la captura%s", dmabuf_mal ? "" : " por la vía normal");
            dmabuf_mal = 1;
            st_fallo = 1;
        }
        if ((quiero != fuente_actual || st_fallo) && ahora >= esperar_hasta) {
            if (fuente_actual != FUENTE_NINGUNA || st_fallo) detener_fuente();
            if (quiero != FUENTE_NINGUNA) {
                LOG("Capturando: %s", nombre_fuente[quiero]);
                int r = quiero == FUENTE_JUEGO ? iniciar_juego() : iniciar_escritorio();
                if (r == 0) { fuente_actual = quiero; t_fuente = time(NULL); }
                else esperar_hasta = time(NULL) + (r == -2 ? 30 : 3);
            }
        }
        usleep(300000);
    }
    return NULL;
}

/* ================================================================== */
/* Codificación                                                        */
/* ================================================================== */
#define MAX_FRANJAS 8
typedef struct {
    tjhandle tj;
    const uint8_t *src;
    int w, h, pf, q, rgb, samp;
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
    unsigned long need = tjBufSize(f->w, f->h, f->samp);
    if (f->cap < need) { tjFree(f->out); f->out = tjAlloc((int)need); f->cap = need; }
    f->tam = f->cap;
    f->err = tjCompress2(f->tj, f->src, f->w, f->w * 4, f->h, f->pf, &f->out, &f->tam,
                         f->samp, f->q, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
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
/* procesa un mensaje de la pantalla; devuelve 1 si la pantalla se despide */
static int procesar_entrada(const dl_input *mm)
{
    dl_input m = *mm;
    if (m.type == DL_ACK) {
        t_ack_us = ahora_us();
        pthread_mutex_lock(&mtx);
        /* ACK con número: confirma ese fotograma y todos los anteriores (si se perdió
           una confirmación, la siguiente lo arregla y la imagen nunca se frena) */
        int quitar = 1;
        if (m.value > 0 && m.value < 256) {
            quitar = 0;
            for (int i = 0; i < envio_n; i++)
                if (envio_id[(envio_ini + i) % (EN_CAMINO_MAX + 2)] == (uint8_t)m.value) { quitar = i + 1; break; }
        }
        for (int k = 0; k < quitar && envio_n > 0; k++) {
            int ultimo = k == quitar - 1;
            if (ultimo) {
                double ms = (double)(t_ack_us - envio_t[envio_ini]) / 1000.0;
                double r = (double)(t_ack_us - envio_cap[envio_ini]) / 1000.0;
                retraso_ms = ciclos_medidos ? retraso_ms * 0.9 + r * 0.1 : r;
                ciclo_medido_ms = ciclos_medidos ? ciclo_medido_ms * 0.9 + ms * 0.1 : ms;
                ciclos_medidos++;
            }
            envio_ini = (envio_ini + 1) % (EN_CAMINO_MAX + 2);
            envio_n--;
        }
        if (m.value > 0 && m.value < 256) en_camino = envio_n;
        else if (en_camino > 0) en_camino--;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
    } else if (m.type == DL_HOLA) {
        pthread_mutex_lock(&mtx);
        cliente_caps = m.code;
        cliente_pin = m.value;
        hola_recibido = 1;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
    } else if (m.type == DL_ADIOS) {
        return 1;
    } else if (m.type == EV_REL) {
        emitir(mouse_fd, EV_REL, m.code, m.value);
    } else if (m.type == EV_ABS && (m.code == ABS_X || m.code == ABS_Y) && abs_fd >= 0) {
        int v = m.value < 0 ? 0 : m.value > DL_ABS_MAX ? DL_ABS_MAX : m.value;
        if (m.code == ABS_X) {          /* X sin sincronizar: llega Y justo detrás */
            struct input_event ev;
            memset(&ev, 0, sizeof ev);
            ev.type = EV_ABS; ev.code = ABS_X; ev.value = v;
            if (write(abs_fd, &ev, sizeof ev) < 0) { /* ignorar */ }
        } else {
            emitir(abs_fd, EV_ABS, ABS_Y, v);
        }
    } else if (m.type == EV_KEY && m.code < KEY_CNT) {
        pulsadas[m.code] = m.value != 0;
        int fd = kbd_fd;
        if (es_boton(m.code)) fd = (cliente_caps & DL_CAP_ABS) && abs_fd >= 0 ? abs_fd : mouse_fd;
        emitir(fd, EV_KEY, m.code, m.value);
    }
    return 0;
}

static int usb_sesion = 0;          /* hay una pantalla conectada por el canal USB directo */

/* despierta al hilo principal si está bloqueado escribiendo en el canal USB */
static void despertar_principal(void)
{
    if (usb_escribiendo_desde) pthread_kill(hilo_principal, SIGUSR1);
}

static void fin_conexion(void)
{
    pthread_mutex_lock(&mtx);
    conexion_viva = 0;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mtx);
    despertar_principal();
}

static void sin_nada(int s) { (void)s; }

/* si la pantalla USB deja de leer (app cerrada de golpe), no dejar la Deck atascada */
static void *hilo_vigia(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        uint64_t d = usb_escribiendo_desde;
        if (d && ahora_us() - d > 4000000u) {
            LOG("La pantalla USB no recoge los datos: cierro la sesión");
            fin_conexion();
        }
    }
    return NULL;
}

/* pantalla conectada por red (cable USB con tarjeta de red, o wifi) */
static void *hilo_entrada(void *arg)
{
    int s = (int)(intptr_t)arg;
    dl_input m;
    while (recv_todo(s, &m, sizeof m) == 0)
        if (procesar_entrada(&m)) break;
    fin_conexion();
    shutdown(s, SHUT_RDWR);
    return NULL;
}

/* ---- canal USB directo (Android): ep1 = Deck -> pantalla, ep2 = pantalla -> Deck ---- */
static int usb_in = -1, usb_out = -1;
static int usb_pendiente = 0;       /* llegó un saludo por USB: empezar (o reiniciar) la sesión */
static int usb_caps = 0;

static void *hilo_usb(void *arg)
{
    (void)arg;
    uint8_t buf[4096];
    size_t lleno = 0;
    int errores = 0;
    for (;;) {
        if (usb_out < 0) {
            int o = open(DL_FFS_DIR "/ep2", O_RDONLY | O_CLOEXEC);
            int i = open(DL_FFS_DIR "/ep1", O_WRONLY | O_CLOEXEC);
            if (o < 0 || i < 0) {
                if (o >= 0) close(o);
                if (i >= 0) close(i);
                sleep(2);
                continue;
            }
            pthread_mutex_lock(&mtx);
            usb_out = o; usb_in = i;
            pthread_mutex_unlock(&mtx);
            lleno = 0;
            errores = 0;
        }
        lleno = 0;          /* cada transferencia USB trae mensajes enteros: nunca se desalinean */
        ssize_t r = read(usb_out, buf, sizeof buf);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            /* cable quitado, o la Deck rehizo el canal (cambio de velocidad) */
            pthread_mutex_lock(&mtx);
            int sesion = usb_sesion;
            pthread_mutex_unlock(&mtx);
            if (sesion) fin_conexion();
            if (++errores >= 3 && !sesion) {
                pthread_mutex_lock(&mtx);
                close(usb_out); close(usb_in);
                usb_out = usb_in = -1;
                pthread_mutex_unlock(&mtx);
            }
            sleep(1);
            continue;
        }
        errores = 0;
        lleno += (size_t)r;
        size_t pos = 0;
        while (lleno - pos >= sizeof(dl_input)) {
            dl_input m;
            memcpy(&m, buf + pos, sizeof m);
            pos += sizeof m;
            if (m.type == DL_HOLA) {
                pthread_mutex_lock(&mtx);
                int reiniciar = usb_sesion;
                if (usb_sesion) conexion_viva = 0;     /* la app se reinició: empezar de nuevo */
                usb_pendiente = 1;
                usb_caps = m.code;
                pthread_cond_broadcast(&cond);
                pthread_mutex_unlock(&mtx);
                if (reiniciar) despertar_principal();
            } else {
                pthread_mutex_lock(&mtx);
                int sesion = usb_sesion;
                pthread_mutex_unlock(&mtx);
                if (sesion && procesar_entrada(&m)) fin_conexion();
            }
        }
        memmove(buf, buf + pos, lleno - pos);
        lleno -= pos;
    }
    return NULL;
}

/* enviar un mensaje completo por la red o por el canal USB */
typedef struct {
    int usb, fd;
    uint8_t *buf;
    size_t cap;
} salida_t;

static int salida_enviar(salida_t *o, const struct iovec *v, int n)
{
    if (!o->usb) {
        for (int i = 0; i < n; i++)
            if (send_todo(o->fd, v[i].iov_base, v[i].iov_len)) return -1;
        return 0;
    }
    /* USB: un solo bloque; si acaba justo en un múltiplo del paquete USB, el móvil se
     * quedaría esperando más datos, así que detrás va un mensaje de relleno corto */
    size_t total = 0;
    for (int i = 0; i < n; i++) total += v[i].iov_len;
    if (o->cap < total) { free(o->buf); o->buf = malloc(total); o->cap = total; }
    size_t p = 0;
    for (int i = 0; i < n; i++) { memcpy(o->buf + p, v[i].iov_base, v[i].iov_len); p += v[i].iov_len; }
    int fd = usb_in;
    if (fd < 0) return -1;
    int res = 0;
    usb_escribiendo_desde = ahora_us();
    for (p = 0; p < total;) {
        size_t k = total - p > 65536 ? 65536 : total - p;
        ssize_t r = write(fd, o->buf + p, k);
        if (r <= 0) {
            if (r < 0 && errno == EINTR && conexion_viva) continue;
            res = -1;
            break;
        }
        p += (size_t)r;
        usb_escribiendo_desde = ahora_us();
    }
    if (!res && total % 512 == 0) {
        dl_frame_hdr z = { DL_MAGIC, 0, 0, DL_FMT_NADA, {0}, 0 };
        if (write(fd, &z, sizeof z) != (ssize_t)sizeof z) res = -1;
    }
    usb_escribiendo_desde = 0;
    return res;
}

/* ¿esta interfaz de red es un cable USB? (las tarjetas de la Deck o el "compartir
 * conexión por USB" de un móvil): por cable no hace falta PIN */
static int interfaz_usb(const char *ifname)
{
    char ruta[300], dest[PATH_MAX];
    if (!strncmp(ifname, "usb", 3)) return 1;
    snprintf(ruta, sizeof ruta, "/sys/class/net/%s/device", ifname);
    if (!realpath(ruta, dest)) return 0;
    return strstr(dest, "/usb") != NULL;
}

static int ip_por_cable(uint32_t ip)
{
    if (ip == inet_addr(DL_IP_DECK) || ip == inet_addr(DL_IP_DECK2)) return 1;
    struct ifaddrs *lista, *i;
    int si = 0;
    if (getifaddrs(&lista)) return 0;
    for (i = lista; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == ip) { si = interfaz_usb(i->ifa_name); break; }
    freeifaddrs(lista);
    return si;
}

/* avisa cada segundo por todas las redes de que la Deck está aquí */
static const char *dir_conf_global;
static void *hilo_baliza(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int uno = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &uno, sizeof uno);
    char nombre[64] = "steamdeck";
    gethostname(nombre, sizeof nombre - 1);
    for (;;) {
        int32_t pin;
        int wifi = wifi_pin(dir_conf_global, &pin);
        char msg[128];
        int n = snprintf(msg, sizeof msg, "%s %d %d %s", DL_BALIZA, DL_PUERTO, wifi, nombre);
        struct ifaddrs *lista, *i;
        if (getifaddrs(&lista) == 0) {
            for (i = lista; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
                if (!(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_broadaddr) continue;
                struct sockaddr_in d = *(struct sockaddr_in *)i->ifa_broadaddr;
                d.sin_port = htons(DL_PUERTO_BALIZA);
                /* por cable siempre se acepta la conexión (sin PIN) */
                if (interfaz_usb(i->ifa_name)) {
                    char m2[128];
                    int n2 = snprintf(m2, sizeof m2, "%s %d 1 %s", DL_BALIZA, DL_PUERTO, nombre);
                    sendto(s, m2, (size_t)n2, 0, (struct sockaddr *)&d, sizeof d);
                } else {
                    sendto(s, msg, (size_t)n, 0, (struct sockaddr *)&d, sizeof d);
                }
            }
            freeifaddrs(lista);
        }
        sleep(1);
    }
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
        if (!strcmp(argv[i], "--red")) return modo_red(i + 1 < argc ? argv[i + 1] : NULL);
        if (!strcmp(argv[i], "--dhcp-prueba") && i + 1 < argc) {
            signal(SIGTERM, al_terminar);
            red_t r = { "prueba", "", "", DL_IP_DECK, DL_IP_PC, "" };
            snprintf(r.ifname, sizeof r.ifname, "%s", argv[i + 1]);
            return servidor_dhcp(&r, 1);
        }
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
    abs_fd   = crear_uinput_abs();
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
    dir_conf_global = dir;
    pthread_t th_baliza;
    pthread_create(&th_baliza, NULL, hilo_baliza, NULL);
    {
        int32_t pin;
        if (wifi_pin(dir, &pin)) LOG("Conexión por wifi ACTIVADA (con PIN)");
        else LOG("Conexión por wifi desactivada (actívala desde el icono DeckLink)");
    }
    LOG("DeckLink listo. Esperando a la pantalla (portátil, PC o Android)...");

    uint8_t *copia = NULL;
    size_t copia_cap = 0;
    salida_t sal = { 0, -1, NULL, 0 };

    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
    hilo_principal = pthread_self();
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = sin_nada;            /* sin SA_RESTART: interrumpe una escritura USB atascada */
        sigaction(SIGUSR1, &sa, NULL);
    }
    pthread_t th_usb, th_vigia;
    pthread_create(&th_usb, NULL, hilo_usb, NULL);
    pthread_create(&th_vigia, NULL, hilo_vigia, NULL);
    pthread_t th_gestor;
    pthread_create(&th_gestor, NULL, hilo_gestor, NULL);

    for (;;) {
        /* esperar una pantalla: por red (accept) o por el canal USB directo (saludo) */
        int c = -1, es_usb = 0;
        for (;;) {
            pthread_mutex_lock(&mtx);
            if (usb_pendiente) { usb_pendiente = 0; es_usb = 1; }
            pthread_mutex_unlock(&mtx);
            if (es_usb) break;
            struct pollfd pf = { ls, POLLIN, 0 };
            if (poll(&pf, 1, 200) > 0 && (c = accept4(ls, NULL, NULL, SOCK_CLOEXEC)) >= 0) break;
        }
        struct sockaddr_in local, remoto;
        memset(&local, 0, sizeof local);
        memset(&remoto, 0, sizeof remoto);
        int por_usb = 1;
        if (!es_usb) {
            fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
            socklen_t ll = sizeof local, lr = sizeof remoto;
            getsockname(c, (struct sockaddr *)&local, &ll);
            getpeername(c, (struct sockaddr *)&remoto, &lr);
            por_usb = ip_por_cable(local.sin_addr.s_addr);
        }
        sal.usb = es_usb;
        sal.fd = c;

        pthread_mutex_lock(&mtx);
        en_camino = 0;
        envio_n = 0; envio_ini = 0; ciclos_medidos = 0; ciclo_medido_ms = 0;
        audio_len = 0;
        hola_recibido = es_usb;
        cliente_caps = es_usb ? usb_caps : DL_CAP_LZ4;   /* la versión 1 de Linux no saluda */
        cliente_pin = -1;
        conexion_viva = 1;
        if (es_usb) usb_sesion = 1;
        /* si ya hay una imagen capturada, se manda enseguida (aunque la pantalla esté quieta) */
        uint64_t enviado = ult_w ? ult_seq - 1 : ult_seq;
        pthread_mutex_unlock(&mtx);

        pthread_t th_in = 0;
        if (!es_usb) pthread_create(&th_in, NULL, hilo_entrada, (void *)(intptr_t)c);

        /* esperar el saludo del cliente (capacidades y PIN) */
        if (!es_usb) {
            uint64_t limite = ahora_us() + (por_usb ? 300000u : 2000000u);
            pthread_mutex_lock(&mtx);
            while (conexion_viva && !hola_recibido && ahora_us() < limite) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50000000;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
                pthread_cond_timedwait(&cond, &mtx, &ts);
            }
            pthread_mutex_unlock(&mtx);
        }
        if (!por_usb) {
            int32_t pin;
            int wifi = wifi_pin(dir, &pin);
            int motivo = !wifi ? DL_RECHAZO_WIFI_OFF : (cliente_pin != pin ? DL_RECHAZO_PIN : 0);
            if (motivo) {
                LOG("Conexión por wifi de %s rechazada: %s", inet_ntoa(remoto.sin_addr),
                    motivo == DL_RECHAZO_WIFI_OFF ? "el wifi está desactivado" : "PIN incorrecto o sin PIN");
                dl_frame_hdr hr = { DL_MAGIC, (uint16_t)motivo, 0, DL_FMT_RECHAZO, {0}, 0 };
                send_todo(c, &hr, sizeof hr);
                usleep(200000);
                shutdown(c, SHUT_RDWR);
                pthread_join(th_in, NULL);
                close(c);
                continue;
            }
        }
        int tarjeta_deck = !es_usb && (local.sin_addr.s_addr == inet_addr(DL_IP_DECK) ||
                                       local.sin_addr.s_addr == inet_addr(DL_IP_DECK2));
        int usb2 = tarjeta_deck || es_usb ? elegir_modo(dir, forzado) : 1;
        int jpeg = usb2 || !(cliente_caps & DL_CAP_LZ4);
        LOG("Pantalla conectada por %s %s (%s)",
            es_usb ? "el canal USB directo" : por_usb ? "cable USB desde" : "wifi desde",
            es_usb ? "" : inet_ntoa(remoto.sin_addr),
            jpeg ? "JPEG con calidad automática" : "color completo sin pérdidas");

        int calidad = por_usb ? 80 : 75;
        size_t tope = por_usb ? 450000 : 200000;     /* tamaño máximo de un fotograma JPEG */
        uint64_t t_envio_us = 0;
        double ciclo_ms = 0;
        int muestras = 0;
        unsigned long n_enviados = 0, cap0 = n_capturados;
        unsigned fps_envio = 0, n_seg = 0;
        uint64_t t_seg = ahora_us();
        time_t t_est = time(NULL);
        uint8_t audio_envio[AUDIO_CAP];
        for (;;) {
            pthread_mutex_lock(&mtx);
            while (conexion_viva && audio_len == 0 && (en_camino >= EN_CAMINO_MAX || ult_seq == enviado)) {
                /* una confirmación perdida no puede dejar la imagen parada */
                uint64_t ult = t_ack_us > t_envio_us ? t_ack_us : t_envio_us;
                if (en_camino > 0 && t_envio_us && ahora_us() - ult > 600000) {
                    en_camino = 0;
                    envio_n = 0;
                    if (ult_seq != enviado) break;
                }
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 250000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&cond, &mtx, &ts);
            }
            if (time(NULL) - t_est >= 30) {
                LOG("Últimos 30 s: %lu fotogramas enviados (%.0f fps), capturados %.0f fps, copia %.1f ms%s",
                    n_enviados, n_enviados / 30.0, (n_capturados - cap0) / 30.0, copia_ms,
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
                ha.reservado[0] = (uint8_t)(fps_envio > 255 ? 255 : fps_envio);   /* para los datos en pantalla */
                ha.reservado[1] = (uint8_t)(fps_captura > 255 ? 255 : fps_captura);
                ha.reservado[2] = (uint8_t)(copia_ms * 10 > 255 ? 255 : copia_ms * 10);   /* décimas de ms */
                struct iovec va[2] = { { &ha, sizeof ha }, { audio_envio, n } };
                if (salida_enviar(&sal, va, 2)) break;
                continue;
            }

            /* cuánto tardan los fotogramas en llegar confirmados (con dos en camino) */
            if (ciclos_medidos != (unsigned long)muestras) {
                ciclo_ms = ciclo_medido_ms;
                muestras = (int)ciclos_medidos;
                if (!jpeg && muestras > 30 && ciclo_ms > 14) {
                    LOG("La conexión no da para color sin pérdidas (%.0f ms por fotograma): paso a JPEG", ciclo_ms);
                    jpeg = 1;
                }
            }

            int w = ult_w, h = ult_h, rgb = ult_rgb;
            size_t n = (size_t)w * h;
            if (copia_cap < n * 4) { free(copia); copia = malloc(n * 4); copia_cap = n * 4; }
            memcpy(copia, ultimo, n * 4);
            uint64_t t_cap = ult_t_cap;
            enviado = ult_seq;
            en_camino++;
            /* apuntar el fotograma ya (antes de comprimirlo), con su número */
            int hueco = -1;
            uint8_t id_frame = sig_id;
            sig_id = sig_id == 255 ? 1 : sig_id + 1;
            if (envio_n < EN_CAMINO_MAX + 2) {
                hueco = (envio_ini + envio_n) % (EN_CAMINO_MAX + 2);
                envio_t[hueco] = ahora_us();
                envio_cap[hueco] = t_cap;
                envio_id[hueco] = id_frame;
                envio_n++;
            }
            pthread_mutex_unlock(&mtx);

            /* 8 franjas en paralelo (8 hilos en la Deck); con DL_CAP_FRANJAS cada una sale
               en cuanto está comprimida y la pantalla la descomprime mientras llegan las demás */
            int por_franjas = jpeg && (cliente_caps & DL_CAP_FRANJAS);
            int nf = h >= 64 ? MAX_FRANJAS : 1;
            pthread_t th[MAX_FRANJAS];
            for (int i = 0; i < nf; i++) {
                franja_t *f = &franjas[i];
                f->src = copia + (size_t)dl_franja_y0(i, nf, h) * w * 4;
                f->w = w; f->h = dl_franja_h(i, nf, h);
                f->pf = rgb ? TJPF_RGBX : TJPF_BGRX;
                f->rgb = rgb;
                f->q = calidad;
                f->samp = TJSAMP_420;     /* la mitad de datos y más rápido: menos retraso */
                pthread_create(&th[i], NULL, jpeg ? comprimir_franja : lz4_franja, f);
            }
            unsigned r_ms = retraso_ms > 0 ? (unsigned)(retraso_ms + 0.5) : 0;
            if (r_ms > 65535) r_ms = 65535;
            uint32_t total = 0;
            int err = 0;
            if (por_franjas) {
                for (int i = 0; i < nf; i++) {
                    pthread_join(th[i], NULL);
                    if (err) continue;                       /* recoger los hilos que quedan */
                    uint32_t t = franjas[i].err ? 0 : (uint32_t)franjas[i].tam;
                    uint8_t pre[2] = { (uint8_t)i, (uint8_t)nf };
                    dl_frame_hdr hf = { DL_MAGIC, (uint16_t)w, (uint16_t)h, DL_FMT_FRANJA,
                                        { (uint8_t)r_ms, (uint8_t)(r_ms >> 8), id_frame }, 2 + t };
                    if (i == 0) {
                        t_envio_us = ahora_us();
                        pthread_mutex_lock(&mtx);
                        if (hueco >= 0 && envio_id[hueco] == id_frame) envio_t[hueco] = t_envio_us;
                        pthread_mutex_unlock(&mtx);
                    }
                    struct iovec vs[3] = { { &hf, sizeof hf }, { pre, 2 }, { franjas[i].out, t } };
                    if (salida_enviar(&sal, vs, t ? 3 : 2)) err = 1;
                    total += t;
                }
            } else {
                uint8_t cab[1 + 4 * MAX_FRANJAS];
                total = 1 + 4 * nf;
                cab[0] = (uint8_t)nf;
                for (int i = 0; i < nf; i++) {
                    pthread_join(th[i], NULL);
                    uint32_t t = franjas[i].err ? 0 : (uint32_t)franjas[i].tam;
                    memcpy(cab + 1 + 4 * i, &t, 4);
                    total += t;
                }
                dl_frame_hdr hd = { DL_MAGIC, (uint16_t)w, (uint16_t)h, jpeg ? DL_FMT_JPEG : DL_FMT_LZ4,
                                    { (uint8_t)r_ms, (uint8_t)(r_ms >> 8), id_frame }, total };
                t_envio_us = ahora_us();
                pthread_mutex_lock(&mtx);
                if (hueco >= 0 && envio_id[hueco] == id_frame) envio_t[hueco] = t_envio_us;   /* desde que sale */
                pthread_mutex_unlock(&mtx);
                struct iovec vf[2 + MAX_FRANJAS];
                int nv = 0;
                vf[nv++] = (struct iovec){ &hd, sizeof hd };
                vf[nv++] = (struct iovec){ cab, (size_t)(1 + 4 * nf) };
                for (int i = 0; i < nf; i++)
                    if (!franjas[i].err) vf[nv++] = (struct iovec){ franjas[i].out, franjas[i].tam };
                err = salida_enviar(&sal, vf, nv);
            }
            if (jpeg) {
                /* la mejor calidad con la que cada fotograma cruza el cable en menos de ~10 ms:
                   así el cable nunca va lleno y ningún fotograma espera en cola (sin retraso extra) */
                /* por cable: cada fotograma cruza en ~6 ms; por wifi hay más ida y vuelta */
                double alto = por_usb ? 7 : 28, bajo = por_usb ? 4.5 : 16;
                int minima = por_usb ? 55 : 40;
                if ((ciclo_ms > alto || total > tope) && calidad > minima) calidad -= 2;
                else if (ciclo_ms < bajo && total < tope * 7 / 10 && calidad < 95) calidad += 1;
            }
            if (err) break;
            n_enviados++;
            n_seg++;
            if (ahora_us() - t_seg >= 1000000) {
                fps_envio = (unsigned)(n_seg * 1000000.0 / (ahora_us() - t_seg) + 0.5);
                n_seg = 0;
                t_seg = ahora_us();
            }
        }

        if (!es_usb) {
            shutdown(c, SHUT_RDWR);
            pthread_join(th_in, NULL);
            close(c);
        } else {
            pthread_mutex_lock(&mtx);
            usb_sesion = 0;
            pthread_mutex_unlock(&mtx);
        }
        fin_conexion();
        soltar_todo();
        LOG("Pantalla desconectada");
    }
}
