/*
 * Lienzo: todo lo que pinta el programa de PC pasa por aquí.
 *
 * Con OpenGL (lo normal) hay escalado FSR 1 de AMD (EASU + RCAS): la imagen de la Deck
 * (1280x800) se agranda a la pantalla con bordes nítidos. Si OpenGL no está, se usa
 * SDL_Renderer como antes (sin FSR).
 *
 * Coordenadas de dibujo: con lz_logico(W, H) se dibuja en un lienzo virtual de WxH centrado
 * con bandas (como SDL_RenderSetLogicalSize); con lz_logico(0, 0), en píxeles de la ventana.
 *
 * FSR 1: https://github.com/GPUOpen-Effects/FidelityFX-FSR (licencia MIT, © AMD).
 * Se incluye desde decklink-portatil.c.
 */
#include <SDL2/SDL_opengl.h>

enum { MODO_AJUSTAR = 0, MODO_ESTIRAR, MODO_RELLENAR };

typedef struct {
    SDL_Texture *sdl;
    GLuint gl;
    int w, h;
} lz_tex;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;          /* si no hay OpenGL */
    SDL_GLContext ctx;
    int gl;
    int W, H;                   /* ventana, en píxeles */
    float ox, oy, esc;          /* lienzo lógico -> ventana */
    float cr, cg, cb, ca;       /* color actual (0..1) */
    GLuint p_ui, p_easu, p_rcas, vbo, blanco, fbo, fbo_tex;
    int fbo_w, fbo_h;
    GLint ui_pantalla, ui_color, ui_tex;
    GLint e_con[4], e_origen, e_tex;
    GLint r_tex, r_tam, r_nit;
} lz;

/* --- funciones de OpenGL (se cargan con SDL_GL_GetProcAddress) --- */
#define FUNCIONES_GL(X) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Clear, (GLbitfield)) \
    X(void, Enable, (GLenum)) \
    X(void, Disable, (GLenum)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(GLenum, GetError, (void)) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, GenerateMipmap, (GLenum)) \
    X(void, ActiveTexture, (GLenum))

#define DECLARAR(ret, nombre, args) ret (APIENTRY *nombre) args;
static struct { FUNCIONES_GL(DECLARAR) } gl;   /* gl.Viewport(...) */
#undef DECLARAR

/* ------------------------------ shaders ------------------------------ */

static const char *VS_UI =
    "#version 120\n"
    "attribute vec2 pos; attribute vec2 uv;\n"
    "uniform vec2 pantalla;\n"
    "varying vec2 v;\n"
    "void main(){ v = uv; gl_Position = vec4(pos.x / pantalla.x * 2.0 - 1.0, 1.0 - pos.y / pantalla.y * 2.0, 0.0, 1.0); }\n";

static const char *FS_UI =
    "#version 120\n"
    "uniform sampler2D tex; uniform vec4 color;\n"
    "varying vec2 v;\n"
    "void main(){ gl_FragColor = texture2D(tex, v) * color; }\n";

