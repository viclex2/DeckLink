/*
 * Lectura de búferes DMA-BUF con la gráfica (para el modo escritorio).
 *
 * KDE solo puede dar la pantalla de dos formas:
 *  - en memoria normal: la copia él mismo con glGetTexImage y se queda parado
 *    esperando a la gráfica en cada imagen (por eso no pasaba de ~50 fps);
 *  - en un búfer de la gráfica (DMA-BUF): lo rellena la gráfica sin parar a KDE.
 * Leer ese búfer con el procesador es lentísimo (está en la memoria de la gráfica),
 * así que lo importamos en OpenGL ES y le pedimos a la gráfica que lo copie a un
 * búfer en memoria normal (PBO), que luego se lee rápido.
 *
 * EGL y GLES se cargan con dlopen: si no están, se usa la vía normal.
 * Se incluye desde decklink-deck.c.
 */
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>

/* lo que hace falta de GLES 3 (sin depender de sus cabeceras) */
#define GL_PIXEL_PACK_BUFFER_     0x88EB
#define GL_STREAM_READ_           0x88E1
#define GL_MAP_READ_BIT_          0x0001
#define GL_SYNC_GPU_COMMANDS_COMPLETE_ 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT_    0x00000001
typedef struct __GLsync *GLsync_;

static struct {
    int listo, fallo;
    EGLDisplay dpy;
    EGLContext ctx;
    int actual;                       /* contexto activo en el hilo de PipeWire */
    uint64_t mods[64];
    int n_mods;
    GLuint pbo;
    size_t pbo_tam;

    PFNEGLGETPROCADDRESSPROC GetProcAddress;
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    const char *(*QueryString)(EGLDisplay, EGLint);
    PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplay;
    PFNEGLCREATEIMAGEKHRPROC CreateImage;
    PFNEGLDESTROYIMAGEKHRPROC DestroyImage;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC QueryModifiers;

    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*PixelStorei)(GLenum, GLint);
    GLenum (*GetError)(void);
    void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean (*UnmapBuffer)(GLenum);
    GLsync_ (*FenceSync)(GLenum, GLbitfield);
    GLenum (*ClientWaitSync)(GLsync_, GLbitfield, uint64_t);
    void (*DeleteSync)(GLsync_);
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTexture2D;
} gpu;

typedef struct {
    EGLImageKHR img;
    GLuint tex, fbo;
} gpu_buf_t;

/* Prepara EGL (sin ventana) y averigua qué búferes sabe importar. 0 = bien. */
static int gpu_preparar(void)
{
    if (gpu.listo) return 0;
    if (gpu.fallo) return -1;
    gpu.fallo = 1;
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gl) { LOG("Escritorio: no hay EGL/GLES (%s)", dlerror()); return -1; }
    gpu.GetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
    if (!gpu.GetProcAddress) return -1;
#define E(campo, nombre) gpu.campo = (void *)gpu.GetProcAddress(nombre); if (!gpu.campo) { LOG("Escritorio: falta %s", nombre); return -1; }
#define G(campo, nombre) gpu.campo = (void *)dlsym(gl, nombre); if (!gpu.campo) gpu.campo = (void *)gpu.GetProcAddress(nombre); \
                         if (!gpu.campo) { LOG("Escritorio: falta %s", nombre); return -1; }
    E(Initialize, "eglInitialize");
    E(BindAPI, "eglBindAPI");
    E(CreateContext, "eglCreateContext");
    E(MakeCurrent, "eglMakeCurrent");
    E(QueryString, "eglQueryString");
    E(GetPlatformDisplay, "eglGetPlatformDisplayEXT");
    E(CreateImage, "eglCreateImageKHR");
    E(DestroyImage, "eglDestroyImageKHR");
    gpu.QueryModifiers = (void *)gpu.GetProcAddress("eglQueryDmaBufModifiersEXT");
    G(GenTextures, "glGenTextures");
    G(DeleteTextures, "glDeleteTextures");
    G(BindTexture, "glBindTexture");
    G(GenFramebuffers, "glGenFramebuffers");
    G(DeleteFramebuffers, "glDeleteFramebuffers");
    G(BindFramebuffer, "glBindFramebuffer");
    G(FramebufferTexture2D, "glFramebufferTexture2D");
    G(CheckFramebufferStatus, "glCheckFramebufferStatus");
    G(GenBuffers, "glGenBuffers");
    G(BindBuffer, "glBindBuffer");
    G(BufferData, "glBufferData");
    G(ReadPixels, "glReadPixels");
    G(PixelStorei, "glPixelStorei");
    G(GetError, "glGetError");
    G(MapBufferRange, "glMapBufferRange");
    G(UnmapBuffer, "glUnmapBuffer");
    G(FenceSync, "glFenceSync");
    G(ClientWaitSync, "glClientWaitSync");
    G(DeleteSync, "glDeleteSync");
    G(ImageTargetTexture2D, "glEGLImageTargetTexture2DOES");
