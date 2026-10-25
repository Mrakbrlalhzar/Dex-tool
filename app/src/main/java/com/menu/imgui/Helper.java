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
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class Helper {
    private static final String TAG = "ImGuiTool";
    private static GLSurfaceView overlay;
    private static volatile boolean ready = false;

    public static void attach(Context ctx) {
        if (!(ctx instanceof Activity)) return;
        final Activity activity = (Activity) ctx;
        activity.runOnUiThread(() -> attachOnUi(activity));
    }

    private static void attachOnUi(Activity activity) {
        if (overlay != null) return;
        try {
            overlay = new GLSurfaceView(activity);
            overlay.setEGLContextClientVersion(3);
            overlay.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
            overlay.setZOrderOnTop(true);
            overlay.getHolder().setFormat(PixelFormat.TRANSLUCENT);

            overlay.setRenderer(new GLSurfaceView.Renderer() {
                @Override public void onSurfaceCreated(GL10 gl, EGLConfig c) {
                    Surface s = overlay.getHolder().getSurface();
                    if (s != null) { nativeInit(s); ready = true; }
                }
                @Override public void onSurfaceChanged(GL10 gl, int w, int h) { nativeResize(w, h); }
                @Override public void onDrawFrame(GL10 gl) { if (ready) nativeRender(); }
            });
            overlay.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);

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

            ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
            decor.addView(overlay, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
            ));
            Log.i(TAG, "Overlay attached");

            new Thread(() -> {
                try {
                    List<String> classes = listAllClasses(activity);
                    nativeSetClassList(classes.toArray(new String[0]));
                    Log.i(TAG, "Class list sent: " + classes.size());
                } catch (Throwable t) {
                    Log.e(TAG, "listAllClasses failed", t);
                }
            }, "ImGuiClassLister").start();
        } catch (Throwable t) {
            Log.e(TAG, "attach failed", t);
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
                    } catch (IOException ignored) {}
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "listAllClasses error", t);
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
            // skip uleb128 length prefix
            while (p < d.length && (d[p] & 0x80) != 0) p++;
            if (p < d.length) p++;
            // read until null
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