/* EASU: agranda con bordes nítidos. Escribe en el FBO; ip son píxeles de salida con y hacia abajo. */
static const char *FS_EASU =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 con0, con1, con2, con3;\n"
    "uniform vec2 alto_salida;\n"   /* x = alto del FBO */
    "vec3 C(vec2 p){ return texture2D(tex, p).rgb; }\n"
    "float L(vec3 c){ return c.g + 0.5 * (c.r + c.b); }\n"
    "void Set(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE){\n"
    "  float lenX = max(abs(lD - lC), abs(lC - lB));\n"
    "  float dirX = lD - lB; dir.x += dirX * w;\n"
    "  lenX = clamp(abs(dirX) / max(lenX, 1e-5), 0.0, 1.0); lenX *= lenX; len += lenX * w;\n"
    "  float lenY = max(abs(lE - lC), abs(lC - lA));\n"
    "  float dirY = lE - lA; dir.y += dirY * w;\n"
    "  lenY = clamp(abs(dirY) / max(lenY, 1e-5), 0.0, 1.0); lenY *= lenY; len += lenY * w;\n"
    "}\n"
    "void Tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c){\n"
    "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
    "  wB *= wB; wA *= wA; wB = 1.5625 * wB - 0.5625;\n"
    "  float w = wB * wA; aC += c * w; aW += w;\n"
    "}\n"
    "void main(){\n"
    "  vec2 ip = vec2(floor(gl_FragCoord.x), alto_salida.x - 1.0 - floor(gl_FragCoord.y));\n"
    "  vec2 pp = ip * con0.xy + con0.zw;\n"
    "  vec2 fp = floor(pp); pp -= fp;\n"
    "  vec2 p0 = fp * con1.xy + con1.zw;\n"
    "  vec2 p1 = p0 + con2.xy, p2 = p0 + con2.zw, p3 = p0 + con3.xy;\n"
    "  vec4 o = vec4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;\n"
    "  vec3 bC = C(p0 + o.xw), cC = C(p0 + o.yw);\n"
    "  vec3 iC = C(p1 + o.xw), jC = C(p1 + o.yw), fC = C(p1 + o.yz), eC = C(p1 + o.xz);\n"
    "  vec3 kC = C(p2 + o.xw), lC = C(p2 + o.yw), hC = C(p2 + o.yz), gC = C(p2 + o.xz);\n"
    "  vec3 oC = C(p3 + o.yz), nC = C(p3 + o.xz);\n"
    "  float bL = L(bC), cL = L(cC), iL = L(iC), jL = L(jC), fL = L(fC), eL = L(eC);\n"
    "  float kL = L(kC), lL = L(lC), hL = L(hC), gL = L(gC), oL = L(oC), nL = L(nC);\n"
    "  vec2 dir = vec2(0.0); float len = 0.0;\n"
    "  Set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "  Set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
    "  Set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
    "  Set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "  vec2 dir2 = dir * dir; float dirR = dir2.x + dir2.y;\n"
    "  bool zro = dirR < (1.0 / 32768.0);\n"
    "  dirR = zro ? 1.0 : inversesqrt(dirR);\n"
    "  dir.x = zro ? 1.0 : dir.x;\n"
    "  dir *= dirR;\n"
    "  len = len * 0.5; len *= len;\n"
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
    "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
    "  vec3 aC = vec3(0.0); float aW = 0.0;\n"
    "  Tap(aC, aW, vec2( 0.0,-1.0) - pp, dir, len2, lob, clp, bC);\n"
    "  Tap(aC, aW, vec2( 1.0,-1.0) - pp, dir, len2, lob, clp, cC);\n"
    "  Tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
    "  Tap(aC, aW, vec2( 0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
    "  Tap(aC, aW, vec2( 0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
    "  Tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
    "  Tap(aC, aW, vec2( 1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
    "  Tap(aC, aW, vec2( 2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
    "  Tap(aC, aW, vec2( 2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
    "  Tap(aC, aW, vec2( 1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
    "  Tap(aC, aW, vec2( 1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
    "  Tap(aC, aW, vec2( 0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
    "  gl_FragColor = vec4(min(mx, max(mn, aC / aW)), 1.0);\n"
    "}\n";

/* RCAS: afila sin crear halos. Lee el FBO de EASU y pinta en la ventana. */
static const char *FS_RCAS =
    "#version 120\n"
    "uniform sampler2D tex; uniform vec4 tam; uniform float nit;\n"   /* tam = (ancho, alto, x0, y0 del viewport) */
    "vec3 P(vec2 ip){ return texture2D(tex, (ip + 0.5) / tam.xy).rgb; }\n"
    "void main(){\n"
    "  vec2 ip = floor(gl_FragCoord.xy - tam.zw);\n"
    "  vec3 b = P(ip + vec2(0.0, 1.0)), d = P(ip + vec2(-1.0, 0.0)), e = P(ip);\n"
    "  vec3 f = P(ip + vec2(1.0, 0.0)), h = P(ip + vec2(0.0, -1.0));\n"
    "  float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
    "  float eL = e.b * 0.5 + (e.r * 0.5 + e.g), fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
    "  float hL = h.b * 0.5 + (h.r * 0.5 + h.g);\n"
    "  float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;\n"
    "  float rango = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
    "  nz = clamp(abs(nz) / max(rango, 1e-5), 0.0, 1.0);\n"
    "  nz = -0.5 * nz + 1.0;\n"
    "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "  vec3 hitMin = mn4 / (4.0 * mx4 + 1e-5);\n"
    "  vec3 hitMax = (1.0 - mx4) / (4.0 * mn4 - 4.0 - 1e-5);\n"
    "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * nit;\n"
    "  lobe *= nz;\n"
    "  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
    "  gl_FragColor = vec4((lobe * (b + d + h + f) + e) * rcpL, 1.0);\n"
    "}\n";

