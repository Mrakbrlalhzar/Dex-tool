package com.menu.imgui;

import android.app.Activity;
import android.content.Context;
import android.util.Log;
import android.view.MotionEvent;
import android.view.ViewGroup;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class Helper {
    private static final String TAG = "ImGuiTool";
    private static OverlaySurfaceView overlay;

    static {
        try {
            Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
                try {
                    File f = new File(getLogDir(), "crash.log");
                    FileWriter w = new FileWriter(f, true);
                    PrintWriter pw = new PrintWriter(w);
                    pw.println("\n=== JAVA CRASH " + now() + " ===");
                    pw.println("Thread: " + t.getName());
                    e.printStackTrace(pw);
                    pw.println("=== END ===");
                    pw.close();
                } catch (Throwable ignored) { }
            });
        } catch (Throwable ignored) { }
    }

    private static String now() {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
    }

    private static String getPkgName() {
        try (FileInputStream fis = new FileInputStream("/proc/self/cmdline")) {
            byte[] buf = new byte[256];
            int n = fis.read(buf);
            if (n > 0) {
                int end = 0;
                while (end < n && buf[end] != 0) end++;
                String pkg = new String(buf, 0, end, "UTF-8");
                int colon = pkg.indexOf(':');
                return colon > 0 ? pkg.substring(0, colon) : pkg;
            }
        } catch (Throwable ignored) { }
        return "unknown";
    }

    private static String getLogDir() {
        File downloads = new File("/sdcard/Download");
        if (downloads.exists() && downloads.canWrite()) {
            File test = new File(downloads, ".wtest");
            try { test.createNewFile(); test.delete(); return downloads.getAbsolutePath(); }
            catch (Throwable ignored) { }
        }
        File ext = new File("/sdcard/Android/data/" + getPkgName());
        if (ext.exists()) return ext.getAbsolutePath();
        try { ext.mkdirs(); } catch (Throwable ignored) { }
        if (ext.exists() && ext.canWrite()) return ext.getAbsolutePath();
        return "/data/data/" + getPkgName();
    }

    private static void log(String msg) {
        Log.i(TAG, msg);
        try {
            File f = new File(getLogDir(), "crash.log");
            FileWriter w = new FileWriter(f, true);
            w.write("[J " + now() + "] " + msg + "\n");
            w.close();
        } catch (Throwable ignored) { }
    }

    public static void attach(Context ctx) {
        log("Helper.attach entered");
        if (!(ctx instanceof Activity)) return;
        final Activity activity = (Activity) ctx;
        try {
            activity.runOnUiThread(() -> attachOnUi(activity));
        } catch (Throwable t) {
            log("runOnUiThread failed: " + t);
        }
    }

    private static void attachOnUi(Activity activity) {
        if (overlay != null) return;
        try {
            overlay = new OverlaySurfaceView(activity);
            overlay.setOnTouchListener((v, e) -> {
                if (!overlay.isReady()) return false;
                int action;
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:   action = 0; break;
                    case MotionEvent.ACTION_UP:     action = 1; break;
                    case MotionEvent.ACTION_MOVE:   action = 2; break;
                    case MotionEvent.ACTION_CANCEL: action = 3; break;
                    default: return false;
                }
                return nativeTouch(action, e.getX(), e.getY());
            });
            ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
            decor.addView(overlay, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
            ));
            log("overlay attached");

            new Thread(() -> {
                try {
                    List<String> classes = listAllClasses(activity);
                    log("classLister: found " + classes.size());
                    nativeSetClassList(classes.toArray(new String[0]));
                } catch (Throwable t) {
                    log("classLister failed: " + t);
                }
            }, "ImGuiClassLister").start();
        } catch (Throwable t) {
            log("attachOnUi FAILED: " + t);
        }
    }

    private static List<String> listAllClasses(Context ctx) {
        List<String> out = new ArrayList<>();
        String apkPath;
        try { apkPath = ctx.getPackageCodePath(); }
        catch (Throwable t) { return out; }
        try (ZipFile zip = new ZipFile(apkPath)) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                String n = entry.getName();
                if (n.equals("classes.dex") || n.matches("classes\\d+\\.dex")) {
                    try (InputStream in = zip.getInputStream(entry)) {
                        byte[] data = readAll(in);
                        parseDex(data, out);
                    } catch (IOException ignored) { }
                }
            }
        } catch (Throwable t) { log("listAllClasses error: " + t); }
        Collections.sort(out);
        return out;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[8192]; int n;
        while ((n = in.read(b)) > 0) o.write(b, 0, n);
        return o.toByteArray();
    }

    private static int rd32(byte[] d, int off) {
        return (d[off] & 0xFF) | ((d[off+1] & 0xFF) << 8) |
               ((d[off+2] & 0xFF) << 16) | ((d[off+3] & 0xFF) << 24);
    }

    private static void parseDex(byte[] d, List<String> out) {
        if (d.length < 0x70) return;
        if (d[0] != 'd' || d[1] != 'e' || d[2] != 'x' || d[3] != '\n') return;
        int strIdsSize = rd32(d, 0x38);
        int strIdsOff  = rd32(d, 0x3C);
        int typeIdsSize= rd32(d, 0x40);
        int typeIdsOff = rd32(d, 0x44);
        int classDefSize=rd32(d, 0x60);
        int classDefOff= rd32(d, 0x64);
        if (classDefOff + classDefSize * 32 > d.length) return;
        for (int i = 0; i < classDefSize; i++) {
            int cd = classDefOff + i * 32;
            int classIdx = rd32(d, cd);
            if (classIdx < 0 || classIdx >= typeIdsSize) continue;
            int strIdx = rd32(d, typeIdsOff + classIdx * 4);
            if (strIdx < 0 || strIdx >= strIdsSize) continue;
            int strDataOff = rd32(d, strIdsOff + strIdx * 4);
            if (strDataOff <= 0 || strDataOff >= d.length) continue;
            int p = strDataOff;
            while (p < d.length && (d[p] & 0x80) != 0) p++;
            if (p < d.length) p++;
            int start = p;
            while (p < d.length && d[p] != 0) p++;
            if (p <= start) continue;
            String desc = new String(d, start, p - start, java.nio.charset.StandardCharsets.UTF_8);
            if (desc.startsWith("L") && desc.endsWith(";"))
                out.add(desc.substring(1, desc.length() - 1).replace('/', '.'));
            else
                out.add(desc);
        }
    }

    static native void nativeInit(android.view.Surface s);
    static native void nativeResize(int w, int h);
    static native void nativeRender();
    static native boolean nativeTouch(int action, float x, float y);
    static native void nativeSetClassList(String[] classNames);
    static native boolean nativeWantKeyboard();
    static native void nativeAddInputChar(int codepoint);
    static native void nativeAddKeyEvent(int keycode, boolean down);
}
