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
import java.util.ArrayList;
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

    static final String TEXTO_BUSCANDO =
            "Buscando la Steam Deck…\n\n"
            + "Por cable: conecta el móvil a la Deck con un cable USB‑C. La primera vez, permite que DeckLink use el dispositivo «Steam Deck DeckLink».\n\n"
            + "Por wifi: actívalo en la Deck (icono DeckLink) y usa la misma red.";

    /* ---------------- interfaz ---------------- */
    private volatile SurfaceHolder superficie;
    private LinearLayout capa;
    private TextView texto;
    private SharedPreferences prefs;
    private long ultimoAtras;

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
    private byte[] imgPend, imgLibre;
    private int imgBytes, imgW, imgH;
    private Thread hiloImagen;
    private int imgRetraso;                 /* ms medidos por la Deck (captura -> llegada) */
    private long imgLlegada;                /* System.nanoTime() al llegar */

    /* ---------------- datos en pantalla (dos toques) ---------------- */
    private volatile boolean verDatos;
    private volatile String dondeConectado = "";
    private int fpsCuenta, fpsValor;
    private volatile int fpsDeck, fpsRecibe;
    private int recCuenta;
    private long recDesde;
    private long fpsDesde;
    private double msLocal;
    private final Paint pDatos = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pFondoDatos = new Paint();
    private ExecutorService decodificadores;

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
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout raiz = new FrameLayout(this);
        raiz.setBackgroundColor(Color.BLACK);
        SurfaceView vista = new SurfaceView(this);
        vista.getHolder().addCallback(this);
        /* dos toques: mostrar u ocultar FPS y retraso (los toques no van a la Deck) */
        final GestureDetector gestos = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onDoubleTap(MotionEvent e) {
                verDatos = !verDatos;
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

        capa = new LinearLayout(this);
        capa.setOrientation(LinearLayout.VERTICAL);
        capa.setGravity(Gravity.CENTER);
        capa.setBackgroundColor(Color.rgb(0x16, 0x14, 0x33));
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.mipmap.ic_launcher);
        int lado = (int) (110 * getResources().getDisplayMetrics().density);
        capa.addView(logo, new LinearLayout.LayoutParams(lado, lado));
        TextView titulo = new TextView(this);
        titulo.setText("DeckLink");
        titulo.setTextColor(Color.WHITE);
        titulo.setTextSize(30);
        titulo.setGravity(Gravity.CENTER);
        capa.addView(titulo);
        texto = new TextView(this);
        texto.setTextColor(Color.rgb(200, 205, 225));
        texto.setTextSize(16);
        texto.setGravity(Gravity.CENTER);
        texto.setPadding(48, 24, 48, 0);
        capa.addView(texto);
        capa.setOnTouchListener(toque);
        raiz.addView(capa, new FrameLayout.LayoutParams(-1, -1));
        setContentView(raiz);
        estado(TEXTO_BUSCANDO);
    }

    @Override
    protected void onResume() {
        super.onResume();
        pantallaCompleta();
        activo = true;
        usbPermisoPedido = -1;          /* si antes se rechazó el aviso, volver a preguntar */
        hiloRed = new Thread(new Runnable() { @Override public void run() { bucleRed(); } }, "red");
        hiloRed.start();
        hiloAudio = new Thread(new Runnable() { @Override public void run() { bucleAudio(); } }, "sonido");
        hiloAudio.start();
        hiloEnvio = new Thread(new Runnable() { @Override public void run() { bucleEnvio(); } }, "envio");
        hiloEnvio.start();
        decodificadores = Executors.newFixedThreadPool(Math.max(2, Math.min(4, Runtime.getRuntime().availableProcessors())));
        hiloImagen = new Thread(new Runnable() { @Override public void run() { bucleImagen(); } }, "imagen");
        hiloImagen.setPriority(Thread.MAX_PRIORITY);
        hiloImagen.start();
    }

    @Override
    protected void onPause() {
        super.onPause();
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
        setIntent(i);           /* p. ej. al conectar la Deck por USB con la app abierta */
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

    private void estado(final String t) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (t == null) {
                    capa.setVisibility(View.GONE);
                } else {
                    texto.setText(t);
                    capa.setVisibility(View.VISIBLE);
                }
            }
        });
    }

    @Override public void surfaceCreated(SurfaceHolder h) { superficie = h; }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { superficie = h; }
    @Override public void surfaceDestroyed(SurfaceHolder h) { superficie = null; }

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
                enviar(HOLA, CAP_420, c.usb ? -1 : prefs.getInt("pin", -1));
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
                    if (fmt == FMT_JPEG && !conImagen) {
                        conImagen = true;
                        estado(null);
                    }
                    if (fmt == FMT_AUDIO) {
                        if ((cab[9] & 0xFF) != 0) fpsDeck = cab[9] & 0xFF;
                        byte[] a = new byte[bytes];
                        System.arraycopy(datos, 0, a, 0, bytes);
                        if (bytesEnCola > AUDIO_HZ * 4 / 8) { colaAudio.clear(); bytesEnCola = 0; } /* > 125 ms */
                        bytesEnCola += bytes;
                        colaAudio.offer(a);
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
        try {
            while (activo) {
                byte[] d;
                int bytes, w, h, retraso;
                long llegada;
                synchronized (mtxImg) {
                    while (activo && imgPend == null) mtxImg.wait(500);
                    if (!activo) break;
                    d = imgPend; bytes = imgBytes; w = imgW; h = imgH;
                    retraso = imgRetraso; llegada = imgLlegada;
                    imgPend = null;
                }
                try {
                    dibujar(d, bytes, w, h, franjas, ops, pintura, retraso, llegada);
                } catch (Exception e) {
                    /* un fotograma roto no debe parar la imagen */
                }
                synchronized (mtxImg) { imgLibre = d; }
            }
        } catch (InterruptedException ignored) { }
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
        Canvas c;
        try { c = bloquear(sh); } catch (Exception e) { return; }
        if (c == null) return;
        try {
            float ew = c.getWidth(), eh = c.getHeight();
            float esc = Math.min(ew / w, eh / h);
            float x0 = (ew - w * esc) / 2, y0 = (eh - h * esc) / 2;
            c.drawColor(Color.BLACK);
            for (int i = 0; i < n; i++) {
                if (franjas[i] == null) continue;
                float ya = y0 + franjaY0(i, n, h) * esc;
                RectF dst = new RectF(x0, ya, x0 + w * esc, ya + franjaH(i, n, h) * esc);
                c.drawBitmap(franjas[i], null, dst, pintura);
            }
            if (verDatos) dibujarDatos(c, w, h, retraso);
        } finally {
            try { sh.getSurface().unlockCanvasAndPost(c); } catch (Exception ignored) { }
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

    private void dibujarDatos(Canvas c, int w, int h, int retraso) {
        float t = Math.max(18f, c.getHeight() / 24f);
        pDatos.setTextSize(t);
        pDatos.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        int ms = retraso > 0 ? (int) Math.round(retraso + msLocal) : 0;
        String[] l = {
                fpsValor + " FPS",
                "llegan " + fpsRecibe + (fpsDeck > 0 ? " · la Deck manda " + fpsDeck : ""),
                ms > 0 ? ms + " ms de retraso" : "retraso: actualiza la Deck",
                ms > 0 ? "Deck + cable " + retraso + " ms · móvil " + Math.round(msLocal) + " ms" : "",
                dondeConectado + " · " + w + "x" + h,
        };
        int[] col = {
                fpsValor >= 55 ? Color.rgb(120, 255, 140) : fpsValor >= 30 ? Color.rgb(255, 210, 90) : Color.rgb(255, 110, 100),
                Color.rgb(170, 175, 195),
                ms == 0 ? Color.LTGRAY : ms <= 25 ? Color.rgb(120, 255, 140) : ms <= 45 ? Color.rgb(255, 210, 90) : Color.rgb(255, 110, 100),
                Color.rgb(170, 175, 195),
                Color.rgb(200, 205, 225),
        };
        float ancho = 0;
        for (String x : l) ancho = Math.max(ancho, pDatos.measureText(x));
        float m = t * 0.6f;
        pFondoDatos.setColor(Color.argb(170, 0, 0, 0));
        c.drawRoundRect(new RectF(m, m, m + ancho + 2 * m, m + l.length * t * 1.3f + m), m, m, pFondoDatos);
        for (int i = 0; i < l.length; i++) {
            pDatos.setColor(col[i]);
            c.drawText(l[i], 2 * m, m + (i + 1) * t * 1.3f - t * 0.25f, pDatos);
        }
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
            if (e.getAction() == KeyEvent.ACTION_UP) {
                long ahora = SystemClock.elapsedRealtime();
                if (ahora - ultimoAtras < 2000) finish();
                else Toast.makeText(this, "Pulsa «atrás» otra vez para salir de DeckLink", Toast.LENGTH_SHORT).show();
                ultimoAtras = ahora;
            }
            return true;
        }
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