static GLuint compilar(const char *vs, const char *fs)
{
    GLuint p = gl.CreateProgram();
    const char *src[2] = { vs, fs };
    GLenum tipo[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    for (int i = 0; i < 2; i++) {
        GLuint s = gl.CreateShader(tipo[i]);
        gl.ShaderSource(s, 1, &src[i], NULL);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            gl.GetShaderInfoLog(s, sizeof log, NULL, log);
            printf("OpenGL: shader con error: %s\n", log);
            return 0;
        }
        gl.AttachShader(p, s);
    }
    gl.BindAttribLocation(p, 0, "pos");
    gl.BindAttribLocation(p, 1, "uv");
    gl.LinkProgram(p);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* Abre la ventana con OpenGL; si no se puede, con SDL_Renderer. 0 = bien. */
static int lz_abrir(const char *titulo)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    lz.win = SDL_CreateWindow(titulo, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                              SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL);
    if (lz.win) lz.ctx = SDL_GL_CreateContext(lz.win);
    int ok = lz.ctx != NULL;
#define CARGAR(ret, nombre, args) if (ok && !(gl.nombre = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #nombre))) { \
        printf("OpenGL: falta gl%s\n", #nombre); ok = 0; }
    FUNCIONES_GL(CARGAR)
#undef CARGAR
    if (ok) {
        lz.p_ui = compilar(VS_UI, FS_UI);
        lz.p_easu = compilar(VS_UI, FS_EASU);
        lz.p_rcas = compilar(VS_UI, FS_RCAS);
        ok = lz.p_ui && lz.p_easu && lz.p_rcas;
    }
    if (ok) {
        lz.gl = 1;
        SDL_GL_SetSwapInterval(0);              /* sin esperar al refresco: menos retraso */
        lz.ui_pantalla = gl.GetUniformLocation(lz.p_ui, "pantalla");
        lz.ui_color = gl.GetUniformLocation(lz.p_ui, "color");
        lz.ui_tex = gl.GetUniformLocation(lz.p_ui, "tex");
        const char *nc[4] = { "con0", "con1", "con2", "con3" };
        for (int i = 0; i < 4; i++) lz.e_con[i] = gl.GetUniformLocation(lz.p_easu, nc[i]);
        lz.e_origen = gl.GetUniformLocation(lz.p_easu, "alto_salida");
        lz.e_tex = gl.GetUniformLocation(lz.p_easu, "tex");
        lz.r_tex = gl.GetUniformLocation(lz.p_rcas, "tex");
        lz.r_tam = gl.GetUniformLocation(lz.p_rcas, "tam");
        lz.r_nit = gl.GetUniformLocation(lz.p_rcas, "nit");
        gl.GenBuffers(1, &lz.vbo);
        gl.GenTextures(1, &lz.blanco);
        gl.BindTexture(GL_TEXTURE_2D, lz.blanco);
        uint32_t b = 0xFFFFFFFF;
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &b);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.Enable(GL_BLEND);
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        printf("Pantalla: OpenGL (%s), escalado FSR disponible\n", (const char *)gl.GetString(GL_RENDERER));
        return 0;
    }
    /* sin OpenGL: como antes */
    if (lz.ctx) { SDL_GL_DeleteContext(lz.ctx); lz.ctx = NULL; }
    if (lz.win) SDL_DestroyWindow(lz.win);
    lz.win = SDL_CreateWindow(titulo, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!lz.win) return -1;
    lz.ren = SDL_CreateRenderer(lz.win, -1, SDL_RENDERER_ACCELERATED);
    if (!lz.ren) lz.ren = SDL_CreateRenderer(lz.win, -1, 0);
    if (!lz.ren) return -1;
    SDL_SetRenderDrawBlendMode(lz.ren, SDL_BLENDMODE_BLEND);
    printf("Pantalla: SDL_Renderer (sin FSR)\n");
    return 0;
}