#undef E
#undef G

    /* pantalla "sin superficie" de Mesa: no necesita ventana ni compositor */
    gpu.dpy = gpu.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint ma, mi;
    if (gpu.dpy == EGL_NO_DISPLAY || !gpu.Initialize(gpu.dpy, &ma, &mi)) { LOG("Escritorio: EGL no arranca"); return -1; }
    const char *ext = gpu.QueryString(gpu.dpy, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import") || !strstr(ext, "EGL_KHR_surfaceless_context")) {
        LOG("Escritorio: la gráfica no importa DMA-BUF");
        return -1;
    }
    if (!gpu.BindAPI(EGL_OPENGL_ES_API)) return -1;
    const EGLint atr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    gpu.ctx = gpu.CreateContext(gpu.dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, atr);
    if (gpu.ctx == EGL_NO_CONTEXT) { LOG("Escritorio: no se crea el contexto GLES 3"); return -1; }

    /* formatos que la gráfica sabe leer: lineal siempre, y los suyos (con compresión, etc.) */
    gpu.n_mods = 0;
    gpu.mods[gpu.n_mods++] = DRM_FORMAT_MOD_LINEAR;
    if (gpu.QueryModifiers && strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers")) {
        EGLuint64KHR m[63];
        EGLBoolean solo_ext[63];
        EGLint n = 0;
        if (gpu.QueryModifiers(gpu.dpy, DRM_FORMAT_ARGB8888, 63, m, solo_ext, &n))
            for (int i = 0; i < n && gpu.n_mods < 64; i++)
                if (!solo_ext[i] && m[i] != DRM_FORMAT_MOD_LINEAR && m[i] != DRM_FORMAT_MOD_INVALID)
                    gpu.mods[gpu.n_mods++] = m[i];
    }
    gpu.listo = 1;
    gpu.fallo = 0;
    LOG("Escritorio: la gráfica leerá la imagen (%d formatos de búfer)", gpu.n_mods);
    return 0;
}

/* en el hilo de PipeWire (el que llama a add_buffer/process) */
static int gpu_activar(void)
{
    if (!gpu.listo) return -1;
    if (!gpu.actual) {
        if (!gpu.MakeCurrent(gpu.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu.ctx)) return -1;
        gpu.actual = 1;
    }
    return 0;
}

static uint32_t drm_de_spa(uint32_t f)
{
    switch (f) {
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    default: return 0;
    }
}

