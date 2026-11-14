package com.dextool.app;
import android.app.Application;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
public class DexToolApp extends Application {
    private static final String TAG = "DexToolApp";
    private static final String DL  = "/sdcard/Download";
    @Override
    public void onCreate() {
        super.onCreate();
        startLogcatThread();
        writeBootMarker();
        startCopierThread();
        installUncaughtHandler();
        try {
            System.loadLibrary("tool");
            Log.i(TAG, "libtool.so loaded");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "loadLibrary failed: " + e.getMessage());
        }
        retargetNativeLogs();
    }
    private void retargetNativeLogs() {
        try {
            File d = new File(getFilesDir(), "dextool-logs");
            d.mkdirs();
            nativeSetLogDir(d.getAbsolutePath());
            Log.i(TAG, "native log dir: " + d.getAbsolutePath());
        } catch (Throwable t) {
            Log.w(TAG, "retarget logs: " + t.getMessage());
        }
    }
    private static native void nativeSetLogDir(String path);
    private void startLogcatThread() {
        Thread t = new Thread(() -> {
            try {
                new File(DL, "dextool-logcat.txt").delete();
                Process p = Runtime.getRuntime().exec(new String[]{
                    "logcat", "-b", "main", "-b", "crash",
                    "-b", "system",
                    "--pid=" + android.os.Process.myPid(),
                    "-v", "brief"});
                java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.InputStreamReader(p.getInputStream()));
                FileOutputStream f =
                    new FileOutputStream(DL + "/dextool-logcat.txt", true);
                String line; int n = 0;
                while ((line = r.readLine()) != null) {
                    if (n++ < 20000) {
                        f.write((line + "\n").getBytes());
                        f.flush();
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "logcat: " + e.getMessage());
            }
        }, "dextool-logcat");
        t.setDaemon(true);
        t.start();
    }
    private void writeBootMarker() {
        try {
            new File(DL).mkdirs();
            try (FileOutputStream f = new FileOutputStream(DL+"/dextool-boot.marker")) {
                f.write(("boot ts="+System.currentTimeMillis()+"\n").getBytes());
            }
        } catch (IOException e) { Log.w(TAG, "boot marker: "+e.getMessage()); }
    }
    private void startCopierThread() {
        Thread t = new Thread(() -> {
            while (true) {
                try {
                    Thread.sleep(5000);
                    copyLogs();
                } catch (InterruptedException e) { break; }
                  catch (Exception e) { Log.w(TAG, "copier: "+e.getMessage()); }
            }
        }, "dextool-copier");
        t.setDaemon(true);
        t.start();
    }
    private File[] exportDirs() {
        File ext = getExternalFilesDir(null);
        return ext != null ? new File[]{ new File(DL), ext }
                           : new File[]{ new File(DL) };
    }
    private synchronized void copyLogs() throws IOException {
        String[] names = {"dextool.log", "dextool-debug.log",
                          "dextool-crash.log", "dextool-crash.prev.log"};
        File srcDir = new File(getFilesDir(), "dextool-logs");
        IOException first = null;
        for (File destDir : exportDirs()) {
            try {
                copyInto(srcDir, destDir, names);
            } catch (IOException e) {      /* one failing dir must not block the other */
                if (first == null) first = e;
            }
        }
        if (first != null) Log.w(TAG, "copyLogs: " + first.getMessage());
    }
    private void copyInto(File srcDir, File destDir, String[] names) throws IOException {
        destDir.mkdirs();
        for (String n : names) {
            File s = new File(srcDir, n);
            if (!s.exists()) continue;
            File d = new File(destDir, n);
            if (d.exists() && d.length() == s.length() &&
                    s.lastModified() <= d.lastModified()) continue;
            File t = new File(destDir, n + ".tmp");
            try (InputStream  is = new FileInputStream(s);
                 OutputStream os = new FileOutputStream(t, false)) {
                byte[] buf = new byte[8192]; int len;
                while ((len = is.read(buf)) != -1) os.write(buf, 0, len);
                os.flush();
            }
            if (!t.renameTo(d)) t.delete();
        }
    }
    private void installUncaughtHandler() {
        Thread.UncaughtExceptionHandler prev =
            Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thr, ex) -> {
            try {
                File srcDir = new File(getFilesDir(), "dextool-logs");
                srcDir.mkdirs();
                try (FileOutputStream f =
                         new FileOutputStream(
                             new File(srcDir, "dextool-crash.log"), true)) {
                    f.write(("JAVA_CRASH thr=" + thr.getName()
                             + " " + ex + "\n").getBytes());
                }
                copyLogs();
            } catch (Throwable ignored) {}
            if (prev != null) prev.uncaughtException(thr, ex);
        });
    }
}
