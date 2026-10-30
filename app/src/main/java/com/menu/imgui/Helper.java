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
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
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
    private static android.view.WindowManager sOverlayWm;
    private static android.view.WindowManager.LayoutParams sOverlayLp;
    private static android.widget.FrameLayout sDecorContainer;
    private static int sFabSize = 200;
    private static int sFabMargin = 40;
    private static android.app.Activity sOverlayActivity;
    private static boolean sMenuOpen = false;







public static void attach(final Context context) {
    final android.app.Activity activity;
    if (context instanceof android.app.Activity) activity = (android.app.Activity) context;
    else if (context instanceof android.content.ContextWrapper) {
        android.content.Context base = ((android.content.ContextWrapper) context).getBaseContext();
        activity = (base instanceof android.app.Activity) ? (android.app.Activity) base : null;
    } else activity = null;
    if (activity == null) { android.util.Log.e("DexTool", "attach: no Activity"); return; }
    sOverlayActivity = activity;
    activity.getWindow().getDecorView().post(new Runnable() {
        @Override public void run() { doAttach(activity); }
    });
}

static void doAttach(android.app.Activity activity) {
    OverlaySurfaceView glView = new OverlaySurfaceView(activity);
    android.view.WindowManager wm = activity.getWindowManager();
    android.util.DisplayMetrics dm = activity.getResources().getDisplayMetrics();
    int fabSize = (int)(80 * dm.density);
    int margin = (int)(20 * dm.density);
    sFabSize = fabSize; sFabMargin = margin;
    android.view.WindowManager.LayoutParams lp = new android.view.WindowManager.LayoutParams(
        fabSize, fabSize,
        android.view.WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
        android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | android.view.WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
        android.graphics.PixelFormat.TRANSPARENT);
    lp.token = activity.getWindow().getDecorView().getWindowToken();
    lp.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
    lp.x = dm.widthPixels - fabSize - margin;
    lp.y = margin * 4;
    try {
        wm.addView(glView, lp);
        sOverlayWm = wm; sOverlayLp = lp; overlay = glView;
        android.util.Log.i("DexTool", "Overlay added via WM (size=" + fabSize + ")");
    } catch (Throwable t) {
        android.util.Log.w("DexTool", "WM failed, decor fallback", t);
        try {
            android.view.ViewGroup decor = (android.view.ViewGroup) activity.getWindow().getDecorView();
            android.widget.FrameLayout container = new android.widget.FrameLayout(activity);
            container.setClipChildren(false);
            android.widget.FrameLayout.LayoutParams clp = new android.widget.FrameLayout.LayoutParams(fabSize, fabSize);
            clp.gravity = android.view.Gravity.TOP | android.view.Gravity.END;
            clp.rightMargin = margin; clp.topMargin = margin * 4;
            container.addView(glView, clp);
            sDecorContainer = container; overlay = glView;
            decor.addView(container, new android.view.ViewGroup.LayoutParams(-1, -1));
            android.util.Log.i("DexTool", "Overlay added to DecorView (small container)");
        } catch (Throwable t2) { android.util.Log.e("DexTool", "ALL FAILED", t2); }
    }
    registerLifecycle(activity, glView);
}

public static void setMenuOpen(final boolean open) {
    sMenuOpen = open;
    if (overlay == null) return;
    final int w = sFabSize, m = sFabMargin;
    overlay.post(new Runnable() {
        @Override public void run() {
            try {
                if (sOverlayWm != null && sOverlayLp != null) {
                    if (open) {
                        sOverlayLp.width = android.view.WindowManager.LayoutParams.MATCH_PARENT;
                        sOverlayLp.height = android.view.WindowManager.LayoutParams.MATCH_PARENT;
                        sOverlayLp.x = 0; sOverlayLp.y = 0;
                    } else {
                        sOverlayLp.width = w; sOverlayLp.height = w;
                        sOverlayLp.x = sOverlayActivity.getResources().getDisplayMetrics().widthPixels - w - m;
                        sOverlayLp.y = m * 4;
                    }
                    sOverlayWm.updateViewLayout(overlay, sOverlayLp);
                } else if (sDecorContainer != null) {
                    android.widget.FrameLayout.LayoutParams clp = (android.widget.FrameLayout.LayoutParams) overlay.getLayoutParams();
                    if (open) { clp.width = -1; clp.height = -1; clp.gravity = 0; clp.rightMargin = 0; clp.topMargin = 0; }
                    else { clp.width = w; clp.height = w; clp.gravity = android.view.Gravity.TOP | android.view.Gravity.END; clp.rightMargin = m; clp.topMargin = m * 4; }
                    overlay.setLayoutParams(clp);
                }
                android.util.Log.i("DexTool", "setMenuOpen(" + open + ")");
            } catch (Throwable t) { android.util.Log.w("DexTool", "setMenuOpen err", t); }
        }
    });
}

