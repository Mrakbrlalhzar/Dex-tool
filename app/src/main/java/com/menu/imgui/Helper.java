package com.menu.imgui;

import android.app.Activity;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
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
import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class Helper {
    private static final String TAG = "ImGuiTool";
    private static OverlaySurfaceView overlay;
    private static final Map<String, Object> sInstances = new HashMap<>();

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

    private static String now() { return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date()); }
    private static String getPkgName() {
        try (FileInputStream fis = new FileInputStream("/proc/self/cmdline")) {
            byte[] buf = new byte[256]; int n = fis.read(buf);
            if (n > 0) { int end = 0; while (end < n && buf[end] != 0) end++; String pkg = new String(buf, 0, end, "UTF-8"); int colon = pkg.indexOf(':'); return colon > 0 ? pkg.substring(0, colon) : pkg; }
        } catch (Throwable ignored) { }
        return "unknown";
    }
    private static String getLogDir() {
        File d = new File("/sdcard/Download"); if (d.exists() && d.canWrite()) return d.getAbsolutePath();
        File e = new File("/sdcard/Android/data/" + getPkgName()); if (e.exists() && e.canWrite()) return e.getAbsolutePath();
        return "/data/data/" + getPkgName();
    }
    private static void log(String msg) {
        Log.i(TAG, msg);
        try { File f = new File(getLogDir(), "crash.log"); FileWriter w = new FileWriter(f, true); w.write("[J " + now() + "] " + msg + "\n"); w.close(); } catch (Throwable ignored) { }
    }

    public static void attach(Context ctx) {
        if (!(ctx instanceof Activity)) return;
        final Activity activity = (Activity) ctx;
        activity.runOnUiThread(() -> {
            if (overlay != null) return;
            try {
                overlay = new OverlaySurfaceView(activity);
                overlay.setOnTouchListener((v, e) -> {
                    if (!overlay.isReady()) return false;
                    int action = -1;
                    switch (e.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:   action = 0; break;
                        case MotionEvent.ACTION_UP:     action = 1; break;
                        case MotionEvent.ACTION_MOVE:   action = 2; break;
                        case MotionEvent.ACTION_CANCEL: action = 3; break;
                    }
                    if (action == -1) return false;
                    return nativeTouch(action, e.getX(), e.getY());
                });
                ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
                decor.addView(overlay, new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                new Thread(() -> {
                    try {
                        List<String> classes = listAllClasses(activity);
                        nativeSetClassList(classes.toArray(new String[0]));
                    } catch (Throwable t) { log("classLister failed: " + t); }
                }, "ImGuiClassLister").start();
            } catch (Throwable t) { log("attach failed: " + t); }
        });
    }

    private static List<String> listAllClasses(Context ctx) {
        List<String> out = new ArrayList<>();
        try (ZipFile zip = new ZipFile(ctx.getPackageCodePath())) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                String n = entry.getName();
                if (n.equals("classes.dex") || n.matches("classes\\d+\\.dex")) {
                    try (InputStream in = zip.getInputStream(entry)) { parseDexClasses(readAll(in), out); } catch (IOException ignored) { }
                }
            }
        } catch (Throwable t) { log("listAllClasses error: " + t); }
        Collections.sort(out); return out;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream(); byte[] b = new byte[8192]; int n;
        while ((n = in.read(b)) > 0) o.write(b, 0, n); return o.toByteArray();
    }
    private static int rd16(byte[] d, int off) { return (d[off] & 0xFF) | ((d[off+1] & 0xFF) << 8); }
    private static int rd32(byte[] d, int off) { return (d[off]&0xFF) | ((d[off+1]&0xFF)<<8) | ((d[off+2]&0xFF)<<16) | ((d[off+3]&0xFF)<<24); }
    private static int readUleb128(byte[] d, int[] pos) {
        int r = 0, s = 0; while (true) { byte b = d[pos[0]++]; r |= (b & 0x7F) << s; if ((b & 0x80) == 0) break; s += 7; } return r;
    }
    private static String readString(byte[] d, int strIdsOff, int strIdx) {
        if (strIdx < 0) return "";
        int strDataOff = rd32(d, strIdsOff + strIdx * 4);
        if (strDataOff <= 0 || strDataOff >= d.length) return "";
        int p = strDataOff; while (p < d.length && (d[p] & 0x80) != 0) p++; if (p < d.length) p++;
        int start = p; while (p < d.length && d[p] != 0) p++;
        return new String(d, start, p - start, StandardCharsets.UTF_8);
    }
    private static String getTypeDesc(byte[] d, int typeIdsOff, int strIdsOff, int typeIdx) { return readString(d, strIdsOff, rd32(d, typeIdsOff + typeIdx * 4)); }
    private static String prettyType(String desc) {
        if (desc == null || desc.isEmpty()) return "?";
        switch (desc.charAt(0)) {
            case 'V': return "void"; case 'Z': return "boolean"; case 'B': return "byte"; case 'S': return "short";
            case 'C': return "char"; case 'I': return "int"; case 'J': return "long"; case 'F': return "float"; case 'D': return "double";
            case 'L': return desc.endsWith(";") ? desc.substring(1, desc.length() - 1).replace('/', '.') : desc;
            case '[': return prettyType(desc.substring(1)) + "[]";
            default: return desc;
        }
    }

    private static void parseDexClasses(byte[] d, List<String> out) {
        if (d.length < 0x70 || d[0] != 'd' || d[1] != 'e' || d[2] != 'x') return;
        int strIdsOff = rd32(d, 0x3C); int typeIdsSize = rd32(d, 0x40); int typeIdsOff = rd32(d, 0x44);
        int classDefSize= rd32(d, 0x60); int classDefOff = rd32(d, 0x64);
        if (classDefOff + classDefSize * 32 > d.length) return;
        for (int i = 0; i < classDefSize; i++) {
            int cd = classDefOff + i * 32; int classIdx = rd32(d, cd);
            if (classIdx < 0 || classIdx >= typeIdsSize) continue;
            String desc = getTypeDesc(d, typeIdsOff, strIdsOff, classIdx);
            if (desc.startsWith("L") && desc.endsWith(";")) out.add(desc.substring(1, desc.length() - 1).replace('/', '.'));
        }
    }

    public static void requestClassDetails(int sectionId, String className) {
        new Thread(() -> {
            String[] fields = new String[0]; String[] methods = new String[0];
            try { ClassMembers m = parseClassMembers(className); fields = m.fields.toArray(new String[0]); methods = m.methods.toArray(new String[0]); }
            catch (Throwable t) { log("requestClassDetails failed: " + t); }
            nativeSetClassDetails(sectionId, className, fields, methods);
        }).start();
    }

    private static class ClassMembers { List<String> fields = new ArrayList<>(); List<String> methods = new ArrayList<>(); }

    private static ClassMembers parseClassMembers(String className) {
        ClassMembers out = new ClassMembers(); String targetDesc = "L" + className.replace('.', '/') + ";";
        try (ZipFile zip = new ZipFile(getApkPath())) {
            for (ZipEntry entry : Collections.list(zip.entries())) {
                String n = entry.getName();
                if (n.equals("classes.dex") || n.matches("classes\\d+\\.dex")) {
                    try (InputStream in = zip.getInputStream(entry)) { parseDexMembers(readAll(in), targetDesc, out); } catch (IOException ignored) { }
                }
            }
        } catch (Throwable t) { log("parseClassMembers error: " + t); }
        return out;
    }

    private static String cachedApkPath = null;
    private static String getApkPath() {
        if (cachedApkPath != null) return cachedApkPath;
        try { Class<?> at = Class.forName("android.app.ActivityThread"); Object app = at.getMethod("currentApplication").invoke(null); if (app instanceof Context) { cachedApkPath = ((Context) app).getPackageCodePath(); if (cachedApkPath != null) return cachedApkPath; } } catch (Throwable ignored) { }
        return "/system/framework/framework.jar";
    }

    private static void parseDexMembers(byte[] d, String targetDesc, ClassMembers out) {
        if (d.length < 0x70 || d[0] != 'd' || d[1] != 'e' || d[2] != 'x') return;
        int strIdsOff=rd32(d,0x3C); int typeIdsSize=rd32(d,0x40); int typeIdsOff=rd32(d,0x44);
        int protoIdsOff=rd32(d,0x4C); int fieldIdsOff=rd32(d,0x54); int methodIdsOff=rd32(d,0x5C);
        int classDefSize=rd32(d,0x60); int classDefOff=rd32(d,0x64);
        if (classDefOff + classDefSize * 32 > d.length) return;

        for (int i = 0; i < classDefSize; i++) {
            int cd = classDefOff + i * 32; int classIdx = rd32(d, cd);
            if (classIdx < 0 || classIdx >= typeIdsSize) continue;
            if (!targetDesc.equals(getTypeDesc(d, typeIdsOff, strIdsOff, classIdx))) continue;
            int classDataOff = rd32(d, cd + 24); if (classDataOff == 0) return;

            int[] pos = { classDataOff };
            int staticFields=readUleb128(d,pos); int instanceFields=readUleb128(d,pos);
            int directMethods=readUleb128(d,pos); int virtualMethods=readUleb128(d,pos);

            int fieldIdx = 0;
            for (int j = 0; j < staticFields; j++) { int diff = readUleb128(d, pos); readUleb128(d, pos); fieldIdx += diff; out.fields.add("[static] " + getFieldSig(d, fieldIdsOff, typeIdsOff, strIdsOff, fieldIdx)); }
            fieldIdx = 0;
            for (int j = 0; j < instanceFields; j++) { int diff = readUleb128(d, pos); readUleb128(d, pos); fieldIdx += diff; out.fields.add("[instance] " + getFieldSig(d, fieldIdsOff, typeIdsOff, strIdsOff, fieldIdx)); }
            int methodIdx = 0;
            for (int j = 0; j < directMethods; j++) { int diff = readUleb128(d, pos); readUleb128(d, pos); readUleb128(d, pos); methodIdx += diff; out.methods.add(getMethodRaw(d, methodIdsOff, protoIdsOff, typeIdsOff, strIdsOff, methodIdx)); }
            methodIdx = 0;
            for (int j = 0; j < virtualMethods; j++) { int diff = readUleb128(d, pos); readUleb128(d, pos); readUleb128(d, pos); methodIdx += diff; out.methods.add(getMethodRaw(d, methodIdsOff, protoIdsOff, typeIdsOff, strIdsOff, methodIdx)); }
            return;
        }
    }

    private static String getFieldSig(byte[] d, int fieldIdsOff, int typeIdsOff, int strIdsOff, int fieldIdx) {
        int fi = fieldIdsOff + fieldIdx * 8; String name = readString(d, strIdsOff, rd32(d, fi + 4));
        String type = prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, rd16(d, fi + 2))); return type + " " + name;
    }

    private static String getMethodRaw(byte[] d, int methodIdsOff, int protoIdsOff, int typeIdsOff, int strIdsOff, int methodIdx) {
        int mi = methodIdsOff + methodIdx * 8; int protoIdx = rd16(d, mi + 2); String name = readString(d, strIdsOff, rd32(d, mi + 4));
        int pi = protoIdsOff + protoIdx * 12; String returnType = prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, rd32(d, pi + 4)));
        int paramsOff = rd32(d, pi + 8); StringBuilder params = new StringBuilder();
        if (paramsOff != 0 && paramsOff + 4 <= d.length) {
            int count = rd32(d, paramsOff);
            for (int j = 0; j < count; j++) { int off = paramsOff + 4 + j * 2; if (off + 2 > d.length) break; if (j > 0) params.append(";"); params.append(prettyType(getTypeDesc(d, typeIdsOff, strIdsOff, rd16(d, off)))); }
        }
        return name + "|" + returnType + "|" + (name.contains("$") || name.equals("<clinit>") ? "1" : "0") + "|" + ("<init>".equals(name) ? "1" : "0") + "|" + params;
    }

    public static String[] getAvailableInstances(String className) {
        List<String> res = new ArrayList<>();
        res.add("null");
        try {
            Class<?> target = Class.forName(className);
            for (Map.Entry<String, Object> entry : sInstances.entrySet()) {
                if (entry.getValue() != null && target.isAssignableFrom(entry.getValue().getClass())) {
                    res.add(entry.getKey());
                }
            }
        } catch (Throwable ignored) {}
        return res.toArray(new String[0]);
    }

    public static void requestMethodCall(int sectionId, String className, String methodSig, String methodName, String[] paramTypes, String[] args, boolean isStatic, boolean isConstructor, boolean runInMain) {
        Runnable r = () -> {
            String result;
            try {
                Class<?> cls = Class.forName(className);
                Class<?>[] paramClasses = new Class<?>[paramTypes.length];
                for (int i = 0; i < paramTypes.length; i++) paramClasses[i] = classFromString(paramTypes[i]);
                Object[] argValues = new Object[args.length];
                for (int i = 0; i < args.length; i++) argValues[i] = parseArg(paramClasses[i], args[i]);

                Object ret;
                if (isConstructor) {
                    Constructor<?> c = cls.getDeclaredConstructor(paramClasses); c.setAccessible(true); ret = c.newInstance(argValues);
                    result = "Created: " + (ret == null ? "null" : ret.toString()); sInstances.put(className, ret);
                } else if (isStatic) {
                    Method m = cls.getDeclaredMethod(methodName, paramClasses); m.setAccessible(true); ret = m.invoke(null, argValues);
                    result = "Returned: " + (ret == null ? "null" : ret.toString());
                } else {
                    Method m = cls.getDeclaredMethod(methodName, paramClasses); m.setAccessible(true);
                    Object instance = sInstances.get(className);
                    if (instance == null) { instance = tryCreateInstance(cls); if (instance != null) sInstances.put(className, instance); }
                    if (instance == null) result = "Error: Cannot find or create instance of " + className;
                    else { ret = m.invoke(instance, argValues); result = "Returned: " + (ret == null ? "null" : ret.toString()); }
                }
            } catch (Throwable t) { result = "Error: " + t.toString(); }
            nativeSetMethodResult(sectionId, className, methodSig, result);
        };
        if (runInMain) new Handler(Looper.getMainLooper()).post(r); else new Thread(r).start();
    }

    public static void requestFindInstances(String className) {
        new Thread(() -> {
            List<String> found = new ArrayList<>();
            try {
                if (sInstances.containsKey(className)) found.add("Cached: " + sInstances.get(className).toString());
                Class<?> atClass = Class.forName("android.app.ActivityThread");
                Object currentAt = atClass.getMethod("currentActivityThread").invoke(null);
                if (currentAt != null) {
                    java.lang.reflect.Field mActField = atClass.getDeclaredField("mActivities"); mActField.setAccessible(true);
                    android.util.ArrayMap<?, ?> acts = (android.util.ArrayMap<?, ?>) mActField.get(currentAt);
                    if (acts != null) {
                        for (int i = 0; i < acts.size(); i++) {
                            Object record = acts.valueAt(i);
                            java.lang.reflect.Field actField = record.getClass().getDeclaredField("activity"); actField.setAccessible(true);
                            Object act = actField.get(record);
                            if (act != null && act.getClass().getName().equals(className)) found.add("Activity: " + act.toString());
                        }
                    }
                }
                Object app = atClass.getMethod("currentApplication").invoke(null);
                if (app != null && className.equals("android.content.Context")) found.add("Application: " + app.toString());
            } catch (Throwable ignored) {}
            nativeSetInspectorInstances(className, found.toArray(new String[0]));
        }).start();
    }

    private static Object tryCreateInstance(Class<?> cls) {
        try { Constructor<?> c = cls.getDeclaredConstructor(); c.setAccessible(true); return c.newInstance(); } catch (Throwable t) { return null; }
    }
    private static Class<?> classFromString(String s) throws ClassNotFoundException {
        switch (s) {
            case "int": return int.class; case "long": return long.class; case "short": return short.class;
            case "byte": return byte.class; case "char": return char.class; case "boolean": return boolean.class;
            case "float": return float.class; case "double": return double.class; case "void": return void.class;
        }
        if (s.endsWith("[]")) return Array.newInstance(classFromString(s.substring(0, s.length() - 2)), 0).getClass();
        return Class.forName(s);
    }
    private static Object parseArg(Class<?> type, String s) {
        if (s == null || s.equals("null") || s.isEmpty()) return null;
        if (type == int.class || type == Integer.class) return Integer.parseInt(s);
        if (type == long.class || type == Long.class) return Long.parseLong(s);
        if (type == short.class || type == Short.class) return Short.parseShort(s);
        if (type == byte.class || type == Byte.class) return Byte.parseByte(s);
        if (type == char.class || type == Character.class) return s.charAt(0);
        if (type == boolean.class || type == Boolean.class) return s.equals("1") || s.equalsIgnoreCase("true");
        if (type == float.class || type == Float.class) return Float.parseFloat(s);
        if (type == double.class || type == Double.class) return Double.parseDouble(s);
        if (type == String.class) return s;
        if (sInstances.containsKey(s)) return sInstances.get(s);
        return null;
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
    static native void nativeSetMethodResult(int sectionId, String className, String methodSig, String result);
    static native void nativeSetInspectorInstances(String className, String[] instances);
    static native void nativeSetAvailableInstances(String className, String[] instances);
}
