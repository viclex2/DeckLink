package io.github.viclex2.droidvm;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.text.InputType;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/** Cliente VNC (RFB 3.8) mínimo hecho a medida para QEMU por socket unix, con ratón táctil/touchpad y teclado. */
final class VncView extends View {
    interface Host {
        void onStatus(String s);
        void onConnected(int w, int h);
        void onDisconnected();
    }

    private Host host;
    private final Object lock = new Object();
    private Bitmap bmp;
    private int fbW, fbH;
    private int[] fb;
    private Bitmap cursor;
    private int hotX, hotY;
    private volatile boolean running;
    private LocalSocket sock;
    private OutputStream out;
    private Thread thread;

    // escala actual (para traducir toques a píxeles de la VM)
    private float sx = 1, sy = 1, ox = 0, oy = 0;
    boolean stretch = false;
    boolean touchpad = false;
    float speed = 1.6f;

    int ptrX, ptrY, buttons;
    Map<Integer, Integer> layout;
    int sticky = 0; // modificadores pulsados desde la barra: 1 ctrl, 2 alt, 4 shift, 8 win
    Runnable onStickyUsed;

    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();
    private final Map<Integer, Integer> downKeys = new HashMap<Integer, Integer>();

    VncView(Context c) {
        super(c);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackgroundColor(Color.BLACK);
    }

    boolean connected() { return out != null; }

    // ---------------- conexión ----------------

    void start(final String path, final Runner runner, Host h) {
        host = h;
        running = true;
        thread = new Thread(new Runnable() { public void run() {
            int tries = 0;
            while (running) {
                try {
                    LocalSocket s = new LocalSocket();
                    s.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
                    sock = s;
                    session(s);
                } catch (IOException e) {
                    if (!running) break;
                    if (runner != null && !runner.alive) break;
                    if (out != null) { out = null; status("Conexión perdida, reintentando…"); }
                    else status(tries < 3 ? "Arrancando QEMU…" : "Esperando a la pantalla de la VM…");
                    tries++;
                    try { Thread.sleep(500); } catch (InterruptedException ie) { break; }
                } finally {
                    out = null;
                    try { if (sock != null) sock.close(); } catch (IOException ignored) { }
                }
            }
            running = false;
            U.ui(new Runnable() { public void run() { if (host != null) host.onDisconnected(); } });
        } }, "vnc");
        thread.setDaemon(true);
        thread.start();
    }

    void stop() {
        running = false;
        try { if (sock != null) sock.shutdownInput(); } catch (Exception ignored) { }
        try { if (sock != null) sock.close(); } catch (Exception ignored) { }
    }

    private void status(final String s) {
        U.ui(new Runnable() { public void run() { if (host != null) host.onStatus(s); } });
    }

    private static void readFully(DataInputStream in, byte[] b, int len) throws IOException { in.readFully(b, 0, len); }