/* importa un búfer DMA-BUF de PipeWire (todas sus capas) */
static gpu_buf_t *gpu_importar(struct spa_buffer *sb, int w, int h, uint32_t formato_spa, uint64_t mod)
{
    if (gpu_activar()) return NULL;
    uint32_t fourcc = drm_de_spa(formato_spa);
    if (!fourcc || sb->n_datas < 1) return NULL;
    EGLint a[64];
    int n = 0;
    a[n++] = EGL_WIDTH; a[n++] = w;
    a[n++] = EGL_HEIGHT; a[n++] = h;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT; a[n++] = (EGLint)fourcc;
    static const EGLint FD[4] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint OF[4] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint PI[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint ML[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint MH[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
    for (uint32_t i = 0; i < sb->n_datas && i < 4; i++) {
        struct spa_data *d = &sb->datas[i];
        if (d->type != SPA_DATA_DmaBuf || d->fd < 0) return NULL;
        a[n++] = FD[i]; a[n++] = (EGLint)d->fd;
        a[n++] = OF[i]; a[n++] = (EGLint)(d->chunk ? d->chunk->offset : 0);
        a[n++] = PI[i]; a[n++] = (EGLint)(d->chunk && d->chunk->stride ? d->chunk->stride : w * 4);
        if (mod != DRM_FORMAT_MOD_INVALID) {
            a[n++] = ML[i]; a[n++] = (EGLint)(mod & 0xFFFFFFFF);
            a[n++] = MH[i]; a[n++] = (EGLint)(mod >> 32);
        }
    }
    a[n++] = EGL_NONE;
    EGLImageKHR img = gpu.CreateImage(gpu.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (img == EGL_NO_IMAGE_KHR) { LOG("Escritorio: la gráfica no acepta el búfer de KDE"); return NULL; }
    gpu_buf_t *g = calloc(1, sizeof *g);
    g->img = img;
    gpu.GenTextures(1, &g->tex);
    gpu.BindTexture(GL_TEXTURE_2D, g->tex);
    gpu.ImageTargetTexture2D(GL_TEXTURE_2D, (GLeglImageOES)img);
    gpu.GenFramebuffers(1, &g->fbo);
    gpu.BindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    gpu.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex, 0);
    if (gpu.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("Escritorio: no se puede leer el búfer importado");
        gpu.DeleteFramebuffers(1, &g->fbo);
        gpu.DeleteTextures(1, &g->tex);
        gpu.DestroyImage(gpu.dpy, img);
        free(g);
        return NULL;
    }
    return g;
}

static void gpu_soltar(gpu_buf_t *g)
{
    if (!g) return;
    if (gpu_activar() == 0) {
        gpu.DeleteFramebuffers(1, &g->fbo);
        gpu.DeleteTextures(1, &g->tex);
        gpu.DestroyImage(gpu.dpy, g->img);
    }
    free(g);
}

/* La gráfica copia la imagen a memoria normal y de ahí a dst (RGBA, w*4 por fila). 0 = bien. */
static int gpu_leer(gpu_buf_t *g, int w, int h, uint8_t *dst)
{
    if (gpu_activar()) return -1;
    size_t n = (size_t)w * h * 4;
    if (!gpu.pbo) gpu.GenBuffers(1, &gpu.pbo);
    gpu.BindBuffer(GL_PIXEL_PACK_BUFFER_, gpu.pbo);
    if (gpu.pbo_tam != n) {
        gpu.BufferData(GL_PIXEL_PACK_BUFFER_, (GLsizeiptr)n, NULL, GL_STREAM_READ_);
        gpu.pbo_tam = n;
    }
    gpu.BindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    gpu.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gpu.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    GLsync_ s = gpu.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0);
    if (s) {
        gpu.ClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT_, 100000000ULL);   /* hasta 100 ms */
        gpu.DeleteSync(s);
    }
    const uint8_t *p = gpu.MapBufferRange(GL_PIXEL_PACK_BUFFER_, 0, (GLsizeiptr)n, GL_MAP_READ_BIT_);
    int r = -1;
    if (p) {
        copiar_rapido(dst, p, n);
        gpu.UnmapBuffer(GL_PIXEL_PACK_BUFFER_);
        r = 0;
    }
    gpu.BindBuffer(GL_PIXEL_PACK_BUFFER_, 0);
    if (gpu.GetError() != GL_NO_ERROR && r == 0) r = 0;   /* errores previos sin importancia */
    return r;
}
