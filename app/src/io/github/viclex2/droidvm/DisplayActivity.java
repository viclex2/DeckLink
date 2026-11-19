package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;

/** Pantalla de la VM: visor VNC y barra de control (como la ventana de VirtualBox). */
public final class DisplayActivity extends Activity implements VncView.Host, Runner.Listener {
    private Runner runner;
    private VncView vnc;
    private LinearLayout bar, keysBar;
    private HorizontalScrollView barScroll, keysScroll;
    private TextView status;
    private Button mouseB, pauseB;
    private final Button[] mods = new Button[4];
    private boolean barShown = true;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        runner = Runner.get(getIntent().getStringExtra("id"));
        if (runner == null) { finish(); return; }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        vnc = new VncView(this);
        vnc.layout = Keys.layout(this, runner.vm.keymap.equals("ninguno") ? "en-us" : runner.vm.keymap);
        vnc.touchpad = getPreferences(0).getBoolean("touchpad", false);
        vnc.stretch = getPreferences(0).getBoolean("stretch", false);
        vnc.onStickyUsed = new Runnable() { public void run() { paintMods(); } };

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(vnc, new LinearLayout.LayoutParams(-1, 0, 1));
        keysScroll = new HorizontalScrollView(this);
        keysBar = new LinearLayout(this);
        keysScroll.addView(keysBar);
        keysScroll.setBackgroundColor(0xEE15191D);
        keysScroll.setVisibility(View.GONE);
        col.addView(keysScroll, new LinearLayout.LayoutParams(-1, -2));
        root.addView(col, new FrameLayout.LayoutParams(-1, -1));