static void lz_tam_ventana(void)
{
    if (lz.gl) SDL_GL_GetDrawableSize(lz.win, &lz.W, &lz.H);
    else SDL_GetRendererOutputSize(lz.ren, &lz.W, &lz.H);
}

/* textura RGBA (bytes R,G,B,A) o BGRA; lineal = suavizada al escalar */
static lz_tex lz_crear(int w, int h, int bgra, int lineal, int mipmap, const void *px)
{
    lz_tex t = { NULL, 0, w, h };
    if (lz.gl) {
        gl.GenTextures(1, &t.gl);
        gl.BindTexture(GL_TEXTURE_2D, t.gl);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, px);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lineal ? GL_LINEAR : GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? GL_LINEAR_MIPMAP_LINEAR : lineal ? GL_LINEAR : GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (mipmap) gl.GenerateMipmap(GL_TEXTURE_2D);
    } else {
        t.sdl = SDL_CreateTexture(lz.ren, bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888,
                                  px ? SDL_TEXTUREACCESS_STATIC : SDL_TEXTUREACCESS_STREAMING, w, h);
        if (t.sdl) {
            SDL_SetTextureBlendMode(t.sdl, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.sdl, lineal ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            if (px) SDL_UpdateTexture(t.sdl, NULL, px, w * 4);
        }
    }
    return t;
}

static void lz_borrar(lz_tex *t)
{
    if (t->gl) gl.DeleteTextures(1, &t->gl);
    if (t->sdl) SDL_DestroyTexture(t->sdl);
    t->gl = 0; t->sdl = NULL;
}

/* sube un fotograma BGRA (4 bytes por píxel, sin relleno) */
static void lz_subir(lz_tex *t, const void *px)
{
    if (lz.gl) {
        gl.BindTexture(GL_TEXTURE_2D, t->gl);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    } else if (t->sdl) {
        SDL_UpdateTexture(t->sdl, NULL, px, t->w * 4);
    }
}

static void lz_inicio(void)
{
    lz_tam_ventana();
    if (lz.gl) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.Viewport(0, 0, lz.W, lz.H);
        gl.ClearColor(0, 0, 0, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT);
    } else {
        SDL_RenderSetLogicalSize(lz.ren, 0, 0);
        SDL_SetRenderDrawColor(lz.ren, 0, 0, 0, 255);
        SDL_RenderClear(lz.ren);
    }
    lz.ox = lz.oy = 0; lz.esc = 1;
}

static void lz_presentar(void)
{
    if (lz.gl) SDL_GL_SwapWindow(lz.win);
    else SDL_RenderPresent(lz.ren);
}

/* lienzo virtual de w x h centrado (0,0 = píxeles de la ventana) */
static void lz_logico(int w, int h)
{
    if (!w || !h) { lz.ox = lz.oy = 0; lz.esc = 1; return; }
    lz.esc = SDL_min((float)lz.W / w, (float)lz.H / h);
    lz.ox = (lz.W - w * lz.esc) / 2;
    lz.oy = (lz.H - h * lz.esc) / 2;
}

static void lz_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    lz.cr = r / 255.f; lz.cg = g / 255.f; lz.cb = b / 255.f; lz.ca = a / 255.f;
}

