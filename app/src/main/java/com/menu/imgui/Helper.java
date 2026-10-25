package com.menu.imgui;

import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
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

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class Helper {
    private static final String TAG = "ImGuiTool";
    private static GLSurfaceView overlay;
    private static volatile boolean ready = false;

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
        // Primary: Downloads (needs WRITE_EXTERNAL_STORAGE)
        File downloads = new File("/sdcard/Download");
        if (downloads.exists() && downloads.canWrite()) {
            File test = new File(downloads, ".wtest");
            try {
                test.createNewFile();
                test.delete();
                return downloads.getAbsolutePath();
            } catch (Throwable ignored) { }
        }
        // Fallback: app's external data dir (no permission)
        File ext = new File("/sdcard/Android/data/" + getPkgName());
        if (ext.exists()) return ext.getAbsolutePath();
        try { ext.mkdirs(); } catch (Throwable ignored) { }
        if (ext.exists() && ext.canWrite()) return ext.getAbsolutePath();
        // Last resort
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
        log("Helper.attach entered, ctx=" + (ctx != null ? ctx.getClass().getSimpleName() : "null"));
        if (!(ctx instanceof Activity)) {
            log("Helper.attach: ctx is not Activity, abort");
            return;
        }
        final Activity activity = (Activity) ctx;
        try {
            activity.runOnUiThread(() -> attachOnUi(activity));
        } catch (Throwable t) {
            log("Helper.attach: runOnUiThread failed: " + t);
        }
    }

    private static void attachOnUi(Activity activity) {
        log("attachOnUi entered");
        if (overlay != null) {
            log("attachOnUi: overlay already set, abort");
            return;
        }
        try {
            log("attachOnUi: creating GLSurfaceView");
            overlay = new GLSurfaceView(activity);
            log("attachOnUi: setting EGL context 3");
            overlay.setEGLContextClientVersion(3);
            overlay.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
            overlay.setZOrderOnTop(true);
            overlay.getHolder().setFormat(PixelFormat.TRANSLUCENT);
            log("attachOnUi: setting renderer");
            overlay.setRenderer(new GLSurfaceView.Renderer() {
                @Override public void onSurfaceCreated(GL10 gl, EGLConfig c) {
                    log("Renderer.onSurfaceCreated");
                    Surface s = overlay.getHolder().getSurface();
                    if (s != null) {
                        log("Renderer: calling nativeInit");
                        try { nativeInit(s); }
                        catch (Throwable t) { log("nativeInit threw: " + t); }
                        ready = true;
                        log("Renderer: nativeInit returned OK");
                    } else {
                        log("Renderer: Surface is null!");
                    }
                }
                @Override public void onSurfaceChanged(GL10 gl, int w, int h) {
                    log("Renderer.onSurfaceChanged: " + w + "x" + h);
                    nativeResize(w, h);
                }
                @Override public void onDrawFrame(GL10 gl) {
                    if (ready) nativeRender();
                }
            });
            overlay.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
            log("attachOnUi: setting touch listener");
            overlay.setOnTouchListener((v, e) -> {
                if (!ready) return false;
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
            log("attachOnUi: adding to decor");
            ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
            decor.addView(overlay, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
            ));
            log("attachOnUi: overlay attached successfully");
            new Thread(() -> {
                try {
                    log("classLister: starting");
                    List<String> classes = listAllClasses(activity);
                    log("classLister: found " + classes.size() + " classes");
                    nativeSetClassList(classes.toArray(new String[0]));
                    log("classLister: sent to native");
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
        catch (Throwable t) { log("getPackageCodePath failed: " + t); return out; }
        log("listAllClasses: apk=" + apkPath);
        try (ZipFile zip = new ZipFile(apkPath)) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                String n = entry.getName();
                if (n.equals("classes.dex") || n.matches("classes\\d+\\.dex")) {
                    try (InputStream in = zip.getInputStream(entry)) {
                        byte[] data = readAll(in);
                        log("listAllClasses: parsing " + n + " (" + data.length + " bytes)");
                        parseDex(data, out);
                    } catch (IOException ignored) { }
                }
            }
        } catch (Throwable t) {
            log("listAllClasses error: " + t);
        }
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
        if (d[0] != 'd' || d[1] != 'e' || d[2] != 'x' || d[3] != '\n') {
            log("parseDex: bad magic");
            return;
        }
        int strIdsSize = rd32(d, 0x38);
        int strIdsOff  = rd32(d, 0x3C);
        int typeIdsSize= rd32(d, 0x40);
        int typeIdsOff = rd32(d, 0x44);
        int classDefSize=rd32(d, 0x60);
        int classDefOff= rd32(d, 0x64);
        if (classDefOff + classDefSize * 32 > d.length) {
            log("parseDex: classDef out of bounds");
            return;
        }
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

    private static native void nativeInit(Surface s);
    private static native void nativeResize(int w, int h);
    private static native void nativeRender();
    private static native boolean nativeTouch(int action, float x, float y);
    private static native void nativeSetClassList(String[] classNames);
}
