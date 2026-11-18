/*
 * DeckLink para Android (móvil, tablet y Meta Quest).
 * Hace de pantalla de la Steam Deck: muestra su imagen y reproduce su sonido.
 *
 * Cómo encuentra la Deck, por orden:
 *   1. Canal USB directo: el móvil hace de anfitrión USB y habla con la Deck sin red
 *      (la Deck presenta una interfaz USB propia de DeckLink). Funciona en casi todos.
 *   2. Red por cable: si el móvil usa la Deck como tarjeta de red (10.77/10.79).
 *   3. Wifi o "compartir conexión": balizas UDP de la Deck. Por wifi pide el PIN.
 */
package io.github.viclex2.decklink;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.hardware.usb.UsbConstants;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.opengl.GLUtils;
import android.text.SpannableStringBuilder;
import android.text.style.ForegroundColorSpan;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    /* ---------------- protocolo (igual que src/protocolo.h) ---------------- */
    static final int PUERTO = 47800, PUERTO_BALIZA = 47801;
    static final String IP_USB = "10.77.0.1", IP_USB2 = "10.79.0.1";
    static final int MAGIC = 0x33464C44;
    static final int ACK = 0xFFFF, HOLA = 0xFFFE, ADIOS = 0xFFFD;
    static final int FMT_JPEG = 1, FMT_AUDIO = 2, FMT_LZ4 = 3, FMT_RECHAZO = 4, FMT_NADA = 5;
    static final int RECHAZO_WIFI_OFF = 1, RECHAZO_PIN = 2;
    static final int EV_KEY = 1;
    static final int AUDIO_HZ = 48000;
    static final int USB_VID = 0x1d6b, USB_PID = 0x0104;
    static final int CAP_420 = 4;           /* JPEG 4:2:0: menos datos y se descomprime antes */
    static final int CAP_FRANJAS = 8, FMT_FRANJA = 6;   /* franjas sueltas: se descomprimen según llegan */

    static final String TEXTO_BUSCANDO = "Buscando la Steam Deck\n\nSe conectará sola en cuanto la encuentre.";

    /* colores de la interfaz (oscuro, con el azul de acento de la consola) */
    static final int C_FONDO_A = 0xFF1A222E, C_FONDO_B = 0xFF0B0E13, C_BARRA = 0xFF080A0E, C_TARJETA = 0xFF1D232C,
            C_BORDE = 0xFF2F3743, C_TEXTO = 0xFFEBEFF4, C_GRIS = 0xFF8B929A, C_AZUL = 0xFF1A9FFF,
            C_VERDE = 0xFF78DC6E, C_AMARILLO = 0xFFF3BE46, C_ROJO = 0xFFE85A50, C_TECLA = 0xFF3A424E;

    /* ---------------- interfaz ---------------- */
    private volatile SurfaceHolder superficie;
    private LinearLayout capa, pasosVista;
    private TextView texto, tituloVista;
    private android.widget.ProgressBar girando;
    private LinearLayout datosCaja;
    private SharedPreferences prefs;

    /* ---------------- conexión ---------------- */
    private volatile boolean activo;
    private Thread hiloRed, hiloAudio, hiloEnvio;
    private volatile Conexion actual;
    private volatile DatagramSocket balizas;
    private final Object cerrojoSalida = new Object();
    private final ArrayBlockingQueue<Integer> pinListo = new ArrayBlockingQueue<>(1);
    private final LinkedBlockingQueue<byte[]> colaEnvio = new LinkedBlockingQueue<>();
    private String ignorarIp = "";
    private long ignorarHasta;
    private int usbPermisoPedido = -1;      /* id del dispositivo USB al que ya se pidió permiso */

    /* ---------------- imagen: la red recibe y otro hilo descomprime y pinta ---------------- */
    private final Object mtxImg = new Object();

    /* franjas sueltas: la red las pasa aquí y el hilo de imagen las descomprime en paralelo
       y las sube a la gráfica según van llegando */
    static class Evento {
        int tipo;               /* 0 franja recibida, 1 franja descomprimida, 2 despertar */
        int id, idx, n, w, h, retraso, juego;
        long llegada;
        byte[] datos;
        Bitmap bmp;
    }
    private final LinkedBlockingQueue<Evento> colaImg = new LinkedBlockingQueue<>();
    private byte[] imgPend, imgLibre;
    private int imgBytes, imgW, imgH;
    private Thread hiloImagen;
    private int imgRetraso;                 /* ms medidos por la Deck (captura -> llegada) */
    private long imgLlegada;                /* System.nanoTime() al llegar */

    /* ---------------- datos en pantalla (dos toques) ---------------- */
    private volatile boolean verDatos;
    private volatile String dondeConectado = "";
    private int fpsCuenta, fpsValor;
    private volatile int fpsDeck, fpsRecibe, fpsCaptura, copiaDecimas;
    private int recCuenta;
    private long recDesde;
    private long fpsDesde;
    private double msLocal;
    private TextView datosVista;
    private long datosActualizados;

    /* ---------------- pintar con OpenGL sin esperar a la pantalla ----------------
       Con eglSwapInterval(0) Android no hace cola de imágenes: cada fotograma nuevo
       sustituye al anterior y sale en el siguiente refresco. Así no se acumula retraso. */
    private EGLDisplay eglDpy = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglCtx = EGL14.EGL_NO_CONTEXT;
    private EGLConfig eglCfg;
    private EGLSurface eglSup = EGL14.EGL_NO_SURFACE;
    private Surface eglDe;
    private int glProg, glPos, glUv;
    private final int[] glTex = new int[4], glTexW = new int[4], glTexH = new int[4];
    private boolean glMal;                 /* si OpenGL falla, se pinta con Canvas como antes */
    private final Object mtxSup = new Object();
    private volatile boolean soltarSup;
    private FloatBuffer glQuad;
    private ExecutorService decodificadores;
    private int glRect;                    /* trozo de la imagen que se ve (modo rellenar) */
    /* AMD FSR 1 (EASU + RCAS): https://github.com/GPUOpen-Effects/FidelityFX-FSR (MIT, © AMD) */
    private int glEasu, glRcas, glFbo, glFboTex, glFboW, glFboH;
    private int eCon0, eCon1, eCon2, eCon3, eAlto, rTam, rNit;
    private volatile boolean fsrMal;

    /* ---------------- ajustes de imagen (menú con «atrás») ---------------- */
    static final int MODO_AJUSTAR = 0, MODO_ESTIRAR = 1, MODO_RELLENAR = 2;
    static final String[] NOMBRE_MODO = { "Ajustar", "Estirar", "Rellenar" };
    static final String[] DESC_MODO = { "Imagen entera, con bandas negras", "Toda la pantalla, un poco deformada",
            "Toda la pantalla, recorta los bordes" };
    static final String[] NOMBRE_NIT = { "Desactivada", "Suave", "Media", "Alta" };
    static final float[] PASOS_NIT = { 0f, 1.0f, 0.5f, 0.2f };
    private volatile int ajModo, ajNitidez;
    private FrameLayout menu;
    private TextView[] menuValor = new TextView[3], menuDesc = new TextView[3];

    /* ---------------- sonido ---------------- */
    private final LinkedBlockingQueue<byte[]> colaAudio = new LinkedBlockingQueue<>();
    private volatile int bytesEnCola;

    /* Una conexión con la Deck: por red (socket) o por el canal USB directo */
    static class Conexion {
        InputStream ent;
        OutputStream sal;
        boolean usb;
        Socket sock;
        UsbDeviceConnection usbCon;
        UsbInterface usbIntf;
        String donde;

        void cerrar() {
            try { if (sock != null) sock.close(); } catch (IOException ignored) { }
            if (usbCon != null) {
                try { if (sal != null) sal.write(mensaje(ADIOS, 0, 0)); } catch (IOException ignored) { }
                try { usbCon.releaseInterface(usbIntf); } catch (Exception ignored) { }
                usbCon.close();
            }
        }
    }

    /* Lectura del canal USB: transferencias bulk de 16 KB */
    static class EntradaUsb extends InputStream {
        final UsbDeviceConnection con;
        final UsbEndpoint ep;
        final byte[] buf = new byte[Build.VERSION.SDK_INT >= 28 ? 256 * 1024 : 16384];
        int pos, len;
        volatile int espera = 3000;     /* hasta la primera respuesta; luego sin límite */

        EntradaUsb(UsbDeviceConnection c, UsbEndpoint e) { con = c; ep = e; }

        @Override public int read() throws IOException {
            byte[] b = new byte[1];
            return read(b, 0, 1) <= 0 ? -1 : (b[0] & 0xFF);
        }

        @Override public int read(byte[] b, int off, int n) throws IOException {
            while (pos >= len) {
                int r = con.bulkTransfer(ep, buf, buf.length, espera);
                if (r < 0) throw new IOException(espera > 0 ? "la Deck no contesta" : "USB desconectado");
                pos = 0;
                len = r;
            }
            int k = Math.min(n, len - pos);
            System.arraycopy(buf, pos, b, off, k);
            pos += k;
            return k;
        }

        /* tira lo que quedara de una sesión anterior */
        void vaciar() {
            pos = len = 0;
            long fin = SystemClock.elapsedRealtime() + 1500;
            while (SystemClock.elapsedRealtime() < fin && con.bulkTransfer(ep, buf, buf.length, 150) > 0) { }
        }
    }

    static class SalidaUsb extends OutputStream {
        final UsbDeviceConnection con;
        final UsbEndpoint ep;
        SalidaUsb(UsbDeviceConnection c, UsbEndpoint e) { con = c; ep = e; }
        @Override public void write(int b) throws IOException { write(new byte[] { (byte) b }, 0, 1); }
        @Override public void write(byte[] b, int off, int n) throws IOException {
            byte[] d = b;
            if (off != 0) { d = new byte[n]; System.arraycopy(b, off, d, 0, n); }
            if (con.bulkTransfer(ep, d, n, 1000) < 0) throw new IOException("USB: no se pudo enviar");
        }
    }

    static byte[] mensaje(int tipo, int codigo, int valor) {
        byte[] m = new byte[8];
        ByteBuffer.wrap(m).order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) tipo).putShort((short) codigo).putInt(valor);
        return m;
    }

    /* ======================= ciclo de vida ======================= */

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("decklink", MODE_PRIVATE);
        ajModo = Math.max(0, Math.min(2, prefs.getInt("pantalla", MODO_AJUSTAR)));
        ajNitidez = Math.max(0, Math.min(3, prefs.getInt("nitidez", 0)));
        verDatos = prefs.getBoolean("datos", false);
        if (prefs.contains("pin")) guardarPinArchivo(prefs.getInt("pin", -1));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        /* la frecuencia más alta de la pantalla (90/120 Hz): la imagen nueva sale antes */
        if (Build.VERSION.SDK_INT >= 23) {
            try {
                android.view.Display pantalla = getWindowManager().getDefaultDisplay();
                android.view.Display.Mode actualModo = pantalla.getMode(), mejor = actualModo;
                for (android.view.Display.Mode m : pantalla.getSupportedModes())
                    if (m.getPhysicalWidth() == actualModo.getPhysicalWidth() && m.getPhysicalHeight() == actualModo.getPhysicalHeight()
                            && m.getRefreshRate() > mejor.getRefreshRate()) mejor = m;
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                lp.preferredDisplayModeId = mejor.getModeId();
                getWindow().setAttributes(lp);
            } catch (Exception ignored) { }
        }

        FrameLayout raiz = new FrameLayout(this);
        raiz.setBackgroundColor(Color.BLACK);
        SurfaceView vista = new SurfaceView(this);
        vista.getHolder().addCallback(this);
        /* dos toques: mostrar u ocultar FPS y retraso (los toques no van a la Deck) */
        final GestureDetector gestos = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onDoubleTap(MotionEvent e) {
                cambiarDatos(!verDatos);
                Toast.makeText(MainActivity.this, verDatos ? "Datos activados (dos toques para quitarlos)" : "Datos ocultos",
                        Toast.LENGTH_SHORT).show();
                return true;
            }
        });
        View.OnTouchListener toque = new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent e) { return gestos.onTouchEvent(e); }
        };
        vista.setOnTouchListener(toque);
        raiz.addView(vista, new FrameLayout.LayoutParams(-1, -1));

        capa = construirPantalla();
        capa.setOnTouchListener(toque);

        /* datos (dos toques): recuadro pequeño arriba a la izquierda, como el indicador de rendimiento */
        datosCaja = new LinearLayout(this);
        datosCaja.setOrientation(LinearLayout.HORIZONTAL);
        android.graphics.drawable.GradientDrawable fd = new android.graphics.drawable.GradientDrawable();
        fd.setColor(0xD90B0E13);
        fd.setCornerRadius(dp(6));
        datosCaja.setBackground(fd);
        View barra = new View(this);
        barra.setBackgroundColor(C_AZUL);
        datosCaja.addView(barra, new LinearLayout.LayoutParams(dp(3), -1));
        datosVista = new TextView(this);
        datosVista.setTextSize(12);
        datosVista.setTypeface(android.graphics.Typeface.MONOSPACE);
        datosVista.setLineSpacing(0, 1.15f);
        datosVista.setPadding(dp(10), dp(6), dp(12), dp(6));
        datosCaja.addView(datosVista);
        datosCaja.setVisibility(verDatos ? View.VISIBLE : View.GONE);
        FrameLayout.LayoutParams lpd = new FrameLayout.LayoutParams(-2, -2);
        lpd.leftMargin = lpd.topMargin = dp(10);
        raiz.addView(datosCaja, lpd);
        raiz.addView(capa, new FrameLayout.LayoutParams(-1, -1));
        menu = construirMenu();
        menu.setVisibility(View.GONE);
        raiz.addView(menu, new FrameLayout.LayoutParams(-1, -1));
        setContentView(raiz);
        estado(TEXTO_BUSCANDO);
        registerReceiver(desenchufe, new android.content.IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED));
        buscarActualizacion();
    }

    @Override
    protected void onResume() {
        super.onResume();
        pantallaCompleta();
    }

    /* Los hilos van con onStart/onStop (no onResume/onPause): los avisos de permiso USB
       pausan la app un momento y antes eso cortaba la conexión a medio hacer. */
    @Override
    protected void onStart() {
        super.onStart();
        /* esperar a que terminen los hilos de la vez anterior (si no, dos hilos se pelean por el USB) */
        long fin = SystemClock.elapsedRealtime() + 2000;
        Thread[] viejos = { hiloRed, hiloAudio, hiloEnvio, hiloImagen };
        for (Thread t : viejos) {
            if (t == null) continue;
            long resto = fin - SystemClock.elapsedRealtime();
            if (resto > 0) try { t.join(resto); } catch (InterruptedException ignored) { }
        }
        activo = true;
        usbPermisoPedido = -1;          /* si antes se rechazó el aviso, volver a preguntar */
        hiloRed = new Thread(new Runnable() { @Override public void run() { bucleRed(); } }, "red");
        hiloRed.start();
        hiloAudio = new Thread(new Runnable() { @Override public void run() { bucleAudio(); } }, "sonido");
        hiloAudio.start();
        hiloEnvio = new Thread(new Runnable() { @Override public void run() { bucleEnvio(); } }, "envio");
        hiloEnvio.start();
        decodificadores = Executors.newFixedThreadPool(Math.max(2, Math.min(8, Runtime.getRuntime().availableProcessors())));
        hiloImagen = new Thread(new Runnable() { @Override public void run() { bucleImagen(); } }, "imagen");
        hiloImagen.setPriority(Thread.MAX_PRIORITY);
        hiloImagen.start();
    }

    @Override
    protected void onStop() {
        super.onStop();
        activo = false;
        cerrar();
        pinListo.offer(-2);
        if (hiloRed != null) hiloRed.interrupt();
        if (hiloAudio != null) hiloAudio.interrupt();
        if (hiloEnvio != null) hiloEnvio.interrupt();
        if (hiloImagen != null) hiloImagen.interrupt();
        if (decodificadores != null) decodificadores.shutdownNow();
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        setIntent(i);
        String a = i.getAction();
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(a) || "io.github.viclex2.decklink.PERMISO_USB".equals(a)) {
            usbPermisoPedido = -1;          /* Deck enchufada con la app abierta: probar ya */
        } else if (ACCION_INSTALADO.equals(a)) {
            resultadoInstalacion(i);
        }
    }

    /* Deck desenchufada: cerrar enseguida para que al volver a enchufar empiece limpio */
    private final android.content.BroadcastReceiver desenchufe = new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context ctx, Intent i) {
            Conexion c = actual;
            if (c != null && c.usb) {
                synchronized (cerrojoSalida) { c.cerrar(); }
            }
            usbPermisoPedido = -1;
        }
    };

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try { unregisterReceiver(desenchufe); } catch (Exception ignored) { }
    }

    /* ======================= actualizaciones (GitHub) ======================= */

    static final String ACCION_INSTALADO = "io.github.viclex2.decklink.INSTALADO";

    static int[] versionDe(String t) {
        int[] v = new int[3];
        int k = 0, i = 0;
        while (i < t.length() && !Character.isDigit(t.charAt(i))) i++;
        while (k < 3 && i < t.length() && Character.isDigit(t.charAt(i))) {
            int n = 0;
            while (i < t.length() && Character.isDigit(t.charAt(i))) n = n * 10 + (t.charAt(i++) - '0');
            v[k++] = n;
            if (i < t.length() && t.charAt(i) == '.') i++; else break;
        }
        return v;
    }

    static int comparar(int[] a, int[] b) {
        for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }

    private static String descargarTexto(String url) throws IOException {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("User-Agent", "DeckLink-Android/" + Version.VERSION);
        c.setConnectTimeout(15000);
        c.setReadTimeout(20000);
        try {
            if (c.getResponseCode() != 200) throw new IOException("HTTP " + c.getResponseCode());
            InputStream in = c.getInputStream();
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
            byte[] b = new byte[16384];
            int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            return o.toString("UTF-8");
        } finally {
            c.disconnect();
        }
    }

    /* Al abrir: mira en GitHub si hay una versión más nueva y pregunta */
    private void buscarActualizacion() {
        new Thread(new Runnable() { @Override public void run() {
            try {
                org.json.JSONArray rel = new org.json.JSONArray(
                        descargarTexto("https://api.github.com/repos/viclex2/DeckLink/releases?per_page=20"));
                int[] actual = versionDe(Version.VERSION), mejor = null;
                String tag = null, url = null;
                for (int i = 0; i < rel.length(); i++) {
                    org.json.JSONObject r = rel.getJSONObject(i);
                    if (r.optBoolean("draft")) continue;
                    boolean pre = r.optBoolean("prerelease");
                    if (pre && !Version.PRUEBAS) continue;
                    String u = null;
                    org.json.JSONArray as = r.optJSONArray("assets");
                    for (int k = 0; as != null && k < as.length(); k++)
                        if ("DeckLink-Android.apk".equals(as.getJSONObject(k).optString("name")))
                            u = as.getJSONObject(k).optString("browser_download_url");
                    if (u == null) continue;
                    int[] v = versionDe(r.optString("tag_name"));
                    int cmp = comparar(v, actual);
                    if (cmp > 0 || (cmp == 0 && Version.PRUEBAS && !pre)) {
                        if (mejor == null || comparar(v, mejor) > 0) { mejor = v; tag = r.optString("tag_name"); url = u; }
                    }
                }
                if (tag == null || tag.equals(prefs.getString("omitir", ""))) return;
                final String fTag = tag, fUrl = url;
                runOnUiThread(new Runnable() { @Override public void run() { preguntarActualizar(fTag, fUrl); } });
            } catch (Exception ignored) {
                /* sin internet: no pasa nada */
            }
        } }, "actualizar").start();
    }

    private void preguntarActualizar(final String tag, final String url) {
        if (isFinishing()) return;
        int[] v = versionDe(tag);
        new AlertDialog.Builder(this)
                .setTitle("Hay una versión nueva de DeckLink")
                .setMessage("Versión " + v[0] + "." + v[1] + (v[2] > 0 ? "." + v[2] : "") + " (tienes la " + Version.VERSION + ").\n\n"
                        + "Recuerda instalar la misma versión en la Steam Deck (icono DeckLink › Buscar actualizaciones).")
                .setPositiveButton("Actualizar", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int i) { descargarEInstalar(url); }
                })
                .setNegativeButton("Ahora no", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int i) { prefs.edit().putString("omitir", tag).apply(); }
                })
                .show();
    }

    private void descargarEInstalar(final String url) {
        Toast.makeText(this, "Descargando la versión nueva…", Toast.LENGTH_SHORT).show();
        new Thread(new Runnable() { @Override public void run() {
            try {
                android.content.pm.PackageInstaller pi = getPackageManager().getPackageInstaller();
                android.content.pm.PackageInstaller.SessionParams sp = new android.content.pm.PackageInstaller.SessionParams(
                        android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                int id = pi.createSession(sp);
                android.content.pm.PackageInstaller.Session ses = pi.openSession(id);
                java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
                c.setRequestProperty("User-Agent", "DeckLink-Android/" + Version.VERSION);
                c.setConnectTimeout(15000);
                c.setReadTimeout(30000);
                if (c.getResponseCode() != 200) throw new IOException("HTTP " + c.getResponseCode());
                InputStream in = c.getInputStream();
                OutputStream o = ses.openWrite("decklink.apk", 0, -1);
                byte[] b = new byte[65536];
                int n;
                while ((n = in.read(b)) > 0) o.write(b, 0, n);
                ses.fsync(o);
                o.close();
                in.close();
                c.disconnect();
                Intent i = new Intent(MainActivity.this, MainActivity.class).setAction(ACCION_INSTALADO);
                int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? 0x02000000 /* FLAG_MUTABLE */ : 0);
                ses.commit(PendingIntent.getActivity(MainActivity.this, 7, i, flags).getIntentSender());
                ses.close();
            } catch (final Exception e) {
                runOnUiThread(new Runnable() { @Override public void run() {
                    Toast.makeText(MainActivity.this, "No se pudo descargar: " + e.getMessage(), Toast.LENGTH_LONG).show();
                } });
            }
        } }, "descarga").start();
    }

    /* respuesta del instalador de Android */
    private void resultadoInstalacion(Intent i) {
        int st = i.getIntExtra(android.content.pm.PackageInstaller.EXTRA_STATUS, -999);
        if (st == android.content.pm.PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirmar = i.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirmar != null) {
                confirmar.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try { startActivity(confirmar); } catch (Exception ignored) { }
            }
        } else if (st != android.content.pm.PackageInstaller.STATUS_SUCCESS) {
            String m = i.getStringExtra(android.content.pm.PackageInstaller.EXTRA_STATUS_MESSAGE);
            Toast.makeText(this, "No se pudo actualizar" + (m != null ? ": " + m : ""), Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean f) {
        super.onWindowFocusChanged(f);
        if (f) pantallaCompleta();
    }

    private void pantallaCompleta() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private TextView etiqueta(String t, float sp, int color, boolean negrita) {
        TextView v = new TextView(this);
        v.setText(t);
        v.setTextSize(sp);
        v.setTextColor(color);
        if (negrita) v.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
        return v;
    }

    private android.graphics.drawable.GradientDrawable redondo(int color, float radio, int borde) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radio);
        if (borde != 0) g.setStroke(dp(1), borde);
        return g;
    }

    /* tecla + texto, como las indicaciones de botones de la consola */
    private void pista(LinearLayout fila, String tecla, String texto) {
        TextView k = etiqueta(tecla, 12, C_TEXTO, true);
        k.setBackground(redondo(C_TECLA, dp(4), 0));
        k.setPadding(dp(7), dp(2), dp(7), dp(2));
        LinearLayout.LayoutParams lk = new LinearLayout.LayoutParams(-2, -2);
        lk.leftMargin = dp(20);
        fila.addView(k, lk);
        TextView t = etiqueta(texto, 13, C_GRIS, false);
        LinearLayout.LayoutParams lt = new LinearLayout.LayoutParams(-2, -2);
        lt.leftMargin = dp(8);
        fila.addView(t, lt);
    }

    private View paso(int n, String titulo, String sub) {
        LinearLayout f = new LinearLayout(this);
        f.setOrientation(LinearLayout.HORIZONTAL);
        f.setPadding(0, dp(8), 0, dp(8));
        TextView num = etiqueta(String.valueOf(n), 13, Color.WHITE, true);
        num.setGravity(Gravity.CENTER);
        android.graphics.drawable.GradientDrawable c = new android.graphics.drawable.GradientDrawable();
        c.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        c.setColor(C_AZUL);
        num.setBackground(c);
        f.addView(num, new LinearLayout.LayoutParams(dp(26), dp(26)));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(etiqueta(titulo, 15, C_TEXTO, true));
        col.addView(etiqueta(sub, 13, C_GRIS, false));
        LinearLayout.LayoutParams lc = new LinearLayout.LayoutParams(0, -2, 1);
        lc.leftMargin = dp(14);
        f.addView(col, lc);
        return f;
    }

    /* pantalla de espera: barra arriba, tarjeta en el centro, teclas abajo */
    private LinearLayout construirPantalla() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackground(new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, new int[] { C_FONDO_A, C_FONDO_B }));

        LinearLayout arriba = new LinearLayout(this);
        arriba.setOrientation(LinearLayout.HORIZONTAL);
        arriba.setGravity(Gravity.CENTER_VERTICAL);
        arriba.setBackgroundColor(C_BARRA);
        arriba.setPadding(dp(18), 0, dp(18), 0);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.mipmap.ic_launcher);
        arriba.addView(logo, new LinearLayout.LayoutParams(dp(30), dp(30)));
        TextView nombre = etiqueta("DeckLink", 18, C_TEXTO, true);
        LinearLayout.LayoutParams ln = new LinearLayout.LayoutParams(0, -2, 1);
        ln.leftMargin = dp(12);
        arriba.addView(nombre, ln);
        arriba.addView(etiqueta(Version.VERSION + (Version.PRUEBAS ? " · pruebas" : ""), 13, C_GRIS, false));
        p.addView(arriba, new LinearLayout.LayoutParams(-1, dp(52)));
        View linea = new View(this);
        linea.setBackgroundColor(C_BORDE);
        p.addView(linea, new LinearLayout.LayoutParams(-1, dp(1)));

        android.widget.ScrollView centro = new android.widget.ScrollView(this);
        centro.setFillViewport(true);
        LinearLayout envoltura = new LinearLayout(this);
        envoltura.setGravity(Gravity.CENTER);
        envoltura.setPadding(dp(20), dp(16), dp(20), dp(16));
        LinearLayout tarjeta = new LinearLayout(this);
        tarjeta.setOrientation(LinearLayout.VERTICAL);
        tarjeta.setBackground(redondo(C_TARJETA, dp(14), C_BORDE));
        tarjeta.setPadding(dp(26), dp(22), dp(26), dp(22));

        LinearLayout cab = new LinearLayout(this);
        cab.setOrientation(LinearLayout.HORIZONTAL);
        cab.setGravity(Gravity.CENTER_VERTICAL);
        girando = new android.widget.ProgressBar(this);
        girando.setIndeterminate(true);
        if (Build.VERSION.SDK_INT >= 21)
            girando.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(C_AZUL));
        LinearLayout.LayoutParams lg = new LinearLayout.LayoutParams(dp(26), dp(26));
        lg.rightMargin = dp(14);
        cab.addView(girando, lg);
        tituloVista = etiqueta("", 22, C_TEXTO, true);
        cab.addView(tituloVista, new LinearLayout.LayoutParams(0, -2, 1));
        tarjeta.addView(cab);
        texto = etiqueta("", 15, C_GRIS, false);
        texto.setPadding(0, dp(8), 0, 0);
        tarjeta.addView(texto);

        pasosVista = new LinearLayout(this);
        pasosVista.setOrientation(LinearLayout.VERTICAL);
        View sep = new View(this);
        sep.setBackgroundColor(C_BORDE);
        LinearLayout.LayoutParams ls = new LinearLayout.LayoutParams(-1, dp(1));
        ls.topMargin = dp(18); ls.bottomMargin = dp(8);
        pasosVista.addView(sep, ls);
        pasosVista.addView(paso(1, "Conéctala con un cable USB‑C", "La primera vez, permite que DeckLink use «Steam Deck DeckLink»."));
        pasosVista.addView(paso(2, "O usa la misma red wifi", "En la Deck: icono DeckLink › Activar la conexión por wifi."));
        pasosVista.addView(paso(3, "Juega", "Dos toques: FPS y retraso. «Atrás»: menú (pantalla y nitidez)."));
        tarjeta.addView(pasosVista);

        envoltura.addView(tarjeta, new LinearLayout.LayoutParams(Math.min(dp(560),
                getResources().getDisplayMetrics().widthPixels - dp(40)), -2));
        centro.addView(envoltura, new FrameLayout.LayoutParams(-1, -1));
        p.addView(centro, new LinearLayout.LayoutParams(-1, 0, 1));

        View linea2 = new View(this);
        linea2.setBackgroundColor(C_BORDE);
        p.addView(linea2, new LinearLayout.LayoutParams(-1, dp(1)));
        LinearLayout abajo = new LinearLayout(this);
        abajo.setOrientation(LinearLayout.HORIZONTAL);
        abajo.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        abajo.setBackgroundColor(C_BARRA);
        abajo.setPadding(dp(18), 0, dp(18), 0);
        pista(abajo, "2 toques", "Datos");
        pista(abajo, "Atrás", "Menú");
        p.addView(abajo, new LinearLayout.LayoutParams(-1, dp(44)));
        return p;
    }

    /* "Título\n\nexplicación" en la tarjeta; null = quitar la pantalla y ver la imagen */
    private void estado(final String t) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (t == null) {
                    capa.setVisibility(View.GONE);
                    return;
                }
                int corte = t.indexOf("\n\n");
                if (corte < 0) corte = t.indexOf('\n');
                String tit = corte < 0 ? t : t.substring(0, corte);
                String cuerpo = corte < 0 ? "" : t.substring(corte).trim();
                if (tit.endsWith(".") || tit.endsWith("…")) tit = tit.substring(0, tit.length() - 1);
                tituloVista.setText(tit);
                texto.setText(cuerpo);
                texto.setVisibility(cuerpo.isEmpty() ? View.GONE : View.VISIBLE);
                boolean buscando = t.equals(TEXTO_BUSCANDO);
                pasosVista.setVisibility(buscando ? View.VISIBLE : View.GONE);
                girando.setVisibility(buscando || t.contains("Esperando") || t.contains("Permite") ? View.VISIBLE : View.GONE);
                capa.setVisibility(View.VISIBLE);
            }
        });
    }

    @Override public void surfaceCreated(SurfaceHolder h) { superficie = h; }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { superficie = h; }
    @Override public void surfaceDestroyed(SurfaceHolder h) {
        superficie = null;
        /* el hilo de imagen tiene que soltar la superficie de OpenGL antes de que desaparezca */
        synchronized (mtxSup) { soltarSup = true; }
        synchronized (mtxImg) { mtxImg.notifyAll(); }
        Evento despertar = new Evento();
        despertar.tipo = 2;
        colaImg.offer(despertar);
        long fin = SystemClock.elapsedRealtime() + 600;
        synchronized (mtxSup) {
            while (soltarSup && hiloImagen != null && hiloImagen.isAlive() && SystemClock.elapsedRealtime() < fin) {
                try { mtxSup.wait(50); } catch (InterruptedException e) { break; }
            }
            soltarSup = false;
        }
    }

    private void cerrar() {
        Conexion c = actual;
        actual = null;
        synchronized (cerrojoSalida) {
            if (c != null) c.cerrar();
        }
        try { if (balizas != null) balizas.close(); } catch (Exception ignored) { }
        balizas = null;
    }

    /* ======================= buscar la Deck ======================= */

    /* 1. Canal USB directo */
    private Conexion conectarUsbDirecto() {
        UsbManager um = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (um == null) return null;
        UsbDevice dev = null;
        try {
            for (UsbDevice d : um.getDeviceList().values())
                if (d.getVendorId() == USB_VID && d.getProductId() == USB_PID) dev = d;
        } catch (Exception e) { return null; }
        if (dev == null) { usbPermisoPedido = -1; return null; }

        if (!um.hasPermission(dev)) {
            if (usbPermisoPedido != dev.getDeviceId()) {
                usbPermisoPedido = dev.getDeviceId();
                Intent i = new Intent(this, MainActivity.class).setAction("io.github.viclex2.decklink.PERMISO_USB");
                int flags = Build.VERSION.SDK_INT >= 31 ? 0x02000000 /* FLAG_MUTABLE */ : 0;
                um.requestPermission(dev, PendingIntent.getActivity(this, 0, i, flags));
            }
            estado("Steam Deck conectada por cable.\n\nPermite que DeckLink use «Steam Deck DeckLink» en el aviso que aparece.");
            SystemClock.sleep(700);
            return null;
        }

        UsbInterface intf = null;
        UsbEndpoint entra = null, sale = null;
        for (int i = 0; i < dev.getInterfaceCount() && intf == null; i++) {
            UsbInterface f = dev.getInterface(i);
            if (f.getInterfaceClass() != UsbConstants.USB_CLASS_VENDOR_SPEC || f.getEndpointCount() < 2) continue;
            UsbEndpoint e1 = null, e2 = null;
            for (int k = 0; k < f.getEndpointCount(); k++) {
                UsbEndpoint e = f.getEndpoint(k);
                if (e.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (e.getDirection() == UsbConstants.USB_DIR_IN) e1 = e; else e2 = e;
            }
            if (e1 != null && e2 != null) { intf = f; entra = e1; sale = e2; }
        }
        if (intf == null) {
            estado("La Steam Deck tiene una versión antigua de DeckLink.\nInstala en la Deck la misma versión que en el móvil.");
            SystemClock.sleep(2000);
            return null;
        }
        UsbDeviceConnection con = um.openDevice(dev);
        if (con == null) return null;
        if (!con.claimInterface(intf, true)) { con.close(); return null; }

        Conexion c = new Conexion();
        c.usb = true;
        c.usbCon = con;
        c.usbIntf = intf;
        EntradaUsb ent = new EntradaUsb(con, entra);
        ent.vaciar();
        c.ent = ent;
        c.sal = new SalidaUsb(con, sale);
        c.donde = "cable USB";
        return c;
    }

    private Conexion deSocket(Socket s, String donde) {
        try {
            Conexion c = new Conexion();
            c.sock = s;
            c.ent = s.getInputStream();
            c.sal = s.getOutputStream();
            c.donde = donde;
            return c;
        } catch (IOException e) {
            try { s.close(); } catch (IOException ignored) { }
            return null;
        }
    }

    private Socket conectar(String ip, int ms) {
        try {
            Socket s = new Socket();
            s.setTcpNoDelay(true);
            s.setReceiveBufferSize(4 << 20);
            s.connect(new InetSocketAddress(ip, PUERTO), ms);
            return s;
        } catch (IOException e) {
            return null;
        }
    }

    /* 2. La Deck como tarjeta de red USB: el socket tiene que ir atado a esa red */
    private Socket conectarPorRedUsb() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return null;
        Network[] redes;
        try { redes = cm.getAllNetworks(); } catch (Exception e) { return null; }
        for (Network red : redes) {
            LinkProperties lp = cm.getLinkProperties(red);
            if (lp == null) continue;
            for (LinkAddress la : lp.getLinkAddresses()) {
                String a = la.getAddress().getHostAddress();
                String deck = a.startsWith("10.77.0.") ? IP_USB : a.startsWith("10.79.0.") ? IP_USB2 : null;
                if (deck == null) continue;
                try {
                    Socket s = red.getSocketFactory().createSocket();
                    s.setTcpNoDelay(true);
                    s.setReceiveBufferSize(4 << 20);
                    s.connect(new InetSocketAddress(deck, PUERTO), 1000);
                    return s;
                } catch (IOException ignored) { }
            }
        }
        return null;
    }

    private Conexion buscarDeck(String[] ip) {
        Conexion c = conectarUsbDirecto();
        if (c != null) { ip[0] = "USB"; return c; }

        Socket s = conectarPorRedUsb();
        if (s != null) { ip[0] = "cable"; return deSocket(s, "cable USB"); }

        /* 3. balizas: wifi o "compartir conexión por USB" */
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        WifiManager.MulticastLock bloqueo = wm != null ? wm.createMulticastLock("decklink") : null;
        try {
            if (bloqueo != null) bloqueo.acquire();
            if (balizas == null) {
                DatagramSocket d = new DatagramSocket(null);
                d.setReuseAddress(true);
                d.setBroadcast(true);
                d.bind(new InetSocketAddress(PUERTO_BALIZA));
                balizas = d;
            }
            balizas.setSoTimeout(1500);
            byte[] buf = new byte[256];
            long fin = SystemClock.elapsedRealtime() + 2000;
            while (activo && SystemClock.elapsedRealtime() < fin) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { balizas.receive(p); } catch (SocketTimeoutException t) { break; }
                String[] partes = new String(p.getData(), 0, p.getLength(), "UTF-8").trim().split(" ");
                if (partes.length < 3 || !partes[0].equals("DECKLINK1")) continue;
                String quien = p.getAddress().getHostAddress();
                if (!partes[2].equals("1")) {
                    estado("He encontrado la Steam Deck, pero tiene la conexión por wifi desactivada.\n\nEn la Deck: icono DeckLink › «Activar la conexión por wifi».");
                    continue;
                }
                if (quien.equals(ignorarIp) && SystemClock.elapsedRealtime() < ignorarHasta) continue;
                s = conectar(quien, 2000);
                if (s != null) { ip[0] = quien; return deSocket(s, "wifi"); }
            }
        } catch (IOException e) {
            SystemClock.sleep(1000);
        } finally {
            if (bloqueo != null && bloqueo.isHeld()) bloqueo.release();
        }
        return null;
    }

    /* ======================= enviar a la Deck ======================= */

    private void enviar(int tipo, int codigo, int valor) {
        colaEnvio.offer(mensaje(tipo, codigo, valor));
    }

    private void bucleEnvio() {
        try {
            while (activo) {
                byte[] m = colaEnvio.poll(500, TimeUnit.MILLISECONDS);
                if (m == null) continue;
                synchronized (cerrojoSalida) {
                    Conexion c = actual;
                    if (c == null || c.sal == null) continue;
                    try { c.sal.write(m); } catch (IOException ignored) { }
                }
            }
        } catch (InterruptedException ignored) { }
    }

    /* ======================= recibir ======================= */

    /* Lee la cabecera; por USB, si quedaban restos de una sesión anterior, busca el inicio */
    private static boolean leerCabecera(DataInputStream ent, byte[] cab, boolean resincronizar) throws IOException {
        ent.readFully(cab);
        if (ByteBuffer.wrap(cab).order(ByteOrder.LITTLE_ENDIAN).getInt(0) == MAGIC) return true;
        if (!resincronizar) return false;
        for (int saltados = 0; saltados < (4 << 20); saltados++) {
            System.arraycopy(cab, 1, cab, 0, 15);
            cab[15] = ent.readByte();
            if (ByteBuffer.wrap(cab).order(ByteOrder.LITTLE_ENDIAN).getInt(0) == MAGIC) return true;
        }
        return false;
    }

    private void bucleRed() {
        byte[] cab = new byte[16];
        byte[] datos = new byte[1 << 20];

        while (activo) {
            String[] ip = new String[1];
            Conexion c = buscarDeck(ip);
            if (c == null) {
                if (activo) estado(TEXTO_BUSCANDO);
                continue;
            }
            colaEnvio.clear();
            synchronized (cerrojoSalida) { actual = c; }
            boolean conectado = false, pedirPin = false, conImagen = false;
            try {
                DataInputStream ent = new DataInputStream(c.ent);
                enviar(HOLA, CAP_420 | CAP_FRANJAS, c.usb ? -1 : prefs.getInt("pin", -1));
                while (activo) {
                    if (!leerCabecera(ent, cab, c.usb)) {
                        estado("La Steam Deck tiene otra versión de DeckLink.\nInstala la misma versión en los dos.");
                        SystemClock.sleep(3000);
                        break;
                    }
                    if (c.usb) ((EntradaUsb) c.ent).espera = 0;
                    ByteBuffer h = ByteBuffer.wrap(cab).order(ByteOrder.LITTLE_ENDIAN);
                    h.position(4);
                    int w = h.getShort() & 0xFFFF, alto = h.getShort() & 0xFFFF;
                    int fmt = h.get() & 0xFF;
                    h.position(12);
                    int bytes = h.getInt();
                    if (bytes < 0 || bytes > (64 << 20)) break;
                    if (fmt == FMT_RECHAZO) {
                        if (w == RECHAZO_PIN) pedirPin = true;
                        else estado("La Steam Deck tiene la conexión por wifi desactivada.\n\nEn la Deck: icono DeckLink › «Activar la conexión por wifi».");
                        break;
                    }
                    if (datos.length < bytes) datos = new byte[bytes];
                    ent.readFully(datos, 0, bytes);
                    if (fmt == FMT_NADA) continue;
                    if (!conectado) {
                        conectado = true;
                        dondeConectado = c.donde;
                        estado("Conectado a la Steam Deck por " + c.donde + ".\n\nEsperando la imagen…");
                    }
                    if ((fmt == FMT_JPEG || fmt == FMT_FRANJA) && !conImagen) {
                        conImagen = true;
                        estado(null);
                    }
                    if (fmt == FMT_AUDIO) {
                        if ((cab[9] & 0xFF) != 0) fpsDeck = cab[9] & 0xFF;
                        if ((cab[10] & 0xFF) != 0) { fpsCaptura = cab[10] & 0xFF; copiaDecimas = cab[11] & 0xFF; }
                        byte[] a = new byte[bytes];
                        System.arraycopy(datos, 0, a, 0, bytes);
                        if (bytesEnCola > AUDIO_HZ * 4 / 8) { colaAudio.clear(); bytesEnCola = 0; } /* > 125 ms */
                        bytesEnCola += bytes;
                        colaAudio.offer(a);
                        continue;
                    }
                    if (fmt == FMT_FRANJA && bytes >= 2) {
                        Evento ev = new Evento();
                        ev.tipo = 0;
                        ev.idx = datos[0] & 0xFF;
                        ev.n = datos[1] & 0xFF;
                        ev.id = cab[11] & 0xFF;
                        ev.w = w; ev.h = alto;
                        ev.retraso = (cab[9] & 0xFF) | (cab[10] & 0xFF) << 8;
                        ev.llegada = System.nanoTime();
                        ev.datos = Arrays.copyOfRange(datos, 2, bytes);
                        colaImg.offer(ev);
                        if (ev.idx == ev.n - 1) {
                            enviar(ACK, 0, ev.id);           /* fotograma entero recibido */
                            recCuenta++;
                            long ahoraR = SystemClock.elapsedRealtime();
                            if (recDesde == 0) recDesde = ahoraR;
                            if (ahoraR - recDesde >= 1000) {
                                fpsRecibe = (int) Math.round(recCuenta * 1000.0 / (ahoraR - recDesde));
                                recCuenta = 0;
                                recDesde = ahoraR;
                            }
                        }
                        continue;
                    }
                    if (fmt != FMT_JPEG) continue;          /* esta app solo pide JPEG */
                    enviar(ACK, 0, cab[11] & 0xFF);          /* la Deck ya prepara el siguiente (con su número) */
                    recCuenta++;
                    long ahoraR = SystemClock.elapsedRealtime();
                    if (recDesde == 0) recDesde = ahoraR;
                    if (ahoraR - recDesde >= 1000) {
                        fpsRecibe = (int) Math.round(recCuenta * 1000.0 / (ahoraR - recDesde));
                        recCuenta = 0;
                        recDesde = ahoraR;
                    }
                    /* se lo pasa al hilo de imagen y sigue recibiendo (si iba atrasado, se salta el viejo) */
                    synchronized (mtxImg) {
                        byte[] viejo = imgPend;
                        imgPend = datos; imgBytes = bytes; imgW = w; imgH = alto;
                        imgRetraso = (cab[9] & 0xFF) | (cab[10] & 0xFF) << 8;
                        imgLlegada = System.nanoTime();
                        if (viejo == null) { viejo = imgLibre; imgLibre = null; }
                        datos = viejo != null ? viejo : new byte[bytes];
                        mtxImg.notify();
                    }
                    Evento despertar = new Evento();
                    despertar.tipo = 2;
                    colaImg.offer(despertar);
                }
            } catch (IOException e) {
                /* desconectado */
            } finally {
                synchronized (cerrojoSalida) {
                    if (actual == c) actual = null;
                    c.cerrar();
                }
            }
            if (!activo) break;
            if (pedirPin) {
                pedirPin(ip[0]);
            } else {
                estado(TEXTO_BUSCANDO);
                SystemClock.sleep(500);
            }
        }
    }

    private void pedirPin(final String ip) {
        pinListo.clear();
        estado("La Steam Deck pide un PIN.\n\nMíralo en la Deck: icono DeckLink del escritorio.");
        final Activity yo = this;
        runOnUiThread(new Runnable() { @Override public void run() {
            final EditText campo = new EditText(yo);
            campo.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD);
            campo.setHint("PIN");
            new AlertDialog.Builder(yo)
                    .setTitle("PIN de la Steam Deck")
                    .setMessage("Escribe el PIN que aparece en la Deck (icono DeckLink del escritorio).")
                    .setView(campo)
                    .setCancelable(false)
                    .setPositiveButton("Conectar", new DialogInterface.OnClickListener() {
                        @Override public void onClick(DialogInterface d, int i) {
                            int pin = -1;
                            try { pin = Integer.parseInt(campo.getText().toString().trim()); } catch (NumberFormatException ignored) { }
                            prefs.edit().putInt("pin", pin).apply();
                            guardarPinArchivo(pin);
                            pinListo.offer(pin);
                        }
                    })
                    .setNegativeButton("Cancelar", new DialogInterface.OnClickListener() {
                        @Override public void onClick(DialogInterface d, int i) { pinListo.offer(-1); }
                    })
                    .show();
        } });
        try {
            Integer r = pinListo.poll(10, TimeUnit.MINUTES);
            if (r == null || r < 0) {
                ignorarIp = ip;
                ignorarHasta = SystemClock.elapsedRealtime() + 30000;
            }
        } catch (InterruptedException ignored) { }
    }

    /* ======================= imagen ======================= */

    private static int franjaY0(int i, int n, int h) { return i * (h / n); }
    private static int franjaH(int i, int n, int h) { return i == n - 1 ? h - i * (h / n) : h / n; }

    private Method lockHardware;
    private boolean buscadoLockHardware;

    private Canvas bloquear(SurfaceHolder h) {
        /* lockHardwareCanvas (Android 8+) dibuja con la GPU: mucho más rápido */
        if (!buscadoLockHardware) {
            buscadoLockHardware = true;
            if (Build.VERSION.SDK_INT >= 26) {
                try { lockHardware = Surface.class.getMethod("lockHardwareCanvas"); } catch (Exception ignored) { }
            }
        }
        if (lockHardware != null) {
            try { return (Canvas) lockHardware.invoke(h.getSurface()); } catch (Exception e) { lockHardware = null; }
        }
        return h.lockCanvas();
    }

    private void bucleImagen() {
        final Bitmap[] franjas = new Bitmap[16];
        BitmapFactory.Options[] ops = new BitmapFactory.Options[16];
        for (int i = 0; i < 16; i++) {
            ops[i] = new BitmapFactory.Options();
            ops[i].inMutable = true;
            ops[i].inPreferredConfig = Bitmap.Config.ARGB_8888;
        }
        Paint pintura = new Paint(Paint.FILTER_BITMAP_FLAG);

        /* franjas sueltas: 3 "juegos" de bitmaps y texturas, uno por fotograma en marcha */
        final int J = 3;
        final Bitmap[][] bmp = new Bitmap[J][16];
        final BitmapFactory.Options[][] opsJ = new BitmapFactory.Options[J][16];
        for (int a = 0; a < J; a++)
            for (int i = 0; i < 16; i++) {
                opsJ[a][i] = new BitmapFactory.Options();
                opsJ[a][i].inMutable = true;
                opsJ[a][i].inPreferredConfig = Bitmap.Config.ARGB_8888;
            }
        int[] jId = { -1, -1, -1 }, jN = new int[J], jSub = new int[J], jW = new int[J], jH = new int[J], jRet = new int[J];
        long[] jLleg = new long[J];
        int idActual = -1, juego = 0;

        try {
            while (activo) {
                if (soltarSup) {
                    glSoltarSuperficie();
                    synchronized (mtxSup) { soltarSup = false; mtxSup.notifyAll(); }
                }
                Evento ev = colaImg.poll(500, TimeUnit.MILLISECONDS);
                if (!activo) break;

                /* fotograma entero (Deck con versión anterior) */
                byte[] d = null;
                int bytes = 0, w = 0, h = 0, retraso = 0;
                long llegada = 0;
                synchronized (mtxImg) {
                    if (imgPend != null) {
                        d = imgPend; bytes = imgBytes; w = imgW; h = imgH;
                        retraso = imgRetraso; llegada = imgLlegada;
                        imgPend = null;
                    }
                }
                if (d != null) {
                    try { dibujar(d, bytes, w, h, franjas, ops, pintura, retraso, llegada); } catch (Exception e) { }
                    synchronized (mtxImg) { imgLibre = d; }
                }
                if (ev == null || ev.tipo == 2) continue;

                if (ev.tipo == 0) {
                    /* franja recibida: a descomprimir ya, en paralelo con las demás */
                    if (ev.id != idActual) {
                        idActual = ev.id;
                        juego = (juego + 1) % J;
                        jId[juego] = ev.id; jN[juego] = ev.n; jSub[juego] = 0;
                        jW[juego] = ev.w; jH[juego] = ev.h; jRet[juego] = ev.retraso;
                    }
                    jLleg[juego] = ev.llegada;
                    final Evento e = ev;
                    final int jg = juego;
                    e.juego = jg;
                    ExecutorService ex = decodificadores;
                    if (ex == null || ex.isShutdown() || e.idx >= 16 || e.n < 1 || e.n > 16) continue;
                    ex.execute(new Runnable() {
                        @Override public void run() {
                            try {
                                e.bmp = decodificarFranja(e.datos, 0, e.datos.length, bmp[jg][e.idx], e.w,
                                        franjaH(e.idx, e.n, e.h), opsJ[jg][e.idx]);
                                bmp[jg][e.idx] = e.bmp;
                            } catch (Exception x) {
                                e.bmp = null;
                            }
                            e.tipo = 1;
                            e.datos = null;
                            colaImg.offer(e);
                        }
                    });
                    continue;
                }

                /* franja descomprimida: subirla a la gráfica; con la última, a la pantalla */
                int jg = ev.juego;
                if (jId[jg] != ev.id) continue;                 /* de un fotograma ya abandonado */
                SurfaceHolder sh = superficie;
                if (sh == null) continue;
                boolean gl = glPreparar(sh.getSurface());
                if (gl && ev.bmp != null) glSubir(jg, ev.bmp, franjaY0(ev.idx, ev.n, ev.h), ev.w, ev.h);
                if (++jSub[jg] < jN[jg]) continue;
                try {
                    if (gl) glMostrar(jg, jW[jg], jH[jg]);
                    else canvasMostrar(sh, bmp[jg], jN[jg], jW[jg], jH[jg], pintura);
                } catch (Exception x) { }
                jId[jg] = -1;
                contarFotograma(jLleg[jg]);
                if (verDatos && SystemClock.elapsedRealtime() - datosActualizados > 400) {
                    datosActualizados = SystemClock.elapsedRealtime();
                    mostrarDatos(jW[jg], jH[jg], jRet[jg]);
                }
            }
        } catch (InterruptedException ignored) {
        } finally {
            glTerminar();
            synchronized (mtxSup) { soltarSup = false; mtxSup.notifyAll(); }
        }
    }

    private void contarFotograma(long llegada) {
        double ms = (System.nanoTime() - llegada) / 1e6;
        msLocal = msLocal == 0 ? ms : msLocal * 0.9 + ms * 0.1;
        fpsCuenta++;
        long ahora = SystemClock.elapsedRealtime();
        if (fpsDesde == 0) fpsDesde = ahora;
        if (ahora - fpsDesde >= 1000) {
            fpsValor = (int) Math.round(fpsCuenta * 1000.0 / (ahora - fpsDesde));
            fpsCuenta = 0;
            fpsDesde = ahora;
        }
    }

    private void canvasMostrar(SurfaceHolder sh, Bitmap[] fr, int n, int w, int h, Paint pintura) {
        Canvas c;
        try { c = bloquear(sh); } catch (Exception e) { return; }
        if (c == null) return;
        try {
            float[] m = mapa(c.getWidth(), c.getHeight(), w, h);
            c.drawColor(Color.BLACK);
            for (int i = 0; i < n; i++) {
                if (fr[i] == null) continue;
                float ya = m[1] + franjaY0(i, n, h) * m[3];
                c.drawBitmap(fr[i], null, new RectF(m[0], ya, m[0] + w * m[2], ya + franjaH(i, n, h) * m[3]), pintura);
            }
        } finally {
            try { sh.getSurface().unlockCanvasAndPost(c); } catch (Exception ignored) { }
        }
    }

    /* ======================= menú lateral («atrás») ======================= */

    private void cambiarDatos(boolean v) {
        verDatos = v;
        datosCaja.setVisibility(v ? View.VISIBLE : View.GONE);
        datosActualizados = 0;
        prefs.edit().putBoolean("datos", v).apply();
        actualizarMenu();
    }

    private boolean hayFsr() {
        return !glMal && !fsrMal;
    }

    private void actualizarMenu() {
        if (menu == null) return;
        menuValor[0].setText(NOMBRE_MODO[ajModo]);
        menuDesc[0].setText(DESC_MODO[ajModo]);
        if (hayFsr()) {
            menuValor[1].setText(NOMBRE_NIT[ajNitidez]);
            menuValor[1].setTextColor(C_AZUL);
            menuDesc[1].setText(ajNitidez == 0 ? "Escalado normal (suavizado)" : "Agranda la imagen con bordes nítidos");
        } else {
            menuValor[1].setText("No disponible");
            menuValor[1].setTextColor(C_GRIS);
            menuDesc[1].setText("Este móvil no lo admite");
        }
        menuValor[2].setText(verDatos ? "Sí" : "No");
    }

    /* opción i: dir = -1/+1 cambia el valor; 0 = tocar (avanza) */
    private void opcionMenu(int i, int dir) {
        if (i == 0) {
            ajModo = (ajModo + (dir < 0 ? 2 : 1)) % 3;
            prefs.edit().putInt("pantalla", ajModo).apply();
        } else if (i == 1) {
            if (!hayFsr()) return;
            ajNitidez = (ajNitidez + (dir < 0 ? 3 : 1)) % 4;
            prefs.edit().putInt("nitidez", ajNitidez).apply();
        } else if (i == 2) {
            cambiarDatos(!verDatos);
        } else if (i == 3) {
            cerrarMenu();
            return;
        } else if (i == 4) {
            finish();
            return;
        } else if (i == 5) {
            abrirVr();
            return;
        }
        actualizarMenu();
    }

    /* ---------------- Meta Quest: modo VR (actividad nativa con OpenXR) ---------------- */
    private boolean hayVr() {
        String f = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER.toLowerCase(java.util.Locale.ROOT);
        if (!f.contains("oculus") && !f.contains("meta")) return false;
        return new java.io.File(getApplicationInfo().nativeLibraryDir, "libdecklinkvr.so").exists();
    }

    /* la VR (código nativo) lee el PIN de este archivo */
    private void guardarPinArchivo(int pin) {
        try {
            java.io.FileOutputStream o = new java.io.FileOutputStream(new java.io.File(getFilesDir(), "pin"));
            o.write((pin + "\n").getBytes("UTF-8"));
            o.close();
        } catch (Exception ignored) { }
    }

    private void abrirVr() {
        cerrarMenu();
        try {
            Intent i = new Intent();
            i.setClassName(this, "android.app.NativeActivity");
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
        } catch (Exception e) {
            Toast.makeText(this, "No se pudo abrir la VR: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private boolean menuAbierto() { return menu != null && menu.getVisibility() == View.VISIBLE; }

    private void abrirMenu() {
        actualizarMenu();
        menu.setVisibility(View.VISIBLE);
        View primero = menu.findViewWithTag("op0");
        if (primero != null) primero.requestFocus();
    }

    private void cerrarMenu() {
        menu.setVisibility(View.GONE);
        pantallaCompleta();
    }

    private FrameLayout construirMenu() {
        FrameLayout f = new FrameLayout(this);
        View fondo = new View(this);
        fondo.setBackgroundColor(0x70000000);
        fondo.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { cerrarMenu(); } });
        f.addView(fondo, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xFF171A21);
        panel.setClickable(true);
        LinearLayout cuerpo = new LinearLayout(this);
        cuerpo.setOrientation(LinearLayout.HORIZONTAL);
        View acento = new View(this);
        acento.setBackgroundColor(C_AZUL);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(18), dp(20), dp(14));

        LinearLayout cab = new LinearLayout(this);
        cab.setOrientation(LinearLayout.HORIZONTAL);
        cab.setGravity(Gravity.CENTER_VERTICAL);
        cab.addView(etiqueta("DeckLink", 20, C_TEXTO, true), new LinearLayout.LayoutParams(0, -2, 1));
        cab.addView(etiqueta("V" + Version.VERSION, 12, 0xFF67707B, false));
        col.addView(cab);
        View l1 = new View(this);
        l1.setBackgroundColor(C_BORDE);
        LinearLayout.LayoutParams ll = new LinearLayout.LayoutParams(-1, dp(1));
        ll.topMargin = dp(14); ll.bottomMargin = dp(8);
        col.addView(l1, ll);

        android.widget.ScrollView sv = new android.widget.ScrollView(this);
        LinearLayout ops = new LinearLayout(this);
        ops.setOrientation(LinearLayout.VERTICAL);
        String[] titulos = { "Pantalla", "Nitidez (AMD FSR)", "Datos en pantalla", "Volver al juego", "Salir de DeckLink",
                "Ver en VR (3D)" };
        int[] orden = hayVr() ? new int[] { 0, 1, 2, 5, 3, 4 } : new int[] { 0, 1, 2, 3, 4 };
        for (int k = 0; k < orden.length; k++) {
            final int i = orden[k], fi = i;
            LinearLayout fila = new LinearLayout(this);
            fila.setOrientation(LinearLayout.VERTICAL);
            fila.setPadding(dp(12), dp(11), dp(12), dp(11));
            fila.setTag("op" + i);
            fila.setFocusable(true);
            fila.setClickable(true);
            android.graphics.drawable.StateListDrawable sel = new android.graphics.drawable.StateListDrawable();
            android.graphics.drawable.GradientDrawable marcado = redondo(0xFF3D4450, dp(4), 0);
            sel.addState(new int[] { android.R.attr.state_pressed }, marcado);
            sel.addState(new int[] { android.R.attr.state_focused }, marcado);
            sel.addState(new int[0], new android.graphics.drawable.ColorDrawable(0));
            fila.setBackground(sel);
            LinearLayout arriba = new LinearLayout(this);
            arriba.setOrientation(LinearLayout.HORIZONTAL);
            arriba.setGravity(Gravity.CENTER_VERTICAL);
            arriba.addView(etiqueta(titulos[i], 16, C_TEXTO, true), new LinearLayout.LayoutParams(0, -2, 1));
            if (i < 3) {
                menuValor[i] = etiqueta("", 15, C_AZUL, true);
                arriba.addView(menuValor[i]);
            }
            fila.addView(arriba);
            if (i < 3) {
                menuDesc[i] = etiqueta(i == 2 ? "FPS y retraso (también con dos toques)" : "", 12, C_GRIS, false);
                menuDesc[i].setPadding(0, dp(3), 0, 0);
                fila.addView(menuDesc[i]);
            }
            fila.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { opcionMenu(fi, 0); } });
            fila.setOnKeyListener(new View.OnKeyListener() {
                @Override public boolean onKey(View v, int k, KeyEvent e) {
                    if (e.getAction() != KeyEvent.ACTION_DOWN || fi > 2) return false;
                    if (k == KeyEvent.KEYCODE_DPAD_LEFT) { opcionMenu(fi, -1); return true; }
                    if (k == KeyEvent.KEYCODE_DPAD_RIGHT) { opcionMenu(fi, 1); return true; }
                    return false;
                }
            });
            LinearLayout.LayoutParams lf = new LinearLayout.LayoutParams(-1, -2);
            lf.topMargin = k == 3 ? dp(12) : dp(2);
            ops.addView(fila, lf);
        }
        sv.addView(ops);
        col.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        View l2 = new View(this);
        l2.setBackgroundColor(C_BORDE);
        LinearLayout.LayoutParams ll2 = new LinearLayout.LayoutParams(-1, dp(1));
        ll2.bottomMargin = dp(10);
        col.addView(l2, ll2);
        col.addView(etiqueta("Toca una opción para cambiarla · «Atrás» cierra", 12, C_GRIS, false));

        cuerpo.addView(acento, new LinearLayout.LayoutParams(dp(3), -1));
        cuerpo.addView(col, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(cuerpo, new LinearLayout.LayoutParams(-1, -1));
        int ancho = Math.min(dp(380), (int) (getResources().getDisplayMetrics().widthPixels * 0.6f));
        f.addView(panel, new FrameLayout.LayoutParams(ancho, -1, Gravity.END));
        return f;
    }

    /* ======================= OpenGL ======================= */

    private static int shader(int tipo, String src) {
        int s = GLES20.glCreateShader(tipo);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) throw new RuntimeException("shader: " + GLES20.glGetShaderInfoLog(s));
        return s;
    }

    /* prepara OpenGL para la superficie actual; false = usar Canvas */
    private boolean glPreparar(Surface s) {
        if (glMal || s == null) return false;
        if (eglSup != EGL14.EGL_NO_SURFACE && eglDe == s) return true;
        try {
            if (eglDpy == EGL14.EGL_NO_DISPLAY) {
                eglDpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] v = new int[2];
                if (!EGL14.eglInitialize(eglDpy, v, 0, v, 1)) throw new RuntimeException("eglInitialize");
                int[] atr = { EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE };
                EGLConfig[] cfg = new EGLConfig[1];
                int[] n = new int[1];
                if (!EGL14.eglChooseConfig(eglDpy, atr, 0, cfg, 0, 1, n, 0) || n[0] < 1) throw new RuntimeException("eglChooseConfig");
                eglCfg = cfg[0];
                eglCtx = EGL14.eglCreateContext(eglDpy, eglCfg, EGL14.EGL_NO_CONTEXT,
                        new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
                if (eglCtx == EGL14.EGL_NO_CONTEXT) throw new RuntimeException("eglCreateContext");
            }
            glSoltarSuperficie();
            eglSup = EGL14.eglCreateWindowSurface(eglDpy, eglCfg, s, new int[] { EGL14.EGL_NONE }, 0);
            if (eglSup == EGL14.EGL_NO_SURFACE) throw new RuntimeException("eglCreateWindowSurface");
            if (!EGL14.eglMakeCurrent(eglDpy, eglSup, eglSup, eglCtx)) throw new RuntimeException("eglMakeCurrent");
            EGL14.eglSwapInterval(eglDpy, 0);      /* sin cola: el fotograma más nuevo siempre */
            eglDe = s;
            if (glProg == 0) {
                glProg = GLES20.glCreateProgram();
                GLES20.glAttachShader(glProg, shader(GLES20.GL_VERTEX_SHADER,
                        "attribute vec2 p; attribute vec2 t; varying vec2 v; uniform vec4 r;"
                        + "void main(){ v = r.xy + t * r.zw; gl_Position = vec4(p, 0.0, 1.0); }"));
                GLES20.glAttachShader(glProg, shader(GLES20.GL_FRAGMENT_SHADER,
                        "precision mediump float; uniform sampler2D s; varying vec2 v;"
                        + "void main(){ gl_FragColor = texture2D(s, v); }"));
                GLES20.glLinkProgram(glProg);
                glPos = GLES20.glGetAttribLocation(glProg, "p");
                glUv = GLES20.glGetAttribLocation(glProg, "t");
                glRect = GLES20.glGetUniformLocation(glProg, "r");
                GLES20.glGenTextures(glTex.length, glTex, 0);
                for (int k = 0; k < glTex.length; k++) {
                    GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glTex[k]);
                    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
                    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
                    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
                    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
                    glTexW[k] = glTexH[k] = 0;
                }
                float[] q = { -1, 1, 0, 0,   -1, -1, 0, 1,   1, 1, 1, 0,   1, -1, 1, 1 };
                glQuad = ByteBuffer.allocateDirect(q.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
                glQuad.put(q).position(0);
                fsrPreparar();
            }
            return true;
        } catch (Exception e) {
            glMal = true;
            glTerminar();
            return false;
        }
    }

    private void glSoltarSuperficie() {
        if (eglDpy != EGL14.EGL_NO_DISPLAY && eglSup != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(eglDpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(eglDpy, eglSup);
        }
        eglSup = EGL14.EGL_NO_SURFACE;
        eglDe = null;
    }

    private void glTerminar() {
        glSoltarSuperficie();
        if (eglDpy != EGL14.EGL_NO_DISPLAY) {
            if (eglCtx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDpy, eglCtx);
            EGL14.eglTerminate(eglDpy);
        }
        eglDpy = EGL14.EGL_NO_DISPLAY;
        eglCtx = EGL14.EGL_NO_CONTEXT;
        glProg = 0;
        glEasu = glRcas = glFbo = glFboTex = glFboW = glFboH = 0;
    }

    private void glSubir(int t, Bitmap b, int y0, int w, int h) {
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glTex[t]);
        if (glTexW[t] != w || glTexH[t] != h) {
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, w, h, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
            glTexW[t] = w; glTexH[t] = h;
        }
        if (b.getWidth() == w) GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, y0, b);
    }

    static final String VS_FSR = "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
    static final String FS_EASU =
        "precision highp float;\n"
        + "uniform sampler2D tex; uniform vec4 con0, con1, con2, con3; uniform float alto;\n"
        + "vec3 C(vec2 p){ return texture2D(tex, p).rgb; }\n"
        + "float L(vec3 c){ return c.g + 0.5 * (c.r + c.b); }\n"
        + "void Set(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE){\n"
        + "  float lenX = max(abs(lD - lC), abs(lC - lB)); float dirX = lD - lB; dir.x += dirX * w;\n"
        + "  lenX = clamp(abs(dirX) / max(lenX, 1e-5), 0.0, 1.0); lenX *= lenX; len += lenX * w;\n"
        + "  float lenY = max(abs(lE - lC), abs(lC - lA)); float dirY = lE - lA; dir.y += dirY * w;\n"
        + "  lenY = clamp(abs(dirY) / max(lenY, 1e-5), 0.0, 1.0); lenY *= lenY; len += lenY * w; }\n"
        + "void Tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c){\n"
        + "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len;\n"
        + "  float d2 = min(dot(v, v), clp); float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
        + "  wB *= wB; wA *= wA; wB = 1.5625 * wB - 0.5625; float w = wB * wA; aC += c * w; aW += w; }\n"
        + "void main(){\n"
        + "  vec2 ip = vec2(floor(gl_FragCoord.x), alto - 1.0 - floor(gl_FragCoord.y));\n"
        + "  vec2 pp = ip * con0.xy + con0.zw; vec2 fp = floor(pp); pp -= fp;\n"
        + "  vec2 p0 = fp * con1.xy + con1.zw; vec2 p1 = p0 + con2.xy, p2 = p0 + con2.zw, p3 = p0 + con3.xy;\n"
        + "  vec4 o = vec4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;\n"
        + "  vec3 bC = C(p0 + o.xw), cC = C(p0 + o.yw);\n"
        + "  vec3 iC = C(p1 + o.xw), jC = C(p1 + o.yw), fC = C(p1 + o.yz), eC = C(p1 + o.xz);\n"
        + "  vec3 kC = C(p2 + o.xw), lC = C(p2 + o.yw), hC = C(p2 + o.yz), gC = C(p2 + o.xz);\n"
        + "  vec3 oC = C(p3 + o.yz), nC = C(p3 + o.xz);\n"
        + "  float bL = L(bC), cL = L(cC), iL = L(iC), jL = L(jC), fL = L(fC), eL = L(eC);\n"
        + "  float kL = L(kC), lL = L(lC), hL = L(hC), gL = L(gC), oL = L(oC), nL = L(nC);\n"
        + "  vec2 dir = vec2(0.0); float len = 0.0;\n"
        + "  Set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
        + "  Set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
        + "  Set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
        + "  Set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
        + "  vec2 dir2 = dir * dir; float dirR = dir2.x + dir2.y; bool zro = dirR < (1.0 / 32768.0);\n"
        + "  dirR = zro ? 1.0 : inversesqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;\n"
        + "  len = len * 0.5; len *= len;\n"
        + "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
        + "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
        + "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
        + "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
        + "  vec3 aC = vec3(0.0); float aW = 0.0;\n"
        + "  Tap(aC, aW, vec2( 0.0,-1.0) - pp, dir, len2, lob, clp, bC);\n"
        + "  Tap(aC, aW, vec2( 1.0,-1.0) - pp, dir, len2, lob, clp, cC);\n"
        + "  Tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
        + "  Tap(aC, aW, vec2( 0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
        + "  Tap(aC, aW, vec2( 0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
        + "  Tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
        + "  Tap(aC, aW, vec2( 1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
        + "  Tap(aC, aW, vec2( 2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
        + "  Tap(aC, aW, vec2( 2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
        + "  Tap(aC, aW, vec2( 1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
        + "  Tap(aC, aW, vec2( 1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
        + "  Tap(aC, aW, vec2( 0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
        + "  gl_FragColor = vec4(min(mx, max(mn, aC / aW)), 1.0);\n"
        + "}\n";
    static final String FS_RCAS =
        "precision highp float;\n"
        + "uniform sampler2D tex; uniform vec4 tam; uniform float nit;\n"
        + "vec3 P(vec2 ip){ return texture2D(tex, (ip + 0.5) / tam.xy).rgb; }\n"
        + "void main(){\n"
        + "  vec2 ip = floor(gl_FragCoord.xy - tam.zw);\n"
        + "  vec3 b = P(ip + vec2(0.0, 1.0)), d = P(ip + vec2(-1.0, 0.0)), e = P(ip);\n"
        + "  vec3 f = P(ip + vec2(1.0, 0.0)), h = P(ip + vec2(0.0, -1.0));\n"
        + "  float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
        + "  float eL = e.b * 0.5 + (e.r * 0.5 + e.g), fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
        + "  float hL = h.b * 0.5 + (h.r * 0.5 + h.g);\n"
        + "  float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;\n"
        + "  float rango = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
        + "  nz = clamp(abs(nz) / max(rango, 1e-5), 0.0, 1.0); nz = -0.5 * nz + 1.0;\n"
        + "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
        + "  vec3 hitMin = mn4 / (4.0 * mx4 + 1e-5);\n"
        + "  vec3 hitMax = (1.0 - mx4) / (4.0 * mn4 - 4.0 - 1e-5);\n"
        + "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
        + "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * nit;\n"
        + "  lobe *= nz; float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
        + "  gl_FragColor = vec4((lobe * (b + d + h + f) + e) * rcpL, 1.0);\n"
        + "}\n";

    private static int programa(String vs, String fs) {
        int p = GLES20.glCreateProgram();
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glBindAttribLocation(p, 0, "p");
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) throw new RuntimeException("programa: " + GLES20.glGetProgramInfoLog(p));
        return p;
    }

    /* FSR necesita precisión alta en los shaders; si no hay, la opción queda «no disponible» */
    private void fsrPreparar() {
        if (fsrMal) return;
        try {
            int[] rango = new int[2], prec = new int[1];
            GLES20.glGetShaderPrecisionFormat(GLES20.GL_FRAGMENT_SHADER, GLES20.GL_HIGH_FLOAT, rango, 0, prec, 0);
            if (prec[0] < 16) throw new RuntimeException("sin highp");
            glEasu = programa(VS_FSR, FS_EASU);
            glRcas = programa(VS_FSR, FS_RCAS);
            eCon0 = GLES20.glGetUniformLocation(glEasu, "con0");
            eCon1 = GLES20.glGetUniformLocation(glEasu, "con1");
            eCon2 = GLES20.glGetUniformLocation(glEasu, "con2");
            eCon3 = GLES20.glGetUniformLocation(glEasu, "con3");
            eAlto = GLES20.glGetUniformLocation(glEasu, "alto");
            rTam = GLES20.glGetUniformLocation(glRcas, "tam");
            rNit = GLES20.glGetUniformLocation(glRcas, "nit");
        } catch (Exception e) {
            fsrMal = true;
            glEasu = glRcas = 0;
            runOnUiThread(new Runnable() { @Override public void run() { actualizarMenu(); } });
        }
    }

    private boolean fboPreparar(int w, int h) {
        if (glFbo != 0 && glFboW == w && glFboH == h) return true;
        int[] id = new int[1];
        if (glFbo == 0) { GLES20.glGenFramebuffers(1, id, 0); glFbo = id[0]; }
        if (glFboTex != 0) { id[0] = glFboTex; GLES20.glDeleteTextures(1, id, 0); }
        GLES20.glGenTextures(1, id, 0);
        glFboTex = id[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glFboTex);
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, w, h, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, glFbo);
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, glFboTex, 0);
        boolean ok = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER) == GLES20.GL_FRAMEBUFFER_COMPLETE;
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        glFboW = ok ? w : 0; glFboH = ok ? h : 0;
        return ok;
    }

    private void cuadro(int atributo) {
        glQuad.position(0);
        GLES20.glVertexAttribPointer(atributo, 2, GLES20.GL_FLOAT, false, 16, glQuad);
        GLES20.glEnableVertexAttribArray(atributo);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    }

    /* La imagen en la pantalla según el modo (ajustar/estirar/rellenar) y, si se agranda, con FSR */
    private void glMostrar(int t, int w, int h) {
        int[] sw = new int[1], shh = new int[1];
        EGL14.eglQuerySurface(eglDpy, eglSup, EGL14.EGL_WIDTH, sw, 0);
        EGL14.eglQuerySurface(eglDpy, eglSup, EGL14.EGL_HEIGHT, shh, 0);
        int SW = sw[0], SH = shh[0];
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        GLES20.glViewport(0, 0, SW, SH);
        GLES20.glClearColor(0, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        float sx = 0, sy = 0, sWd = w, sHt = h;          /* trozo de la imagen */
        float dx = 0, dy = 0, dw = SW, dh = SH;          /* dónde va */
        int modo = ajModo;
        if (modo == MODO_AJUSTAR) {
            float e = Math.min((float) SW / w, (float) SH / h);
            dw = Math.round(w * e); dh = Math.round(h * e);
            dx = (float) Math.floor((SW - dw) / 2); dy = (float) Math.floor((SH - dh) / 2);
        } else if (modo == MODO_RELLENAR) {
            float e = Math.max((float) SW / w, (float) SH / h);
            sWd = SW / e; sHt = SH / e;
            sx = (w - sWd) / 2; sy = (h - sHt) / 2;
        }
        int ox = (int) dx, oy = (int) dy, ow = (int) dw, oh = (int) dh;
        int vy = SH - (oy + oh);                          /* OpenGL cuenta y desde abajo */
        int nit = ajNitidez;
        boolean fsr = nit > 0 && glEasu != 0 && (dw > sWd * 1.02f || dh > sHt * 1.02f) && fboPreparar(ow, oh);
        if (fsr) {
            /* 1) EASU: agranda al tamaño de salida, en una textura aparte */
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, glFbo);
            GLES20.glViewport(0, 0, ow, oh);
            GLES20.glUseProgram(glEasu);
            GLES20.glUniform4f(eCon0, sWd / ow, sHt / oh, 0.5f * sWd / ow - 0.5f + sx, 0.5f * sHt / oh - 0.5f + sy);
            GLES20.glUniform4f(eCon1, 1f / w, 1f / h, 1f / w, -1f / h);
            GLES20.glUniform4f(eCon2, -1f / w, 2f / h, 1f / w, 2f / h);
            GLES20.glUniform4f(eCon3, 0, 4f / h, 0, 0);
            GLES20.glUniform1f(eAlto, oh);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glTex[t]);
            cuadro(0);
            /* 2) RCAS: afila y pinta en la pantalla */
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
            GLES20.glViewport(ox, vy, ow, oh);
            GLES20.glUseProgram(glRcas);
            GLES20.glUniform4f(rTam, ow, oh, ox, vy);
            GLES20.glUniform1f(rNit, (float) Math.pow(2, -PASOS_NIT[nit]));
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glFboTex);
            cuadro(0);
        } else {
            GLES20.glViewport(ox, vy, ow, oh);
            GLES20.glUseProgram(glProg);
            GLES20.glUniform4f(glRect, sx / w, sy / h, sWd / w, sHt / h);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, glTex[t]);
            glQuad.position(0);
            GLES20.glVertexAttribPointer(glPos, 2, GLES20.GL_FLOAT, false, 16, glQuad);
            GLES20.glEnableVertexAttribArray(glPos);
            glQuad.position(2);
            GLES20.glVertexAttribPointer(glUv, 2, GLES20.GL_FLOAT, false, 16, glQuad);
            GLES20.glEnableVertexAttribArray(glUv);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        }
        EGL14.eglSwapBuffers(eglDpy, eglSup);
    }

    /* Canvas (sin OpenGL): {x0, y0, escala x, escala y} de la imagen en la pantalla según el modo */
    private float[] mapa(float ew, float eh, int w, int h) {
        int modo = ajModo;
        if (modo == MODO_ESTIRAR) return new float[] { 0, 0, ew / w, eh / h };
        float e = modo == MODO_RELLENAR ? Math.max(ew / w, eh / h) : Math.min(ew / w, eh / h);
        return new float[] { (ew - w * e) / 2, (eh - h * e) / 2, e, e };
    }

    private void glDibujar(Bitmap[] franjas, int n, int w, int h) {
        for (int i = 0; i < n; i++)
            if (franjas[i] != null) glSubir(3, franjas[i], franjaY0(i, n, h), w, h);
        glMostrar(3, w, h);
    }

    /* descomprime una franja reutilizando su bitmap anterior si se puede */
    private static Bitmap decodificarFranja(byte[] d, int off, int len, Bitmap previo, int w, int fh, BitmapFactory.Options op) {
        op.inBitmap = previo != null && previo.getWidth() == w && previo.getHeight() == fh ? previo : null;
        try {
            return BitmapFactory.decodeByteArray(d, off, len, op);
        } catch (IllegalArgumentException e) {
            op.inBitmap = null;
            return BitmapFactory.decodeByteArray(d, off, len, op);
        }
    }

    private void dibujar(final byte[] d, int bytes, final int w, final int h, final Bitmap[] franjas,
                         final BitmapFactory.Options[] ops, Paint pintura, int retraso, long llegada) throws InterruptedException {
        final int n = d[0] & 0xFF;
        if (n < 1 || n > 16 || bytes < 1 + 4 * n) return;
        ByteBuffer b = ByteBuffer.wrap(d, 1, 4 * n).order(ByteOrder.LITTLE_ENDIAN);
        int[] tam = new int[n];
        for (int i = 0; i < n; i++) tam[i] = b.getInt();

        /* las franjas se descomprimen a la vez, una por núcleo */
        List<Callable<Void>> tareas = new ArrayList<>();
        int off = 1 + 4 * n;
        for (int i = 0; i < n; i++) {
            if (tam[i] > 0 && off + tam[i] <= bytes) {
                final int fi = i, foff = off, flen = tam[i];
                tareas.add(new Callable<Void>() {
                    @Override public Void call() {
                        franjas[fi] = decodificarFranja(d, foff, flen, franjas[fi], w, franjaH(fi, n, h), ops[fi]);
                        return null;
                    }
                });
            }
            off += Math.max(tam[i], 0);
        }
        ExecutorService ex = decodificadores;
        if (ex == null || ex.isShutdown()) return;
        ex.invokeAll(tareas);

        SurfaceHolder sh = superficie;
        if (sh == null) return;
        if (glPreparar(sh.getSurface())) {
            glDibujar(franjas, n, w, h);
        } else {
        Canvas c;
        try { c = bloquear(sh); } catch (Exception e) { return; }
        if (c == null) return;
        try {
            float[] m = mapa(c.getWidth(), c.getHeight(), w, h);
            c.drawColor(Color.BLACK);
            for (int i = 0; i < n; i++) {
                if (franjas[i] == null) continue;
                float ya = m[1] + franjaY0(i, n, h) * m[3];
                RectF dst = new RectF(m[0], ya, m[0] + w * m[2], ya + franjaH(i, n, h) * m[3]);
                c.drawBitmap(franjas[i], null, dst, pintura);
            }
        } finally {
            try { sh.getSurface().unlockCanvasAndPost(c); } catch (Exception ignored) { }
        }
        }
        if (verDatos && SystemClock.elapsedRealtime() - datosActualizados > 400) {
            datosActualizados = SystemClock.elapsedRealtime();
            mostrarDatos(w, h, retraso);
        }
        /* FPS y retraso de este lado (de llegar a verse) */
        double ms = (System.nanoTime() - llegada) / 1e6;
        msLocal = msLocal == 0 ? ms : msLocal * 0.9 + ms * 0.1;
        fpsCuenta++;
        long ahora = SystemClock.elapsedRealtime();
        if (fpsDesde == 0) fpsDesde = ahora;
        if (ahora - fpsDesde >= 1000) {
            fpsValor = (int) Math.round(fpsCuenta * 1000.0 / (ahora - fpsDesde));
            fpsCuenta = 0;
            fpsDesde = ahora;
        }
    }

    private static void fila(SpannableStringBuilder t, String etiqueta, String valor, int color) {
        if (t.length() > 0) t.append("\n");
        int a = t.length();
        t.append(String.format("%-8s", etiqueta));
        t.setSpan(new ForegroundColorSpan(C_GRIS), a, t.length(), 0);
        a = t.length();
        t.append(valor);
        t.setSpan(new ForegroundColorSpan(color), a, t.length(), 0);
    }

    private void mostrarDatos(int w, int h, int retraso) {
        int ms = retraso > 0 ? (int) Math.round(retraso + msLocal) : 0;
        int cf = fpsValor >= 55 ? C_VERDE : fpsValor >= 30 ? C_AMARILLO : C_ROJO;
        int cm = ms == 0 ? C_GRIS : ms <= 25 ? C_VERDE : ms <= 45 ? C_AMARILLO : C_ROJO;
        final SpannableStringBuilder t = new SpannableStringBuilder();
        fila(t, "FPS", String.valueOf(fpsValor), cf);
        fila(t, "RETRASO", ms > 0 ? ms + " ms" : "—", cm);
        if (ms > 0) fila(t, "", "Deck " + retraso + " · móvil " + Math.round(msLocal) + " ms", C_GRIS);
        fila(t, "LLEGAN", fpsRecibe + (fpsDeck > 0 ? " / " + fpsDeck + " enviados" : ""), C_TEXTO);
        if (fpsCaptura > 0)
            fila(t, "CAPTURA", fpsCaptura + " · copia " + (copiaDecimas / 10) + "," + (copiaDecimas % 10) + " ms", C_TEXTO);
        fila(t, "", dondeConectado + " · " + w + "x" + h + (glMal ? " · Canvas" : " · OpenGL"), C_GRIS);
        runOnUiThread(new Runnable() { @Override public void run() { datosVista.setText(t); } });
    }

    /* ======================= sonido ======================= */

    private void bucleAudio() {
        int min = AudioTrack.getMinBufferSize(AUDIO_HZ, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack pista;
        if (Build.VERSION.SDK_INT >= 23) {
            pista = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(AUDIO_HZ)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                    .setBufferSizeInBytes(Math.max(min * 2, 8192))
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
        } else {
            pista = new AudioTrack(AudioManager.STREAM_MUSIC, AUDIO_HZ, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, Math.max(min * 2, 8192), AudioTrack.MODE_STREAM);
        }
        pista.play();
        try {
            while (activo) {
                byte[] a = colaAudio.poll(500, TimeUnit.MILLISECONDS);
                if (a == null) continue;
                bytesEnCola -= a.length;
                pista.write(a, 0, a.length);
            }
        } catch (InterruptedException ignored) {
        } finally {
            pista.stop();
            pista.release();
        }
    }

    /* ======================= teclas ======================= */

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            /* «atrás» abre el menú (pantalla, nitidez, datos, salir); otra vez lo cierra */
            if (e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled()) {
                if (menuAbierto()) cerrarMenu();
                else abrirMenu();
            }
            return true;
        }
        /* con el menú abierto, las teclas lo manejan a él y no van a la Deck */
        if (menuAbierto()) return super.dispatchKeyEvent(e);
        /* teclados físicos (bluetooth/USB): Android da el mismo código de tecla que Linux */
        int sc = e.getScanCode();
        if (sc > 0 && sc < 0x2FF && actual != null && e.getRepeatCount() == 0
                && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)) {
            enviar(EV_KEY, sc, e.getAction() == KeyEvent.ACTION_DOWN ? 1 : 0);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
}
