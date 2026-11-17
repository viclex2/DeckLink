/*
 * DeckLink para Android (móvil, tablet y Meta Quest).
 * Hace de pantalla de la Steam Deck: muestra su imagen, reproduce su sonido
 * y convierte los toques en el ratón de la Deck.
 *
 * Busca la Deck por el cable USB (10.77.0.1) y por wifi (balizas UDP de la Deck).
 * Por wifi pide el PIN que muestra el icono DeckLink de la Deck.
 */
package io.github.viclex2.decklink;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.view.Gravity;
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
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    /* ---------------- protocolo (igual que src/protocolo.h) ---------------- */
    static final int PUERTO = 47800, PUERTO_BALIZA = 47801;
    static final String IP_USB = "10.77.0.1";
    static final int MAGIC = 0x33464C44;
    static final int ACK = 0xFFFF, HOLA = 0xFFFE;
    static final int FMT_JPEG = 1, FMT_AUDIO = 2, FMT_LZ4 = 3, FMT_RECHAZO = 4;
    static final int RECHAZO_WIFI_OFF = 1, RECHAZO_PIN = 2;
    static final int EV_KEY = 1;
    static final int AUDIO_HZ = 48000;

    /* ---------------- interfaz ---------------- */
    private SurfaceView vista;
    private volatile SurfaceHolder superficie;
    private LinearLayout capa;
    private TextView texto;
    private SharedPreferences prefs;

    /* ---------------- conexión ---------------- */
    private volatile boolean activo;
    private Thread hiloRed;
    private volatile Socket sock;
    private volatile DatagramSocket balizas;
    private OutputStream salida;
    private final Object cerrojoSalida = new Object();
    private final ArrayBlockingQueue<Integer> pinListo = new ArrayBlockingQueue<>(1);
    private String ignorarIp = "";
    private long ignorarHasta;

    private long ultimoAtras;

    /* ---------------- sonido ---------------- */
    private final LinkedBlockingQueue<byte[]> colaAudio = new LinkedBlockingQueue<>();
    private volatile int bytesEnCola;
    private Thread hiloAudio;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("decklink", MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout raiz = new FrameLayout(this);
        raiz.setBackgroundColor(Color.BLACK);
        vista = new SurfaceView(this);
        vista.getHolder().addCallback(this);
        raiz.addView(vista, new FrameLayout.LayoutParams(-1, -1));

        capa = new LinearLayout(this);
        capa.setOrientation(LinearLayout.VERTICAL);
        capa.setGravity(Gravity.CENTER);
        capa.setBackgroundColor(Color.rgb(0x16, 0x14, 0x33));
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.mipmap.ic_launcher);
        int lado = (int) (120 * getResources().getDisplayMetrics().density);
        capa.addView(logo, new LinearLayout.LayoutParams(lado, lado));
        TextView titulo = new TextView(this);
        titulo.setText("DeckLink");
        titulo.setTextColor(Color.WHITE);
        titulo.setTextSize(32);
        titulo.setGravity(Gravity.CENTER);
        capa.addView(titulo);
        texto = new TextView(this);
        texto.setTextColor(Color.rgb(200, 205, 225));
        texto.setTextSize(17);
        texto.setGravity(Gravity.CENTER);
        texto.setPadding(40, 30, 40, 0);
        capa.addView(texto);
        raiz.addView(capa, new FrameLayout.LayoutParams(-1, -1));
        setContentView(raiz);
        estado("Buscando la Steam Deck…\n\nPor wifi: actívalo en la Deck (icono DeckLink) y usa la misma red.\nPor cable: conecta la Deck con un cable USB.");
    }

    @Override
    protected void onResume() {
        super.onResume();
        pantallaCompleta();
        activo = true;
        hiloRed = new Thread(new Runnable() { @Override public void run() { bucleRed(); } }, "red");
        hiloRed.start();
        hiloAudio = new Thread(new Runnable() { @Override public void run() { bucleAudio(); } }, "sonido");
        hiloAudio.start();
        hiloEnvio = new Thread(new Runnable() { @Override public void run() { bucleEnvio(); } }, "envio");
        hiloEnvio.start();
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

    /* ======================= red ======================= */

    private void cerrar() {
        try { if (sock != null) sock.close(); } catch (IOException ignored) { }
        try { if (balizas != null) balizas.close(); } catch (Exception ignored) { }
        sock = null;
        balizas = null;
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

    /* primero el cable USB, luego las balizas que manda la Deck por wifi */
    private Socket buscarDeck(String[] ip) {
        Socket s = conectar(IP_USB, 500);
        if (s != null) { ip[0] = IP_USB; return s; }

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
                if (s != null) { ip[0] = quien; return s; }
            }
        } catch (IOException e) {
            SystemClock.sleep(1000);
        } finally {
            if (bloqueo != null && bloqueo.isHeld()) bloqueo.release();
        }
        return null;
    }

    private final LinkedBlockingQueue<byte[]> colaEnvio = new LinkedBlockingQueue<>();
    private Thread hiloEnvio;

    private void enviar(int tipo, int codigo, int valor) {
        byte[] m = new byte[8];
        ByteBuffer.wrap(m).order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) tipo).putShort((short) codigo).putInt(valor);
        colaEnvio.offer(m);
    }

    private void bucleEnvio() {
        try {
            while (activo) {
                byte[] m = colaEnvio.poll(500, TimeUnit.MILLISECONDS);
                if (m == null) continue;
                synchronized (cerrojoSalida) {
                    if (salida == null) continue;
                    try { salida.write(m); } catch (IOException ignored) { }
                }
            }
        } catch (InterruptedException ignored) { }
    }

    private void bucleRed() {
        byte[] cab = new byte[16];
        byte[] datos = new byte[1 << 20];
        Bitmap[] franjas = new Bitmap[16];
        BitmapFactory.Options op = new BitmapFactory.Options();
        op.inMutable = true;
        op.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Paint pintura = new Paint(Paint.FILTER_BITMAP_FLAG);

        while (activo) {
            String[] ip = new String[1];
            Socket s = buscarDeck(ip);
            if (s == null) continue;
            sock = s;
            boolean conectado = false, pedirPin = false;
            try {
                DataInputStream ent = new DataInputStream(s.getInputStream());
                colaEnvio.clear();
                synchronized (cerrojoSalida) { salida = s.getOutputStream(); }
                enviar(HOLA, 0, prefs.getInt("pin", -1));

                while (activo) {
                    ent.readFully(cab);
                    ByteBuffer h = ByteBuffer.wrap(cab).order(ByteOrder.LITTLE_ENDIAN);
                    int magic = h.getInt(), w = h.getShort() & 0xFFFF, alto = h.getShort() & 0xFFFF;
                    int fmt = h.get() & 0xFF;
                    h.position(12);
                    int bytes = h.getInt();
                    if (magic != MAGIC || bytes < 0 || bytes > (64 << 20)) {
                        estado("La Steam Deck tiene otra versión de DeckLink.\nInstala la misma versión en los dos.");
                        SystemClock.sleep(3000);
                        break;
                    }
                    if (fmt == FMT_RECHAZO) {
                        if (w == RECHAZO_PIN) pedirPin = true;
                        else estado("La Steam Deck tiene la conexión por wifi desactivada.\n\nEn la Deck: icono DeckLink › «Activar la conexión por wifi».");
                        break;
                    }
                    if (datos.length < bytes) datos = new byte[bytes];
                    ent.readFully(datos, 0, bytes);
                    if (!conectado) {
                        conectado = true;
                        estado(null);
                    }
                    if (fmt == FMT_AUDIO) {
                        byte[] a = new byte[bytes];
                        System.arraycopy(datos, 0, a, 0, bytes);
                        if (bytesEnCola > AUDIO_HZ * 4 / 8) { colaAudio.clear(); bytesEnCola = 0; } /* > 125 ms */
                        bytesEnCola += bytes;
                        colaAudio.offer(a);
                        continue;
                    }
                    if (fmt != FMT_JPEG) continue;          /* esta app solo pide JPEG */
                    enviar(ACK, 0, 0);                       /* la Deck ya prepara el siguiente */
                    dibujar(datos, bytes, w, alto, franjas, op, pintura);
                }
            } catch (IOException e) {
                /* desconectado */
            } finally {
                synchronized (cerrojoSalida) { salida = null; }
                try { s.close(); } catch (IOException ignored) { }
                sock = null;
            }
            if (!activo) break;
            if (pedirPin) {
                pedirPin(ip[0]);
            } else {
                estado("Buscando la Steam Deck…\n\nPor wifi: actívalo en la Deck (icono DeckLink) y usa la misma red.\nPor cable: conecta la Deck con un cable USB.");
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
                    .setPositiveButton("Conectar", new android.content.DialogInterface.OnClickListener() {
                        @Override public void onClick(android.content.DialogInterface d, int i) {
                            String t = campo.getText().toString().trim();
                            int pin = -1;
                            try { pin = Integer.parseInt(t); } catch (NumberFormatException ignored) { }
                            prefs.edit().putInt("pin", pin).apply();
                            pinListo.offer(pin);
                        }
                    })
                    .setNegativeButton("Cancelar", new android.content.DialogInterface.OnClickListener() {
                        @Override public void onClick(android.content.DialogInterface d, int i) { pinListo.offer(-1); }
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

    private void dibujar(byte[] d, int bytes, int w, int h, Bitmap[] franjas, BitmapFactory.Options op, Paint pintura) {
        int n = d[0] & 0xFF;
        if (n < 1 || n > 16 || bytes < 1 + 4 * n) return;
        ByteBuffer b = ByteBuffer.wrap(d, 1, 4 * n).order(ByteOrder.LITTLE_ENDIAN);
        int[] tam = new int[n];
        for (int i = 0; i < n; i++) tam[i] = b.getInt();

        int off = 1 + 4 * n;
        for (int i = 0; i < n; i++) {
            if (tam[i] <= 0 || off + tam[i] > bytes) { off += Math.max(tam[i], 0); continue; }
            Bitmap previo = franjas[i];
            op.inBitmap = previo != null && previo.getWidth() == w && previo.getHeight() == franjaH(i, n, h) ? previo : null;
            try {
                franjas[i] = BitmapFactory.decodeByteArray(d, off, tam[i], op);
            } catch (IllegalArgumentException e) {
                op.inBitmap = null;
                franjas[i] = BitmapFactory.decodeByteArray(d, off, tam[i], op);
            }
            off += tam[i];
        }

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
        } finally {
            try { sh.getSurface().unlockCanvasAndPost(c); } catch (Exception ignored) { }
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

    /* ======================= toques y teclas ======================= */

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
        if (sc > 0 && sc < 0x2FF && sock != null && e.getRepeatCount() == 0
                && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)) {
            enviar(EV_KEY, sc, e.getAction() == KeyEvent.ACTION_DOWN ? 1 : 0);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
}
