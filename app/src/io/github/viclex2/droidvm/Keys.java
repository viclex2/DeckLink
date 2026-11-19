package io.github.viclex2.droidvm;

import android.content.Context;
import android.view.KeyEvent;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.HashMap;
import java.util.Map;

/** Teclas: de Android a keysyms de X11 (lo que entiende VNC), y modificadores según la distribución de QEMU. */
final class Keys {
    static final int SHIFT = 0xffe1, CTRL = 0xffe3, ALT = 0xffe9, ALTGR = 0xfe03, SUPER = 0xffeb,
            ENTER = 0xff0d, BACKSPACE = 0xff08, TAB = 0xff09, ESC = 0xff1b, DELETE = 0xffff,
            LEFT = 0xff51, UP = 0xff52, RIGHT = 0xff53, DOWN = 0xff54, HOME = 0xff50, END = 0xff57,
            PGUP = 0xff55, PGDN = 0xff56, INSERT = 0xff63, F1 = 0xffbe, PRINT = 0xff61, MENU = 0xff67;

    static final int MOD_SHIFT = 1, MOD_ALTGR = 2;

    private static Map<String, int[]> names;          // nombre X11 -> {keysym, unicode}
    private static final Map<String, Map<Integer, Integer>> LAYOUTS = new HashMap<String, Map<Integer, Integer>>();

    static synchronized void loadNames(Context c) {
        if (names != null) return;
        names = new HashMap<String, int[]>();
        try {
            BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("keysyms.txt"), "UTF-8"));
            String l;
            while ((l = r.readLine()) != null) {
                String[] p = l.trim().split(" ");
                if (p.length == 3) names.put(p[0], new int[]{Integer.parseInt(p[1], 16), Integer.parseInt(p[2], 16)});
            }
            r.close();
        } catch (IOException ignored) { }
    }

    /** unicode -> modificadores necesarios en esa distribución de teclado (según el fichero keymaps/&lt;nombre&gt; de QEMU). */
    static synchronized Map<Integer, Integer> layout(Context c, String name) {
        Map<Integer, Integer> m = LAYOUTS.get(name);
        if (m != null) return m;
        loadNames(c);
        m = new HashMap<Integer, Integer>();
        parse(new File(new File(Bins.share(c), "keymaps"), name), m, 0);
        LAYOUTS.put(name, m);
        return m;
    }

    private static void parse(File f, Map<Integer, Integer> m, int depth) {
        if (depth > 3 || !f.isFile()) return;
        try {
            BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), "ISO-8859-1"));
            String l;
            while ((l = r.readLine()) != null) {
                l = l.trim();
                if (l.length() == 0 || l.startsWith("#")) continue;
                String[] p = l.split("\\s+");
                if (p[0].equals("include") && p.length > 1) { parse(new File(f.getParentFile(), p[1]), m, depth + 1); continue; }
                if (p.length < 2 || !p[1].startsWith("0x")) continue;
                int mods = 0;
                boolean addUpper = false, skip = false;
                for (int i = 2; i < p.length; i++) {
                    if (p[i].equals("shift")) mods |= MOD_SHIFT;
                    else if (p[i].equals("altgr")) mods |= MOD_ALTGR;
                    else if (p[i].equals("addupper")) addUpper = true;
                    else if (p[i].equals("ctrl") || p[i].equals("numlock") || p[i].equals("inhibit")) skip = true;
                }
                if (skip) continue;
                int u = unicodeOf(p[0]);
                if (u <= 0) continue;
                Integer old = m.get(u);
                if (old == null || Integer.bitCount(old) > Integer.bitCount(mods)) m.put(u, mods);
                if (addUpper && Character.isLowerCase(u)) {
                    int up = Character.toUpperCase(u);
                    if (!m.containsKey(up)) m.put(up, mods | MOD_SHIFT);
                }
            }
            r.close();
        } catch (IOException ignored) { }
    }

    private static int unicodeOf(String name) {
        if (name.length() == 1) return name.charAt(0);
        int[] v = names.get(name);
        return v == null ? -1 : v[1];
    }

    /** Keysym de un carácter Unicode. */
    static int keysymFor(int cp) {
        if (cp == '\n' || cp == '\r') return ENTER;
        if (cp == '\t') return TAB;
        if (cp == 8) return BACKSPACE;
        if (cp >= 0x20 && cp <= 0x7e) return cp;
        if (cp >= 0xa0 && cp <= 0xff) return cp;
        if (names != null) for (int[] v : names.values()) if (v[1] == cp) return v[0];
        return 0x01000000 | cp;
    }

    static int modsFor(Map<Integer, Integer> layout, int cp) {
        Integer m = layout == null ? null : layout.get(cp);
        if (m != null) return m;
        return Character.isUpperCase(cp) ? MOD_SHIFT : 0;
    }

    /** Keysym para teclas especiales de Android (0 si no es especial). */
    static int special(int kc) {
        switch (kc) {
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: return ENTER;
            case KeyEvent.KEYCODE_DEL: return BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL: return DELETE;
            case KeyEvent.KEYCODE_TAB: return TAB;
            case KeyEvent.KEYCODE_ESCAPE: return ESC;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_MOVE_HOME: return HOME;
            case KeyEvent.KEYCODE_MOVE_END: return END;
            case KeyEvent.KEYCODE_PAGE_UP: return PGUP;
            case KeyEvent.KEYCODE_PAGE_DOWN: return PGDN;
            case KeyEvent.KEYCODE_INSERT: return INSERT;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return SHIFT;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0xffe2;
            case KeyEvent.KEYCODE_CTRL_LEFT: return CTRL;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0xffe4;
            case KeyEvent.KEYCODE_ALT_LEFT: return ALT;
            case KeyEvent.KEYCODE_ALT_RIGHT: return ALTGR;
            case KeyEvent.KEYCODE_META_LEFT: return SUPER;
            case KeyEvent.KEYCODE_META_RIGHT: return 0xffec;
            case KeyEvent.KEYCODE_CAPS_LOCK: return 0xffe5;
            case KeyEvent.KEYCODE_SYSRQ: return PRINT;
            case KeyEvent.KEYCODE_MENU: return MENU;
            case KeyEvent.KEYCODE_SPACE: return ' ';
        }
        if (kc >= KeyEvent.KEYCODE_F1 && kc <= KeyEvent.KEYCODE_F12) return F1 + (kc - KeyEvent.KEYCODE_F1);
        return 0;
    }
}
