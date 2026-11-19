package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/** Utilidades de interfaz e hilos (sin lambdas: se compila con dx). */
final class U {
    static final Handler MAIN = new Handler(Looper.getMainLooper());
    static final int ACCENT = 0xFF4FC3F7;
    static final int CARD = 0xFF1E2329;

    interface Cb<T> { void on(T v); }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static void ui(Runnable r) { MAIN.post(r); }

    static void bg(Runnable r) { Thread t = new Thread(r, "droidvm-bg"); t.setDaemon(true); t.start(); }

    static void toast(final Context c, final String s) {
        ui(new Runnable() { public void run() { Toast.makeText(c, s, Toast.LENGTH_LONG).show(); } });
    }

    static void alert(final Activity a, final String title, final String msg) {
        ui(new Runnable() { public void run() {
            if (a.isFinishing()) return;
            new AlertDialog.Builder(a).setTitle(title).setMessage(msg).setPositiveButton("Vale", null).show();
        } });
    }

    static void confirm(Activity a, String title, String msg, String yes, final Runnable r) {
        new AlertDialog.Builder(a).setTitle(title).setMessage(msg)
            .setPositiveButton(yes, new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface d, int w) { r.run(); } })
            .setNegativeButton("Cancelar", null).show();
    }

    static void input(Activity a, String title, String hint, String def, boolean number, final Cb<String> cb) {
        final EditText e = new EditText(a);
        e.setHint(hint);
        if (def != null) e.setText(def);
        if (number) e.setInputType(InputType.TYPE_CLASS_NUMBER);
        LinearLayout l = new LinearLayout(a);
        l.setPadding(dp(a, 20), dp(a, 8), dp(a, 20), 0);
        l.addView(e, new LinearLayout.LayoutParams(-1, -2));
        new AlertDialog.Builder(a).setTitle(title).setView(l)
            .setPositiveButton("Aceptar", new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface d, int w) { cb.on(e.getText().toString().trim()); } })
            .setNegativeButton("Cancelar", null).show();
    }

    static void choose(Activity a, String title, final String[] items, final Cb<Integer> cb) {
        new AlertDialog.Builder(a).setTitle(title).setItems(items, new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface d, int w) { cb.on(w); } }).show();
    }

    static ProgressDialog progress(Activity a, String msg) {
        ProgressDialog p = new ProgressDialog(a);
        p.setMessage(msg);
        p.setCancelable(false);
        p.show();
        return p;
    }

    static void dismiss(final ProgressDialog p) {
        ui(new Runnable() { public void run() { try { p.dismiss(); } catch (Exception ignored) {} } });
    }

    static void setProgress(final ProgressDialog p, final String msg) {
        ui(new Runnable() { public void run() { p.setMessage(msg); } });
    }

    static GradientDrawable round(int color, float radiusPx) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radiusPx);
        return g;
    }

    static TextView text(Context c, String s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    static TextView section(Context c, String s) {
        TextView t = text(c, s, 13, ACCENT);
        t.setPadding(0, dp(c, 18), 0, dp(c, 4));
        t.setAllCaps(true);
        return t;
    }

    static Button button(Context c, String s, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    static Spinner spinner(Context c, String[] items, int sel) {
        Spinner s = new Spinner(c);
        ArrayAdapter<String> ad = new ArrayAdapter<String>(c, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(ad);
        if (sel >= 0 && sel < items.length) s.setSelection(sel);
        return s;
    }

    static int indexOf(String[] arr, String v) {
        for (int i = 0; i < arr.length; i++) if (arr[i].equals(v)) return i;
        return 0;
    }

    static EditText field(Context c, String val, boolean number) {
        EditText e = new EditText(c);
        e.setText(val);
        e.setSingleLine(true);
        if (number) e.setInputType(InputType.TYPE_CLASS_NUMBER);
        return e;
    }

    static CheckBox check(Context c, String s, boolean v) {
        CheckBox b = new CheckBox(c);
        b.setText(s);
        b.setChecked(v);
        return b;
    }

    static LinearLayout row(Context c, View... vs) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        for (View v : vs) l.addView(v, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return l;
    }

    static LinearLayout labeled(Context c, String label, View v) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        TextView t = text(c, label, 13, 0xFFB0BEC5);
        t.setPadding(0, dp(c, 8), 0, 0);
        l.addView(t);
        l.addView(v, new LinearLayout.LayoutParams(-1, -2));
        return l;
    }

    static String human(long bytes) {
        if (bytes < 1024) return bytes + " B";
        double v = bytes;
        String[] u = {"KB", "MB", "GB", "TB"};
        int i = -1;
        while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
        return String.format(java.util.Locale.US, "%.1f %s", v, u[i]);
    }

    interface Progress { void on(long done, long total); }

    static void copy(InputStream in, OutputStream out, long total, Progress p) throws IOException {
        byte[] b = new byte[1 << 20];
        long done = 0, last = 0;
        int n;
        try {
            while ((n = in.read(b)) > 0) {
                out.write(b, 0, n);
                done += n;
                if (p != null && done - last > (8 << 20)) { last = done; p.on(done, total); }
            }
        } finally {
            try { in.close(); } catch (IOException ignored) {}
            out.close();
        }
    }

    static void copyFile(File a, File b, Progress p) throws IOException {
        copy(new FileInputStream(a), new FileOutputStream(b), a.length(), p);
    }

    static void rmrf(File f) {
        if (f.isDirectory()) {
            File[] l = f.listFiles();
            if (l != null) for (File x : l) rmrf(x);
        }
        f.delete();
    }

    /** Separa argumentos respetando comillas simples y dobles. */
    static List<String> splitArgs(String s) {
        List<String> r = new ArrayList<String>();
        if (s == null) return r;
        StringBuilder cur = new StringBuilder();
        char q = 0;
        boolean has = false;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (q != 0) {
                if (ch == q) q = 0; else cur.append(ch);
            } else if (ch == '"' || ch == '\'') {
                q = ch; has = true;
            } else if (Character.isWhitespace(ch)) {
                if (has || cur.length() > 0) { r.add(cur.toString()); cur.setLength(0); has = false; }
            } else {
                cur.append(ch);
            }
        }
        if (has || cur.length() > 0) r.add(cur.toString());
        return r;
    }

    static int color(String hex) { return Color.parseColor(hex); }
}