        barScroll = new HorizontalScrollView(this);
        bar = new LinearLayout(this);
        bar.setPadding(U.dp(this, 4), 0, U.dp(this, 4), 0);
        barScroll.addView(bar);
        barScroll.setBackgroundColor(0xCC15191D);
        root.addView(barScroll, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));

        final Button toggle = small("☰");
        toggle.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { showBar(!barShown); } });
        FrameLayout.LayoutParams tl = new FrameLayout.LayoutParams(U.dp(this, 44), U.dp(this, 40), Gravity.TOP | Gravity.END);
        root.addView(toggle, tl);
        toggle.setAlpha(0.55f);

        status = new TextView(this);
        status.setTextColor(0xFFCFD8DC);
        status.setTextSize(15);
        status.setGravity(Gravity.CENTER);
        status.setPadding(U.dp(this, 24), 0, U.dp(this, 24), 0);
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));

        buildBar();
        buildKeys();
        setContentView(root);
        immersive();
        status.setText("Arrancando " + runner.vm.name + "…");
        vnc.start(runner.vncSock, runner, this);
        Runner.addListener(this);
    }

    private Button small(String t) {
        Button b = new Button(this);
        b.setText(t);
        b.setAllCaps(false);
        b.setTextSize(13);
        b.setTextColor(Color.WHITE);
        b.setMinWidth(0); b.setMinimumWidth(0);
        b.setMinHeight(0); b.setMinimumHeight(0);
        b.setPadding(U.dp(this, 10), U.dp(this, 6), U.dp(this, 10), U.dp(this, 6));
        b.setBackgroundColor(Color.TRANSPARENT);
        return b;
    }

    private Button add(LinearLayout l, String t, View.OnClickListener c) {
        Button b = small(t);
        b.setOnClickListener(c);
        l.addView(b, new LinearLayout.LayoutParams(-2, U.dp(this, 40)));
        return b;
    }

    private void buildBar() {
        add(bar, "⌨ Teclado", new View.OnClickListener() { public void onClick(View v) { keyboard(); } });
        add(bar, "Ctrl+Alt+Supr", new View.OnClickListener() { public void onClick(View v) { vnc.combo(Keys.CTRL, Keys.ALT, Keys.DELETE); } });
        add(bar, "Teclas…", new View.OnClickListener() { public void onClick(View v) { specialKeys(); } });
        mouseB = add(bar, "", new View.OnClickListener() { public void onClick(View v) {
            vnc.touchpad = !vnc.touchpad; getPreferences(0).edit().putBoolean("touchpad", vnc.touchpad).apply(); paintMouse(); vnc.invalidate(); } });
        paintMouse();
        add(bar, "Escribir texto", new View.OnClickListener() { public void onClick(View v) {
            U.input(DisplayActivity.this, "Escribir en la VM", "Texto (contraseñas, comandos…)", "", false, new U.Cb<String>() {
                public void on(String s) { vnc.typeText(s); } }); } });
        pauseB = add(bar, "⏸ Pausa", new View.OnClickListener() { public void onClick(View v) { pause(); } });
        add(bar, "📷 Instantánea", new View.OnClickListener() { public void onClick(View v) { Snapshots.show(DisplayActivity.this, runner.vm, runner); } });
        add(bar, "💿 CD/ISO", new View.OnClickListener() { public void onClick(View v) { cdMenu(); } });
        add(bar, "⟳ Reiniciar", new View.OnClickListener() { public void onClick(View v) {
            U.confirm(DisplayActivity.this, "Reiniciar", "¿Reiniciar la máquina? (como pulsar el botón de reset)", "Reiniciar",
                    new Runnable() { public void run() { qmpBg("system_reset"); } }); } });
        add(bar, "⏻ Apagar", new View.OnClickListener() { public void onClick(View v) { powerMenu(); } });
        add(bar, "Pantalla", new View.OnClickListener() { public void onClick(View v) {
            vnc.stretch = !vnc.stretch; getPreferences(0).edit().putBoolean("stretch", vnc.stretch).apply(); vnc.invalidate();
            U.toast(DisplayActivity.this, vnc.stretch ? "Pantalla estirada" : "Pantalla ajustada (proporción original)"); } });
        add(bar, "Registro", new View.OnClickListener() { public void onClick(View v) { showLog(runner); } });
        add(bar, "Salir", new View.OnClickListener() { public void onClick(View v) { finish(); } });
        View sp = new View(this);
        bar.addView(sp, new LinearLayout.LayoutParams(U.dp(this, 44), 1));
    }

    private void buildKeys() {
        String[] ml = {"Ctrl", "Alt", "Shift", "Win"};
        for (int i = 0; i < 4; i++) {
            final int bit = 1 << i;
            mods[i] = add(keysBar, ml[i], new View.OnClickListener() { public void onClick(View v) { vnc.sticky ^= bit; paintMods(); } });
        }
        Object[][] ks = {{"Esc", Keys.ESC}, {"Tab", Keys.TAB}, {"↑", Keys.UP}, {"↓", Keys.DOWN}, {"←", Keys.LEFT}, {"→", Keys.RIGHT},
                {"Supr", Keys.DELETE}, {"Inicio", Keys.HOME}, {"Fin", Keys.END}, {"RePág", Keys.PGUP}, {"AvPág", Keys.PGDN}};
        for (final Object[] k : ks) add(keysBar, (String) k[0], new View.OnClickListener() { public void onClick(View v) { vnc.keyWithSticky((Integer) k[1]); } });
        for (int i = 1; i <= 12; i++) {
            final int f = Keys.F1 + i - 1;
            add(keysBar, "F" + i, new View.OnClickListener() { public void onClick(View v) { vnc.keyWithSticky(f); } });
        }
        paintMods();
    }

    private void paintMods() {
        for (int i = 0; i < 4; i++) mods[i].setTextColor((vnc.sticky & (1 << i)) != 0 ? U.ACCENT : Color.WHITE);
    }

    private void paintMouse() { mouseB.setText(vnc.touchpad ? "🖱 Touchpad" : "👆 Táctil"); }

    private void showBar(boolean s) {
        barShown = s;
        barScroll.setVisibility(s ? View.VISIBLE : View.GONE);
    }

    private void keyboard() {
        vnc.requestFocus();
        InputMethodManager im = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        boolean show = keysScroll.getVisibility() != View.VISIBLE;
        keysScroll.setVisibility(show ? View.VISIBLE : View.GONE);
        if (show) im.showSoftInput(vnc, InputMethodManager.SHOW_FORCED);
        else im.hideSoftInputFromWindow(vnc.getWindowToken(), 0);
    }

    private void specialKeys() {
        final String[] n = {"Win (menú Inicio)", "Alt+Tab", "Alt+F4", "Win+R (Ejecutar)", "Shift+F10 (consola en la instalación de Windows)",
                "Ctrl+Shift+Esc (Administrador de tareas)", "Imprimir pantalla", "Ctrl+Alt+F1", "Ctrl+Alt+F2", "Ctrl+Alt+F7",
                "Ctrl+C", "Ctrl+V", "Esc", "F2", "F12", "Supr (entrar en la BIOS/UEFI)"};
        final int[][] k = {{Keys.SUPER}, {Keys.ALT, Keys.TAB}, {Keys.ALT, Keys.F1 + 3}, {Keys.SUPER, 'r'}, {Keys.SHIFT, Keys.F1 + 9},
                {Keys.CTRL, Keys.SHIFT, Keys.ESC}, {Keys.PRINT}, {Keys.CTRL, Keys.ALT, Keys.F1}, {Keys.CTRL, Keys.ALT, Keys.F1 + 1},
                {Keys.CTRL, Keys.ALT, Keys.F1 + 6}, {Keys.CTRL, 'c'}, {Keys.CTRL, 'v'}, {Keys.ESC}, {Keys.F1 + 1}, {Keys.F1 + 11}, {Keys.DELETE}};
        U.choose(this, "Enviar teclas", n, new U.Cb<Integer>() { public void on(Integer i) { vnc.combo(k[i]); } });
    }

    private void pause() {
        U.bg(new Runnable() { public void run() {
            try {
                boolean p = runner.queryPaused();
                runner.simple(p ? "cont" : "stop");
                runner.paused = !p;
                U.ui(new Runnable() { public void run() {
                    pauseB.setText(runner.paused ? "▶ Reanudar" : "⏸ Pausa");
                    status.setText(runner.paused ? "En pausa" : "");
                } });
            } catch (Exception e) { U.toast(DisplayActivity.this, "Error: " + e.getMessage()); }
        } });
    }

    private void qmpBg(final String cmd) {
        U.bg(new Runnable() { public void run() {
            try { runner.simple(cmd); } catch (Exception e) { U.toast(DisplayActivity.this, "Error: " + e.getMessage()); }
        } });
    }

    private void powerMenu() {
        U.choose(this, "Apagar " + runner.vm.name, new String[]{"Apagado ordenado (botón de encendido ACPI)", "Forzar apagado (como quitar la corriente)",
                "Guardar estado y apagar"}, new U.Cb<Integer>() { public void on(Integer w) {
            if (w == 0) { qmpBg("system_powerdown"); U.toast(DisplayActivity.this, "Señal de apagado enviada al sistema invitado"); }
            else if (w == 1) U.confirm(DisplayActivity.this, "Forzar apagado", "Se perderá lo que no esté guardado dentro de la VM.", "Apagar",
                    new Runnable() { public void run() { runner.kill(); } });
            else Snapshots.saveAndQuit(DisplayActivity.this, runner);
        } });
    }

    private void cdMenu() {
        U.choose(this, "Unidad de CD/DVD", new String[]{"Insertar otra ISO…", "Expulsar"}, new U.Cb<Integer>() { public void on(Integer w) {
            if (w == 0) Pick.file(DisplayActivity.this, 1);
            else U.bg(new Runnable() { public void run() {
                try { runner.hmp("eject -f cd0", 15000); U.toast(DisplayActivity.this, "CD expulsado"); }
                catch (Exception e) { U.toast(DisplayActivity.this, "Error: " + e.getMessage()); }
            } });
        } });
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        File base = getExternalFilesDir(null);
        Pick.resolve(this, u, new File(base != null ? base : getFilesDir(), "isos"), new U.Cb<String>() { public void on(final String p) {
            U.bg(new Runnable() { public void run() {
                try {
                    JSONObject a = new JSONObject();
                    a.put("device", "cd0"); a.put("filename", p); a.put("format", "raw");
                    try { runner.qmp("blockdev-change-medium", a, 30000); }
                    catch (Exception e) { runner.hmp("change cd0 " + p + " raw", 30000); }
                    Vm v = Vm.load(DisplayActivity.this, runner.vm.id);
                    if (v != null) { v.iso = p; v.save(DisplayActivity.this); }
                    U.toast(DisplayActivity.this, "ISO insertada");
                } catch (Exception e) { U.toast(DisplayActivity.this, "Error: " + e.getMessage()); }
            } });
        } });
    }

    static void showLog(Activity a, Runner r) {
        ScrollView sv = new ScrollView(a);
        TextView t = new TextView(a);
        t.setTextIsSelectable(true);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        t.setTextSize(11);
        t.setPadding(U.dp(a, 12), U.dp(a, 8), U.dp(a, 12), U.dp(a, 8));
        t.setText(r.log());
        sv.addView(t);
        new AlertDialog.Builder(a).setTitle("Registro de QEMU").setView(sv).setPositiveButton("Cerrar", null).show();
        final ScrollView fsv = sv;
        sv.post(new Runnable() { public void run() { fsv.fullScroll(View.FOCUS_DOWN); } });
    }

    private void showLog(Runner r) { showLog(this, r); }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (vnc != null && vnc.connected() && vnc.handleKey(e)) return true;
        return super.dispatchKeyEvent(e);
    }

    // VncView.Host
    public void onStatus(String s) { if (!runner.paused) status.setText(s); }

    public void onConnected(int w, int h) {
        status.setText("");
        U.MAIN.postDelayed(new Runnable() { public void run() { if (barShown && vnc.connected()) showBar(false); } }, 4000);
    }

    public void onDisconnected() {
        if (!runner.alive) {
            status.setText(runner.vm.name + " se ha apagado.\n\n" + (runner.exitCode != 0 ? runner.lastLines(12) : ""));
        }
    }

    // Runner.Listener
    public void changed() {
        if (runner != null && !runner.alive) {
            vnc.stop();
            status.setText(runner.vm.name + " se ha apagado." + (runner.exitCode != 0 ? "\n\n" + runner.lastLines(12) : ""));
            showBar(true);
        }
    }

    @Override protected void onDestroy() {
        Runner.removeListener(this);
        if (vnc != null) vnc.stop();
        super.onDestroy();
    }
}