/* cuadro texturizado en píxeles de la ventana; uv en 0..1 */
static void gl_cuadro(GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
    float q[16] = { x, y, u0, v0,   x + w, y, u1, v0,   x, y + h, u0, v1,   x + w, y + h, u1, v1 };
    gl.UseProgram(lz.p_ui);
    gl.Uniform2f(lz.ui_pantalla, (float)lz.W, (float)lz.H);
    gl.Uniform4f(lz.ui_color, lz.cr, lz.cg, lz.cb, lz.ca);
    gl.Uniform1i(lz.ui_tex, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.BindBuffer(GL_ARRAY_BUFFER, lz.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void lz_relleno(float x, float y, float w, float h)
{
    x = lz.ox + x * lz.esc; y = lz.oy + y * lz.esc; w *= lz.esc; h *= lz.esc;
    if (lz.gl) { gl_cuadro(lz.blanco, x, y, w, h, 0, 0, 1, 1); return; }
    SDL_SetRenderDrawColor(lz.ren, (Uint8)(lz.cr * 255), (Uint8)(lz.cg * 255), (Uint8)(lz.cb * 255), (Uint8)(lz.ca * 255));
    SDL_FRect r = { x, y, w, h };
    SDL_RenderFillRectF(lz.ren, &r);
}

static void lz_borde(float x, float y, float w, float h, float grosor)
{
    lz_relleno(x, y, w, grosor);
    lz_relleno(x, y + h - grosor, w, grosor);
    lz_relleno(x, y, grosor, h);
    lz_relleno(x + w - grosor, y, grosor, h);
}

/* copia (un trozo de) una textura; el color multiplica (sirve para teñir el texto) */
static void lz_copia(const lz_tex *t, const SDL_Rect *src, float x, float y, float w, float h)
{
    x = lz.ox + x * lz.esc; y = lz.oy + y * lz.esc; w *= lz.esc; h *= lz.esc;
    if (lz.gl) {
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (src) {
            u0 = (float)src->x / t->w; v0 = (float)src->y / t->h;
            u1 = (float)(src->x + src->w) / t->w; v1 = (float)(src->y + src->h) / t->h;
        }
        gl_cuadro(t->gl, x, y, w, h, u0, v0, u1, v1);
        return;
    }
    if (!t->sdl) return;
    SDL_SetTextureColorMod(t->sdl, (Uint8)(lz.cr * 255), (Uint8)(lz.cg * 255), (Uint8)(lz.cb * 255));
    SDL_SetTextureAlphaMod(t->sdl, (Uint8)(lz.ca * 255));
    SDL_FRect d = { x, y, w, h };
    SDL_RenderCopyF(lz.ren, t->sdl, src, &d);
}

/* La imagen de la Deck en la ventana: modo (ajustar/estirar/rellenar) y FSR.
   nitidez: 0 = sin FSR; 1 suave, 2 media, 3 alta. Devuelve el rectángulo usado. */
static SDL_Rect lz_imagen(const lz_tex *t, int modo, int nitidez)
{
    float W = (float)lz.W, H = (float)lz.H, w = (float)t->w, h = (float)t->h;
    float sx = 0, sy = 0, sw = w, sh = h;             /* trozo de la imagen */
    float dx = 0, dy = 0, dw = W, dh = H;             /* dónde va en la ventana */
    if (modo == MODO_AJUSTAR) {
        float e = SDL_min(W / w, H / h);
        dw = SDL_floorf(w * e + 0.5f); dh = SDL_floorf(h * e + 0.5f);
        dx = SDL_floorf((W - dw) / 2); dy = SDL_floorf((H - dh) / 2);
    } else if (modo == MODO_RELLENAR) {
        float e = SDL_max(W / w, H / h);
        sw = W / e; sh = H / e;
        sx = (w - sw) / 2; sy = (h - sh) / 2;
    }
    SDL_Rect r = { (int)dx, (int)dy, (int)dw, (int)dh };
    if (!lz.gl) {
        if (!t->sdl) return r;
        SDL_SetTextureColorMod(t->sdl, 255, 255, 255);
        SDL_SetTextureAlphaMod(t->sdl, 255);
        SDL_Rect s = { (int)sx, (int)sy, (int)sw, (int)sh };
        SDL_FRect d = { dx, dy, dw, dh };
        SDL_RenderCopyF(lz.ren, t->sdl, &s, &d);
        return r;
    }
    int agranda = dw > sw * 1.02f || dh > sh * 1.02f;
    if (!nitidez || !agranda) {
        lz_color(255, 255, 255, 255);
        gl_cuadro(t->gl, dx, dy, dw, dh, sx / w, sy / h, (sx + sw) / w, (sy + sh) / h);
        return r;
    }
    /* 1) EASU: de la imagen al FBO del tamaño de salida */
    int ow = r.w, oh = r.h;
    if (!lz.fbo || lz.fbo_w != ow || lz.fbo_h != oh) {
        if (!lz.fbo) gl.GenFramebuffers(1, &lz.fbo);
        if (lz.fbo_tex) gl.DeleteTextures(1, &lz.fbo_tex);
        gl.GenTextures(1, &lz.fbo_tex);
        gl.BindTexture(GL_TEXTURE_2D, lz.fbo_tex);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.BindFramebuffer(GL_FRAMEBUFFER, lz.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lz.fbo_tex, 0);
        lz.fbo_w = ow; lz.fbo_h = oh;
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            lz.fbo_w = 0;
            lz_color(255, 255, 255, 255);
            gl_cuadro(t->gl, dx, dy, dw, dh, sx / w, sy / h, (sx + sw) / w, (sy + sh) / h);
            return r;
        }
    }
    gl.Disable(GL_BLEND);
    gl.BindFramebuffer(GL_FRAMEBUFFER, lz.fbo);
    gl.Viewport(0, 0, ow, oh);
    gl.UseProgram(lz.p_easu);
    /* FsrEasuCon, con el origen del trozo (modo rellenar) sumado en con0.zw */
    gl.Uniform4f(lz.e_con[0], sw / ow, sh / oh, 0.5f * sw / ow - 0.5f + sx, 0.5f * sh / oh - 0.5f + sy);
    gl.Uniform4f(lz.e_con[1], 1.f / w, 1.f / h, 1.f / w, -1.f / h);
    gl.Uniform4f(lz.e_con[2], -1.f / w, 2.f / h, 1.f / w, 2.f / h);
    gl.Uniform4f(lz.e_con[3], 0, 4.f / h, 0, 0);
    gl.Uniform2f(lz.e_origen, (float)oh, 0);
    gl.Uniform1i(lz.e_tex, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, t->gl);
    GLint filtro_previo = GL_LINEAR;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    (void)filtro_previo;
    float q[16] = { 0, 0, 0, 0,  (float)ow, 0, 0, 0,  0, (float)oh, 0, 0,  (float)ow, (float)oh, 0, 0 };
    /* el vertex shader de la UI pasa de píxeles a pantalla: le damos el tamaño del FBO */
    gl.UseProgram(lz.p_easu);
    GLint loc = gl.GetUniformLocation(lz.p_easu, "pantalla");
    gl.Uniform2f(loc, (float)ow, (float)oh);
    gl.BindBuffer(GL_ARRAY_BUFFER, lz.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* 2) RCAS: del FBO a la ventana */
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    int vy = lz.H - (r.y + oh);                      /* OpenGL cuenta y desde abajo */
    gl.Viewport(r.x, vy, ow, oh);
    gl.UseProgram(lz.p_rcas);
    gl.Uniform1i(lz.r_tex, 0);
    gl.Uniform4f(lz.r_tam, (float)ow, (float)oh, (float)r.x, (float)vy);
    static const float pasos[4] = { 0, 1.0f, 0.5f, 0.2f };        /* en "stops": menos = más nítido */
    gl.Uniform1f(lz.r_nit, SDL_powf(2.f, -pasos[nitidez < 0 ? 0 : nitidez > 3 ? 3 : nitidez]));
    loc = gl.GetUniformLocation(lz.p_rcas, "pantalla");
    gl.Uniform2f(loc, (float)ow, (float)oh);
    gl.BindTexture(GL_TEXTURE_2D, lz.fbo_tex);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    gl.Viewport(0, 0, lz.W, lz.H);
    gl.Enable(GL_BLEND);
    return r;
}