private static void registerLifecycle(Activity activity, OverlaySurfaceView glView) {
        activity.getApplication().registerActivityLifecycleCallbacks(
            new android.app.Application.ActivityLifecycleCallbacks() {
                @Override public void onActivityPaused(Activity a) { if (a == activity) glView.onPause(); }
                @Override public void onActivityResumed(Activity a) { if (a == activity) { glView.onResume(); try { if (glView.getParent() == null && sOverlayWm != null && sOverlayLp != null) { sOverlayWm.addView(glView, sOverlayLp); android.util.Log.i("DexTool", "re-attached on resume"); } } catch (Throwable t) {} } }
                @Override public void onActivityCreated(Activity a, android.os.Bundle b) {}
                @Override public void onActivityStarted(Activity a) {}
                @Override public void onActivityStopped(Activity a) {}
                @Override public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
                @Override public void onActivityDestroyed(Activity a) {}
            });
    }


    

    private static final String TAG = "ImGuiTool";
    private static OverlaySurfaceView overlay;
    private static final Map<String, Object> sInstances = new HashMap<>();
    private static Object sCurrentActivity = null;
    private static Object sApplicationContext = null;
    private static int sPatchCounter = 1;

    public static class PatchRecord {
        public int id;
        public String type;
        public String className;
        public String targetName;
        public String originalValue;
        public String currentValue;
        public boolean isActive = true;
    }
    private static final List<PatchRecord> sActivePatches = new ArrayList<>();

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

    








    public static void requestClassDetails(int sectionId, String className) {
        new Thread(() -> {
            List<String> fields = new ArrayList<>();
            List<String> methods = new ArrayList<>();
            try {
                Class<?> clazz = Class.forName(className);
                for (Field f : clazz.getDeclaredFields()) {
                    f.setAccessible(true);
                    fields.add((Modifier.isStatic(f.getModifiers()) ? "[static] " : "[instance] ") + f.getType().getSimpleName() + " " + f.getName());
                }
                for (Method m : clazz.getDeclaredMethods()) {
                    m.setAccessible(true);
                    StringBuilder sb = new StringBuilder();
                    sb.append(Modifier.toString(m.getModifiers())).append(" ");
                    sb.append(m.getReturnType().getSimpleName()).append(" ");
                    sb.append(m.getName()).append("(");
                    Class<?>[] params = m.getParameterTypes();
                    for (int i = 0; i < params.length; i++) { if (i > 0) sb.append(", "); sb.append(params[i].getSimpleName()); }
                    sb.append(")");
                    methods.add(sb.toString());
                }
            } catch (Throwable t) { log("requestClassDetails failed: " + t); }
            nativeSetClassDetails(sectionId, className, fields.toArray(new String[0]), methods.toArray(new String[0]));
        }).start();
    }

    public static void requestGetField(int sectionId, String className, String fieldName, String instanceName) {
        new Thread(() -> {
            String result;
            try {
                Class<?> clazz = Class.forName(className);
                Field f = clazz.getDeclaredField(fieldName);
                f.setAccessible(true);
                Object instance = getInstance(className, instanceName);
                if (instance == null && !Modifier.isStatic(f.getModifiers())) result = "Error: Need instance.";
                else { Object val = f.get(instance); result = String.valueOf(val); }
            } catch (Throwable t) { result = "Error: " + t.toString(); }
            nativeSetFieldValue(sectionId, className, fieldName, result, false);
        }).start();
    }

    public static void requestSetField(int sectionId, String className, String fieldName, String instanceName, String value) {
        new Thread(() -> {
            String result;
            try {
                Class<?> clazz = Class.forName(className);
                Field f = clazz.getDeclaredField(fieldName);
                f.setAccessible(true);
                Object instance = getInstance(className, instanceName);
                if (instance == null && !Modifier.isStatic(f.getModifiers())) result = "Error: Need instance.";
                else { Object parsed = parseArg(f.getType(), value); f.set(instance, parsed); result = "Set: " + f.get(instance); }
            } catch (Throwable t) { result = "Error: " + t.toString(); }
            nativeSetFieldValue(sectionId, className, fieldName, result, true);
        }).start();
    }

    private static Object getInstance(String className, String instanceName) {
        if (instanceName == null || instanceName.isEmpty() || instanceName.equals("null")) return null;
        if (instanceName.equals("[Live] Current Activity")) return sCurrentActivity;
        if (instanceName.equals("[Live] Application Context")) return sApplicationContext;
        if (instanceName.startsWith("[Cached] ")) return sInstances.get(instanceName.substring(9));
        return sInstances.get(instanceName);
    }

    public static void requestPatchMethod(String className, String target, String value) { for (PatchRecord __p : sActivePatches) { if ("Method".equals(__p.type) && className.equals(__p.className) && target.equals(__p.targetName)) { __p.currentValue = value; syncPatches(); nativePatchMethod(className, target, value); return; } } PatchRecord p = new PatchRecord(); p.id = sPatchCounter++; p.type = "Method"; p.className = className; p.targetName = target; p.originalValue = "(original)"; p.currentValue = value; sActivePatches.add(p); syncPatches(); nativePatchMethod(className, target, value); }


    public static void requestPatchField(String className, String target, String value) {
    for (PatchRecord __p : sActivePatches) {
        if (__p.type.equals("Field") && __p.className.equals(className) && __p.targetName.equals(target)) {
            __p.currentValue = value;
            syncPatches();
            return;
        }
    }
    PatchRecord p = new PatchRecord();
    p.id = sPatchCounter++;
    p.type = "Field";
    p.className = className;
    p.targetName = target;
    p.originalValue = "(original)";
    p.currentValue = value;
    sActivePatches.add(p);
    syncPatches();
}
    public static void togglePatch(int id, boolean active) {
        for (PatchRecord p : sActivePatches) if (p.id == id) { p.isActive = active; break; }
        syncPatches();
    }
    public static void updatePatchValue(int id, String newValue) {
        for (PatchRecord p : sActivePatches) if (p.id == id) { p.currentValue = newValue; break; }
        syncPatches();
    }
    public static void removePatch(int id) { sActivePatches.removeIf(p -> p.id == id); syncPatches(); }
    private static void syncPatches() {        // TEMP:DEDUP
        try {
            java.util.Map<String, PatchRecord> __map = new java.util.LinkedHashMap<>();
            java.lang.reflect.Field __fC = null, __fM = null;
            for (PatchRecord __r : sActivePatches) {
                if (__fC == null) {
                    try { __fC = __r.getClass().getDeclaredField("className"); __fC.setAccessible(true); } catch (Throwable __e) {}
                    try { __fM = __r.getClass().getDeclaredField("methodName"); __fM.setAccessible(true); } catch (Throwable __e) {}
                }
                String __k;
                if (__fC != null && __fM != null) {
                    __k = String.valueOf(__fC.get(__r)) + "#" + String.valueOf(__fM.get(__r));
                } else {
                    __k = String.valueOf(System.identityHashCode(__r));
                }
                __map.put(__k, __r);
            }
            sActivePatches.clear();
            sActivePatches.addAll(__map.values());
        } catch (Throwable __t) { }

        List<String> serialized = new ArrayList<>();
        for (PatchRecord p : sActivePatches) serialized.add(p.id + "|" + p.type + "|" + p.className + "|" + p.targetName + "|" + p.originalValue + "|" + p.currentValue + "|" + p.isActive);
        nativeSetPatches(serialized.toArray(new String[0]));
    }

    public static int getArgWidgetType(String typeName) {
    try {
        Class<?> c = classFromString(typeName);
        if (c == boolean.class || c == Boolean.class) return 1;
        if (c.isEnum()) return 2;
        if (Context.class.isAssignableFrom(c) || Activity.class.isAssignableFrom(c)) return 3;
        if (c.isArray()) return 5;
        if (c == int.class || c == Integer.class || c == long.class || c == Long.class
                || c == short.class || c == Short.class || c == byte.class || c == Byte.class
                || c == java.math.BigInteger.class) return 6;
        if (c == float.class || c == Float.class || c == double.class || c == Double.class
                || c == java.math.BigDecimal.class) return 7;
        if (c == String.class || c == CharSequence.class || c == char.class || c == Character.class) return 0;
        return 4;
    } catch (Throwable t) { return 0; }
}

    public static String[] getArgSuggestions(String typeName) {
        try {
            Class<?> c = classFromString(typeName);
            List<String> r = new ArrayList<>();
            if (c.isEnum()) { for (Object e : c.getEnumConstants()) r.add(((Enum<?>)e).name()); return r.toArray(new String[0]); }
            if (c.isArray() || c.isPrimitive() || Number.class.isAssignableFrom(c) || c == String.class || c == CharSequence.class) return new String[0];
            r.add("null");
            for (Map.Entry<String, Object> e : sInstances.entrySet()) if (e.getValue() != null && c.isInstance(e.getValue())) r.add("[Cached] " + e.getKey());
            if (sCurrentActivity != null && c.isInstance(sCurrentActivity)) r.add("[Live] Current Activity");
            if (sApplicationContext != null && c.isInstance(sApplicationContext)) r.add("[Live] Application Context");
            return r.toArray(new String[0]);
        } catch (Throwable t) { return new String[]{"null"}; }
    }

    private static Object resolveArg(Class<?> target, String input) throws Exception {
        if (input == null || input.equals("null") || input.isEmpty()) {
            if (target.isPrimitive()) throw new IllegalArgumentException("Cannot pass null to primitive");
            return null;
        }
        if (input.equals("[Live] Current Activity")) return sCurrentActivity;
        if (input.equals("[Live] Application Context")) return sApplicationContext;
        if (input.startsWith("[Cached] ")) return sInstances.get(input.substring(9));
        if (sInstances.containsKey(input)) return sInstances.get(input);
        if (target == String.class || target == CharSequence.class || target == Object.class) return input;
        if (target.isPrimitive() || Number.class.isAssignableFrom(target) || target == Boolean.class || target == Character.class) return coercePrimitive(target, input);
        if (target.isEnum()) { for (Object c : target.getEnumConstants()) if (((Enum<?>)c).name().equalsIgnoreCase(input)) return c; throw new IllegalArgumentException("No enum constant"); }
        if (target.isArray()) {
            Class<?> comp = target.getComponentType();
            if (comp == byte.class) return input.getBytes(StandardCharsets.UTF_8);
            if (comp == char.class) return input.toCharArray();
            String[] parts = input.split(",");
            Object arr = Array.newInstance(comp, parts.length);
            for (int i = 0; i < parts.length; i++) Array.set(arr, i, resolveArg(comp, parts[i].trim()));
            return arr;
        }
        throw new IllegalArgumentException("Cannot convert to " + target.getName());
    }

    private static Object coercePrimitive(Class<?> target, String input) {
        String s = input.trim();
        if (target == int.class || target == Integer.class) return Integer.parseInt(s);
        if (target == long.class || target == Long.class) return Long.parseLong(s);
        if (target == short.class || target == Short.class) return Short.parseShort(s);
        if (target == byte.class || target == Byte.class) return Byte.parseByte(s);
        if (target == float.class || target == Float.class) return Float.parseFloat(s);
        if (target == double.class || target == Double.class) return Double.parseDouble(s);
        if (target == boolean.class || target == Boolean.class) return s.equalsIgnoreCase("true") || s.equals("1");
        if (target == char.class || target == Character.class) return s.charAt(0);
        if (target == java.math.BigInteger.class) return new java.math.BigInteger(s);
        if (target == java.math.BigDecimal.class) return new java.math.BigDecimal(s);
        throw new IllegalArgumentException("Unsupported primitive");
    }

    public static void requestMethodCall(int sectionId, String className, String methodSig, String methodName, String[] paramTypes, String[] args, boolean isStatic, boolean isConstructor, boolean runInMain) {
        Runnable r = () -> {
            String result;
            try {
                Class<?> cls = Class.forName(className);
                Class<?>[] paramClasses = new Class<?>[paramTypes.length];
                for (int i = 0; i < paramTypes.length; i++) paramClasses[i] = classFromString(paramTypes[i]);
                Object[] argValues = new Object[args.length];
                for (int i = 0; i < args.length; i++) argValues[i] = resolveArg(paramClasses[i], args[i]);
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
                    if (instance == null) result = "Error: Cannot find or create instance.";
                    else { ret = m.invoke(instance, argValues); result = "Returned: " + (ret == null ? "null" : ret.toString()); }
                }
            } catch (Throwable t) { result = "Error: " + t.toString(); }
            nativeSetMethodResult(sectionId, className, methodSig, result);
        };
        if (runInMain) new Handler(Looper.getMainLooper()).post(r); else new Thread(r).start();
    }

    private static Object tryCreateInstance(Class<?> cls) {
        try { Constructor<?> c = cls.getDeclaredConstructor(); c.setAccessible(true); return c.newInstance(); }
        catch (Throwable t) { return null; }
    }
    private static Class<?> classFromString(String s) throws ClassNotFoundException {
        switch (s) {
            case "int": return int.class; case "long": return long.class; case "short": return short.class;
            case "byte": return byte.class; case "char": return char.class; case "boolean": return boolean.class;
            case "float": return float.class; case "double": return double.class; case "void": return void.class;
        }
        if (s.endsWith("[]")) return Array.newInstance(classFromString(s.substring(0, s.length() - 2)), 0).getClass();
        if (s.equals("String")) return String.class;
        return Class.forName(s);
    }

    private static Object parseArg(Class<?> type, String s) {
        if (s == null || s.equals("null") || s.isEmpty()) return null;
        if (type == int.class || type == Integer.class) return Integer.parseInt(s);
        if (type == long.class || type == Long.class) return Long.parseLong(s);
        if (type == short.class || type == Short.class) return Short.parseShort(s);
        if (type == byte.class || type == Byte.class) return Byte.parseByte(s);
        if (type == float.class || type == Float.class) return Float.parseFloat(s);
        if (type == double.class || type == Double.class) return Double.parseDouble(s);
        if (type == boolean.class || type == Boolean.class) return s.equalsIgnoreCase("true") || s.equals("1");
        if (type == char.class || type == Character.class) return s.charAt(0);
        if (type == String.class) return s;
        if (type.isEnum()) { for (Object c : type.getEnumConstants()) if (((Enum<?>)c).name().equalsIgnoreCase(s)) return c; return null; }
        return null;
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
    private static int rd32(byte[] d, int off) { return (d[off]&0xFF) | ((d[off+1]&0xFF)<<8) | ((d[off+2]&0xFF)<<16) | ((d[off+3]&0xFF)<<24); }
    private static String readString(byte[] d, int strIdsOff, int strIdx) {
        if (strIdx < 0) return "";
        int strDataOff = rd32(d, strIdsOff + strIdx * 4);
        if (strDataOff <= 0 || strDataOff >= d.length) return "";
        int p = strDataOff; while (p < d.length && (d[p] & 0x80) != 0) p++; if (p < d.length) p++;
        int start = p; while (p < d.length && d[p] != 0) p++;
        return new String(d, start, p - start, StandardCharsets.UTF_8);
    }
    private static String getTypeDesc(byte[] d, int typeIdsOff, int strIdsOff, int typeIdx) { return readString(d, strIdsOff, rd32(d, typeIdsOff + typeIdx * 4)); }
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
    static native void nativeSetFieldValue(int sectionId, String className, String fieldName, String value, boolean isSet);
    static native void nativePatchMethod(String className, String methodName, String returnValue); static native void nativeSetPatches(String[] patches);
}
