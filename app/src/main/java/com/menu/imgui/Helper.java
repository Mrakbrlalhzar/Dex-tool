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
import java.nio.charset.StandardCharsets;
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
        try { activity.runOnUiThread(() -> attachOnUi(activity)); }
        catch (Throwable t) { log("runOnUiThread failed: " + t); }
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
                } catch (Throwable t) { log("classLister failed: " + t); }
            }, "ImGuiClassLister").start();
        } catch (Throwable t) { log("attachOnUi FAILED: " + t); }
    }

    // ==================== Class list ====================
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
                        parseDexClasses(data, out);
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

    private static int rd16(byte[] d, int off) {
        return (d[off] & 0xFF) | ((d[off + 1] & 0xFF) << 8);
    }
    private static int rd32(byte[] d, int off) {
        return (d[off] & 0xFF) | ((d[off+1] & 0xFF) << 8) |
               ((d[off+2] & 0xFF) << 16) | ((d[off+3] & 0xFF) << 24);
    }
    private static int readUleb128(byte[] d, int[] pos) {
        int result = 0;
        int shift = 0;
        while (true) {
            byte b = d[pos[0]++];
            result |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return result;
    }
    private static String readString(byte[] d, int strIdsOff, int strIdx) {
        if (strIdx < 0) return "";
        int strDataOff = rd32(d, strIdsOff + strIdx * 4);
        if (strDataOff <= 0 || strDataOff >= d.length) return "";
        int p = strDataOff;
        while (p < d.length && (d[p] & 0x80) != 0) p++;
        if (p < d.length) p++;
        int start = p;
        while (p < d.length && d[p] != 0) p++;
        return new String(d, start, p - start, StandardCharsets.UTF_8);
    }
    private static String getTypeDesc(byte[] d, int typeIdsOff, int strIdsOff, int typeIdx) {
        int strIdx = rd32(d, typeIdsOff + typeIdx * 4);
        return readString(d, strIdsOff, strIdx);
    }
    private static String prettyType(String desc) {
        if (desc == null || desc.isEmpty()) return "?";
        switch (desc.charAt(0)) {
            case 'V': return "void";
            case 'Z': return "boolean";
            case 'B': return "byte";
            case 'S': return "short";
            case 'C': return "char";
            case 'I': return "int";
            case 'J': return "long";
            case 'F': return "float";
            case 'D': return "double";
            case 'L':
                if (desc.endsWith(";")) return desc.substring(1, desc.length() - 1).replace('/', '.');
                return desc;
            case '[': return prettyType(desc.substring(1)) + "[]";
            default: return desc;
        }
    }

    private static void parseDexClasses(byte[] d, List<String> out) {
        if (d.length < 0x70) return;
        if (d[0] != 'd' || d[1] != 'e' || d[2] != 'x' || d[3] != '\n') return;
        int strIdsOff   = rd32(d, 0x3C);
        int typeIdsSize = rd32(d, 0x40);
        int typeIdsOff  = rd32(d, 0x44);
        int classDefSize= rd32(d, 0x60);
        int classDefOff = rd32(d, 0x64);
        if (classDefOff + classDefSize * 32 > d.length) return;
        for (int i = 0; i < classDefSize; i++) {
            int cd = classDefOff + i * 32;
            int classIdx = rd32(d, cd);
            if (classIdx < 0 || classIdx >= typeIdsSize) continue;
            String desc = getTypeDesc(d, typeIdsOff, strIdsOff, classIdx);
            if (desc.startsWith("L") && desc.endsWith(";"))
                out.add(desc.substring(1, desc.length() - 1).replace('/', '.'));
        }
    }

    // ==================== Class details ====================
    public static void requestClassDetails(int sectionId, String className) {
        log("requestClassDetails: sec=" + sectionId + " " + className);
        new Thread(() -> {
            String[] fields = new String[0];
            String[] methods = new String[0];
            try {
                ClassMembers m = parseClassMembers(className);
                fields = m.fields.toArray(new String[0]);
                methods = m.methods.toArray(new String[0]);
                log("classDetails: sec=" + sectionId + " fields=" + fields.length + " methods=" + methods.length);
            } catch (Throwable t) {
                log("requestClassDetails failed: " + t);
            }
            nativeSetClassDetails(sectionId, className, fields, methods);
        }, "ImGuiClassDetails").start();
    }

    private static class ClassMembers {
        List<String> fields = new ArrayList<>();
        List<String> methods = new ArrayList<>();
    }

    private static ClassMembers parseClassMembers(String className) {
        ClassMembers out = new ClassMembers();
        String targetDesc = "L" + className.replace('.', '/') + ";";

        File apkFile = new File(getApkPath());
        try (ZipFile zip = new ZipFile(apkFile)) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                String n = entry.getName();
                if (n.equals("classes.dex") || n.matches("classes\\d+\\.dex")) {
                    try (InputStream in = zip.getInputStream(entry)) {
                        byte[] data = readAll(in);
                        parseDexMembers(data, targetDesc, out);
                    } catch (IOException ignored) { }
                }
            }
        } catch (Throwable t) { log("parseClassMembers error: " + t); }
        return out;
    }

    private static String cachedApkPath = null;
    private static String getApkPath() {
        if (cachedApkPath != null) return cachedApkPath;
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object app = at.getMethod("currentApplication").invoke(null);
            if (app instanceof android.content.Context) {
                cachedApkPath = ((android.content.Context) app).getPackageCodePath();
                if (cachedApkPath != null) return cachedApkPath;
            }
        } catch (Throwable ignored) { }
        return "/system/framework/framework.jar";
    }

    private static void parseDexMembers(byte[] d, String targetDesc, ClassMembers out) {
        if (d.length < 0x70) return;
        if (d[0] != 'd' || d[1] != 'e' || d[2] != 'x' || d[3] != '\n') return;
        int strIdsOff    = rd32(d, 0x3C);
        int typeIdsSize  = rd32(d, 0x40);
        int typeIdsOff   = rd32(d, 0x44);
        int protoIdsOff  = rd32(d, 0x4C);
        int fieldIdsOff  = rd32(d, 0x54);
        int methodIdsOff = rd32(d, 0x5C);
        int classDefSize = rd32(d, 0x60);
        int classDefOff  = rd32(d, 0x64);
        if (classDefOff + classDefSize * 32 > d.length) return;

        for (int i = 0; i < classDefSize; i++) {
            int cd = classDefOff + i * 32;
            int classIdx = rd32(d, cd);
            if (classIdx < 0 || classIdx >= typeIdsSize) continue;
            String desc = getTypeDesc(d, typeIdsOff, strIdsOff, classIdx);
            if (!targetDesc.equals(desc)) continue;

            int classDataOff = rd32(d, cd + 24);
            if (classDataOff == 0) return;

            int[] pos = { classDataOff };
            int staticFieldsSize   = readUleb128(d, pos);
            int instanceFieldsSize = readUleb128(d, pos);
            int directMethodsSize  = readUleb128(d, pos);
            int virtualMethodsSize = readUleb128(d, pos);

            int fieldIdx = 0;
            for (int j = 0; j < staticFieldsSize; j++) {
                int diff = readUleb128(d, pos);
                readUleb128(d, pos);
                fieldIdx += diff;
                out.fields.add(getFieldSig(d, fieldIdsOff, typeIdsOff, strIdsOff, fieldIdx) + "  [static]");
            }
            fieldIdx = 0;
            for (int j = 0; j < instanceFieldsSize; j++) {
                int diff = readUleb128(d, pos);
                readUleb128(d, pos);
                fieldIdx += diff;
                out.fields.add(getFieldSig(d, fieldIdsOff, typeIdsOff, strIdsOff, fieldIdx) + "  [instance]");
            }
            int methodIdx = 0;
            for (int j = 0; j < directMethodsSize; j++) {
                int diff = readUleb128(d, pos);
                readUleb128(d, pos);
                readUleb128(d, pos);
                methodIdx += diff;
                out.methods.add(getMethodSig(d, methodIdsOff, protoIdsOff, typeIdsOff, strIdsOff, methodIdx) + "  [direct]");
            }
            methodIdx = 0;
            for (int j = 0; j < virtualMethodsSize; j++) {
                int diff = readUleb128(d, pos);
                readUleb128(d, pos);
                readUleb128(d, pos);
                methodIdx += diff;
                out.methods.add(getMethodSig(d, methodIdsOff, protoIdsOff, typeIdsOff, strIdsOff, methodIdx) + "  [virtual]");
            }
            return;
        }
    }

    private static String getFieldSig(byte[] d, int fieldIdsOff, int typeIdsOff, int strIdsOff, int fieldIdx) {
        int fi = fieldIdsOff + fieldIdx * 8;
        int typeIdx = rd16(d, fi + 2);
        int nameIdx = rd32(d, fi + 4);
        String name = readString(d, strIdsOff, nameIdx);
        String type = prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, typeIdx));
        return type + " " + name;
    }

    private static String getMethodSig(byte[] d, int methodIdsOff, int protoIdsOff, int typeIdsOff, int strIdsOff, int methodIdx) {
        int mi = methodIdsOff + methodIdx * 8;
        int protoIdx = rd16(d, mi + 2);
        int nameIdx  = rd32(d, mi + 4);
        String name = readString(d, strIdsOff, nameIdx);
        if ("<init>".equals(name))   name = "<init> (constructor)";
        if ("<clinit>".equals(name)) name = "<clinit> (static init)";

        int pi = protoIdsOff + protoIdx * 12;
        int returnTypeIdx = rd32(d, pi + 4);
        int paramsOff     = rd32(d, pi + 8);

        String returnType = prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, returnTypeIdx));

        StringBuilder sb = new StringBuilder();
        sb.append(returnType).append(" ").append(name).append("(");
        if (paramsOff != 0 && paramsOff + 4 <= d.length) {
            int count = rd32(d, paramsOff);
            for (int j = 0; j < count; j++) {
                int off = paramsOff + 4 + j * 2;
                if (off + 2 > d.length) break;
                int typeIdx = rd16(d, off);
                if (j > 0) sb.append(", ");
                sb.append(prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, typeIdx)));
            }
        }
        sb.append(")");
        return sb.toString();
    }

    static native void nativeInit(android.view.Surface s);
    static native void nativeResize(int w, int h);
    static native void nativeRender();
    static native boolean nativeTouch(int action, float x, float y);
    static native void nativeSetClassList(String[] classNames);
    static native boolean nativeWantKeyboard();
    static native void nativeAddInputChar(int codepoint);
    static native void nativeAddKeyEvent(int keycode, boolean down);
    static native void nativeSetClassDetails(int sectionId, String className, String[] fields, String[] methods);
}