    private void session(LocalSocket s) throws IOException {
        DataInputStream in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 1 << 16));
        OutputStream o = s.getOutputStream();
        byte[] ver = new byte[12];
        readFully(in, ver, 12);
        String v = new String(ver, "US-ASCII");
        if (!v.startsWith("RFB ")) throw new IOException("No es VNC: " + v);
        boolean v33 = v.compareTo("RFB 003.007") < 0;
        o.write((v33 ? "RFB 003.003\n" : "RFB 003.008\n").getBytes("US-ASCII"));
        o.flush();
        if (v33) {
            int t = in.readInt();
            if (t != 1) throw new IOException("Seguridad VNC no soportada: " + t);
        } else {
            int n = in.readUnsignedByte();
            if (n == 0) throw new IOException("VNC rechazado");
            boolean none = false;
            for (int i = 0; i < n; i++) if (in.readUnsignedByte() == 1) none = true;
            if (!none) throw new IOException("La VM pide contraseña VNC");
            o.write(1);
            o.flush();
            if (in.readInt() != 0) throw new IOException("VNC: fallo de seguridad");
        }
        o.write(1); // ClientInit compartido
        o.flush();
        int w = in.readUnsignedShort(), h = in.readUnsignedShort();
        byte[] pf = new byte[16];
        readFully(in, pf, 16);
        int nl = in.readInt();
        byte[] name = new byte[nl];
        readFully(in, name, nl);
        resize(w, h);

        // formato: 32 bpp, little endian, R<<16 G<<8 B
        byte[] spf = {0, 0, 0, 0, 32, 24, 0, 1, 0, (byte) 255, 0, (byte) 255, 0, (byte) 255, 16, 8, 0, 0, 0, 0};
        o.write(spf);
        int[] encs = {1, 0, -223, -239};
        byte[] se = new byte[4 + 4 * encs.length];
        se[0] = 2; se[2] = 0; se[3] = (byte) encs.length;
        for (int i = 0; i < encs.length; i++) {
            int e = encs[i];
            se[4 + i * 4] = (byte) (e >> 24); se[5 + i * 4] = (byte) (e >> 16); se[6 + i * 4] = (byte) (e >> 8); se[7 + i * 4] = (byte) e;
        }
        o.write(se);
        o.flush();
        out = o;
        final int fw = w, fh = h;
        U.ui(new Runnable() { public void run() { if (host != null) host.onConnected(fw, fh); } });
        request(false);

        byte[] row = new byte[4096 * 4];
        while (running) {
            int type = in.readUnsignedByte();
            if (type == 0) {
                in.readUnsignedByte();
                int nr = in.readUnsignedShort();
                for (int r = 0; r < nr; r++) {
                    int x = in.readUnsignedShort(), y = in.readUnsignedShort();
                    int rw = in.readUnsignedShort(), rh = in.readUnsignedShort();
                    int enc = in.readInt();
                    if (enc == 0) {
                        if (rw * 4 > row.length) row = new byte[rw * 4];
                        synchronized (lock) {
                            for (int yy = 0; yy < rh; yy++) {
                                readFully(in, row, rw * 4);
                                int base = (y + yy) * fbW + x;
                                boolean ok = y + yy < fbH && x + rw <= fbW;
                                if (!ok) continue;
                                for (int xx = 0, j = 0; xx < rw; xx++, j += 4) {
                                    fb[base + xx] = 0xFF000000 | ((row[j + 2] & 0xff) << 16) | ((row[j + 1] & 0xff) << 8) | (row[j] & 0xff);
                                }
                            }
                            if (x + rw <= fbW && y + rh <= fbH && rw > 0 && rh > 0) bmp.setPixels(fb, y * fbW + x, fbW, x, y, rw, rh);
                        }
                    } else if (enc == 1) {
                        int srcx = in.readUnsignedShort(), srcy = in.readUnsignedShort();
                        synchronized (lock) {
                            if (srcx + rw <= fbW && srcy + rh <= fbH && x + rw <= fbW && y + rh <= fbH && rw > 0 && rh > 0) {
                                int[] tmp = new int[rw * rh];
                                for (int yy = 0; yy < rh; yy++) System.arraycopy(fb, (srcy + yy) * fbW + srcx, tmp, yy * rw, rw);
                                for (int yy = 0; yy < rh; yy++) System.arraycopy(tmp, yy * rw, fb, (y + yy) * fbW + x, rw);
                                bmp.setPixels(fb, y * fbW + x, fbW, x, y, rw, rh);
                            }
                        }
                    } else if (enc == -223) {
                        resize(rw, rh);
                        final int nw = rw, nh = rh;
                        U.ui(new Runnable() { public void run() { if (host != null) host.onConnected(nw, nh); } });
                    } else if (enc == -239) {
                        int n = rw * rh;
                        byte[] px = new byte[n * 4];
                        readFully(in, px, n * 4);
                        int mw = (rw + 7) / 8;
                        byte[] mask = new byte[mw * rh];
                        readFully(in, mask, mask.length);
                        Bitmap cb = null;
                        if (rw > 0 && rh > 0) {
                            int[] c = new int[n];
                            for (int yy = 0; yy < rh; yy++) for (int xx = 0; xx < rw; xx++) {
                                int j = (yy * rw + xx) * 4;
                                boolean vis = (mask[yy * mw + xx / 8] & (0x80 >> (xx & 7))) != 0;
                                c[yy * rw + xx] = vis ? (0xFF000000 | ((px[j + 2] & 0xff) << 16) | ((px[j + 1] & 0xff) << 8) | (px[j] & 0xff)) : 0;
                            }
                            cb = Bitmap.createBitmap(c, rw, rh, Bitmap.Config.ARGB_8888);
                        }
                        synchronized (lock) { cursor = cb; hotX = x; hotY = y; }
                    } else {
                        throw new IOException("Codificación VNC inesperada " + enc);
                    }
                }
                postInvalidate();
                request(true);
            } else if (type == 1) {
                in.readUnsignedByte(); in.readUnsignedShort();
                int n = in.readUnsignedShort();
                in.skipBytes(n * 6);
            } else if (type == 2) {
                // campana
            } else if (type == 3) {
                in.skipBytes(3);
                int n = in.readInt();
                in.skipBytes(n);
            } else {
                throw new IOException("Mensaje VNC desconocido " + type);
            }
        }
        throw new EOFException();
    }

    private void resize(int w, int h) {
        synchronized (lock) {
            fbW = Math.max(1, w); fbH = Math.max(1, h);
            fb = new int[fbW * fbH];
            bmp = Bitmap.createBitmap(fbW, fbH, Bitmap.Config.ARGB_8888);
            bmp.eraseColor(Color.BLACK);
            ptrX = Math.min(ptrX, fbW - 1); ptrY = Math.min(ptrY, fbH - 1);
        }
        postInvalidate();
    }

    private void send(byte[] b) {
        OutputStream o = out;
        if (o == null) return;
        try {
            synchronized (this) { o.write(b); o.flush(); }
        } catch (IOException ignored) { }
    }

    private void request(boolean incremental) {
        send(new byte[]{3, (byte) (incremental ? 1 : 0), 0, 0, 0, 0, (byte) (fbW >> 8), (byte) fbW, (byte) (fbH >> 8), (byte) fbH});
    }

    void pointer(int mask) {
        buttons = mask;
        int x = Math.max(0, Math.min(fbW - 1, ptrX)), y = Math.max(0, Math.min(fbH - 1, ptrY));
        send(new byte[]{5, (byte) mask, (byte) (x >> 8), (byte) x, (byte) (y >> 8), (byte) y});
        if (cursor == null || touchpad) postInvalidate();
    }

    void key(int keysym, boolean down) {
        send(new byte[]{4, (byte) (down ? 1 : 0), 0, 0, (byte) (keysym >> 24), (byte) (keysym >> 16), (byte) (keysym >> 8), (byte) keysym});
    }

    void tap(int keysym) { key(keysym, true); key(keysym, false); }

    /** Pulsa una combinación (p. ej. Ctrl+Alt+Supr). */
    void combo(int... ks) {
        for (int k : ks) key(k, true);
        for (int i = ks.length - 1; i >= 0; i--) key(ks[i], false);
    }

    private int[] stickyKeys() {
        int n = Integer.bitCount(sticky);
        int[] r = new int[n];
        int i = 0;
        if ((sticky & 1) != 0) r[i++] = Keys.CTRL;
        if ((sticky & 2) != 0) r[i++] = Keys.ALT;
        if ((sticky & 4) != 0) r[i++] = Keys.SHIFT;
        if ((sticky & 8) != 0) r[i++] = Keys.SUPER;
        return r;
    }

    /** Tecla con los modificadores fijados en la barra (que se sueltan después). */
    void keyWithSticky(int keysym) {
        int[] m = stickyKeys();
        for (int k : m) key(k, true);
        tap(keysym);
        for (int i = m.length - 1; i >= 0; i--) key(m[i], false);
        if (sticky != 0) { sticky = 0; if (onStickyUsed != null) onStickyUsed.run(); }
    }

    void typeChar(int cp) {
        int ks = Keys.keysymFor(cp);
        if (sticky != 0) {
            // con Ctrl/Alt fijados se manda la letra base
            int base = Character.toLowerCase(cp);
            keyWithSticky(Keys.keysymFor(base));
            return;
        }
        int mods = (ks == Keys.ENTER || ks == Keys.TAB || ks == Keys.BACKSPACE) ? 0 : Keys.modsFor(layout, cp);
        if ((mods & Keys.MOD_SHIFT) != 0) key(Keys.SHIFT, true);
        if ((mods & Keys.MOD_ALTGR) != 0) key(Keys.ALTGR, true);
        tap(ks);
        if ((mods & Keys.MOD_ALTGR) != 0) key(Keys.ALTGR, false);
        if ((mods & Keys.MOD_SHIFT) != 0) key(Keys.SHIFT, false);
    }

    void typeText(String s) {
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            typeChar(cp);
            i += Character.charCount(cp);
        }
    }

    /** Teclas físicas o del teclado en pantalla (las que llegan como KeyEvent). */
    boolean handleKey(KeyEvent e) {
        int kc = e.getKeyCode();
        if (kc == KeyEvent.KEYCODE_BACK || kc == KeyEvent.KEYCODE_VOLUME_UP || kc == KeyEvent.KEYCODE_VOLUME_DOWN
                || kc == KeyEvent.KEYCODE_HOME || kc == KeyEvent.KEYCODE_POWER) return false;
        if (out == null) return false;
        if (e.getAction() == KeyEvent.ACTION_MULTIPLE) {
            String chars = e.getCharacters();
            if (chars != null) { typeText(chars); return true; }
            return false;
        }
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        int ks;
        if (down) {
            ks = Keys.special(kc);
            if (ks == 0) {
                int meta = e.getMetaState() & ~(KeyEvent.META_CTRL_MASK | KeyEvent.META_META_MASK | KeyEvent.META_ALT_LEFT_ON);
                if ((meta & KeyEvent.META_ALT_RIGHT_ON) == 0) meta &= ~KeyEvent.META_ALT_ON;
                int u = e.getUnicodeChar(meta);
                if (u == 0) u = e.getUnicodeChar();
                if (u == 0) return false;
                ks = Keys.keysymFor(u);
                boolean physical = e.getDevice() != null && !e.getDevice().isVirtual();
                if (!physical) {
                    // teclado en pantalla que manda KeyEvents: escribir el carácter con sus modificadores
                    if (e.getRepeatCount() >= 0) typeChar(u);
                    return true;
                }
            } else if (sticky != 0 && !isModifier(ks)) {
                keyWithSticky(ks);
                return true;
            }
            downKeys.put(kc, ks);
            key(ks, true);
        } else {
            Integer k = downKeys.remove(kc);
            if (k == null) return Keys.special(kc) != 0 || e.getUnicodeChar() != 0;
            key(k, false);
        }
        return true;
    }

    private static boolean isModifier(int ks) { return ks >= 0xffe1 && ks <= 0xffee || ks == Keys.ALTGR; }

    // ---------------- dibujo ----------------

    @Override protected void onDraw(Canvas c) {
        c.drawColor(Color.BLACK);
        synchronized (lock) {
            if (bmp == null) return;
            int W = getWidth(), H = getHeight();
            if (stretch) { sx = (float) W / fbW; sy = (float) H / fbH; }
            else { float s = Math.min((float) W / fbW, (float) H / fbH); sx = sy = s; }
            ox = (W - fbW * sx) / 2f; oy = (H - fbH * sy) / 2f;
            src.set(0, 0, fbW, fbH);
            dst.set(ox, oy, ox + fbW * sx, oy + fbH * sy);
            c.drawBitmap(bmp, src, dst, paint);
            if (cursor != null) {
                float cx = ox + (ptrX - hotX) * sx, cy = oy + (ptrY - hotY) * sy;
                dst.set(cx, cy, cx + cursor.getWidth() * sx, cy + cursor.getHeight() * sy);
                src.set(0, 0, cursor.getWidth(), cursor.getHeight());
                c.drawBitmap(cursor, src, dst, paint);
            } else if (touchpad) {
                Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
                float cx = ox + ptrX * sx, cy = oy + ptrY * sy;
                p.setColor(Color.BLACK); c.drawCircle(cx, cy, U.dp(getContext(), 6), p);
                p.setColor(Color.WHITE); c.drawCircle(cx, cy, U.dp(getContext(), 4), p);
            }
        }
    }

    // ---------------- ratón y toques ----------------

    private float downX, downY, lastX, lastY, scrollAcc;
    private long downT;
    private boolean moved, dragging, longPressed, two, twoMoved;
    private final Runnable longPress = new Runnable() { public void run() {
        longPressed = true;
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        if (touchpad) { dragging = true; pointer(1); }
        else { pointer(4); pointer(0); }
    } };

    private void mapTo(float x, float y) {
        ptrX = (int) ((x - ox) / sx);
        ptrY = (int) ((y - oy) / sy);
        ptrX = Math.max(0, Math.min(fbW - 1, ptrX));
        ptrY = Math.max(0, Math.min(fbH - 1, ptrY));
    }

    private static int mouseMask(int bs) {
        int m = 0;
        if ((bs & MotionEvent.BUTTON_PRIMARY) != 0) m |= 1;
        if ((bs & MotionEvent.BUTTON_TERTIARY) != 0) m |= 2;
        if ((bs & MotionEvent.BUTTON_SECONDARY) != 0) m |= 4;
        return m;
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0) {
            if (e.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE) {
                mapTo(e.getX(), e.getY());
                pointer(mouseMask(e.getButtonState()));
                return true;
            }
            if (e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
                float v = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
                wheel(v > 0);
                return true;
            }
        }
        return super.onGenericMotionEvent(e);
    }

    private void wheel(boolean up) {
        int b = up ? 8 : 16;
        pointer(buttons | b);
        pointer(buttons & ~24);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (bmp == null) return true;
        if (e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
            mapTo(e.getX(), e.getY());
            pointer(e.getActionMasked() == MotionEvent.ACTION_UP ? 0 : mouseMask(e.getButtonState()));
            return true;
        }
        float slop = U.dp(getContext(), 8);
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestFocus();
                downX = lastX = e.getX(); downY = lastY = e.getY(); downT = e.getEventTime();
                moved = dragging = longPressed = two = twoMoved = false;
                if (!touchpad) { mapTo(e.getX(), e.getY()); pointer(0); }
                postDelayed(longPress, 450);
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                removeCallbacks(longPress);
                if (dragging) { dragging = false; pointer(0); }
                two = true; scrollAcc = 0;
                lastY = avgY(e);
                break;
            case MotionEvent.ACTION_MOVE:
                if (two) {
                    if (e.getPointerCount() >= 2) {
                        float y = avgY(e);
                        scrollAcc += y - lastY;
                        lastY = y;
                        float step = U.dp(getContext(), 28);
                        while (Math.abs(scrollAcc) > step) {
                            twoMoved = true;
                            wheel(scrollAcc > 0);
                            scrollAcc -= Math.signum(scrollAcc) * step;
                        }
                    }
                    break;
                }
                float dx = e.getX() - lastX, dy = e.getY() - lastY;
                if (!moved && Math.hypot(e.getX() - downX, e.getY() - downY) > slop) {
                    moved = true;
                    if (!longPressed) removeCallbacks(longPress);
                    if (!touchpad && !longPressed) { dragging = true; pointer(1); }
                }
                if (touchpad) {
                    if (moved || dragging) {
                        ptrX = (int) Math.max(0, Math.min(fbW - 1, ptrX + dx * speed / sx));
                        ptrY = (int) Math.max(0, Math.min(fbH - 1, ptrY + dy * speed / sy));
                        pointer(dragging ? 1 : 0);
                    }
                } else if (dragging) {
                    mapTo(e.getX(), e.getY());
                    pointer(1);
                }
                lastX = e.getX(); lastY = e.getY();
                break;
            case MotionEvent.ACTION_UP:
                removeCallbacks(longPress);
                if (two) {
                    if (!twoMoved && e.getEventTime() - downT < 400) { pointer(4); pointer(0); }
                } else if (dragging) {
                    pointer(0);
                } else if (!longPressed && !moved) {
                    if (!touchpad) mapTo(e.getX(), e.getY());
                    pointer(1); pointer(0);
                }
                dragging = false;
                break;
            case MotionEvent.ACTION_CANCEL:
                removeCallbacks(longPress);
                if (dragging) pointer(0);
                dragging = false;
                break;
        }
        return true;
    }

    private static float avgY(MotionEvent e) {
        float s = 0;
        for (int i = 0; i < e.getPointerCount(); i++) s += e.getY(i);
        return s / e.getPointerCount();
    }

    // ---------------- teclado en pantalla ----------------

    @Override public boolean onCheckIsTextEditor() { return true; }

    @Override public InputConnection onCreateInputConnection(EditorInfo a) {
        a.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        a.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_NONE;
        return new BaseInputConnection(this, false) {
            @Override public boolean commitText(CharSequence t, int pos) { typeText(t.toString()); return true; }
            @Override public boolean setComposingText(CharSequence t, int pos) { return true; }
            @Override public boolean finishComposingText() { return true; }
            @Override public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < Math.max(1, before); i++) keyWithSticky(Keys.BACKSPACE);
                return true;
            }
            @Override public boolean sendKeyEvent(KeyEvent ev) { handleKey(ev); return true; }
            @Override public boolean performEditorAction(int action) { tap(Keys.ENTER); return true; }
        };
    }
}
