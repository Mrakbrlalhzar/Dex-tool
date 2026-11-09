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
        try { Thread.sleep(500); } catch (InterruptedException ignored) {}
        installUncaughtHandler();
        try {
            System.loadLibrary("tool");
            Log.i(TAG, "libtool.so loaded");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "loadLibrary failed: " + e.getMessage());
        }
        /* r16: hand THIS app's private log dir to the native logger.
           JNI_OnLoad runs inside System.loadLibrary, i.e. while
           Application.onCreate is still running - on many Android
           versions ActivityThread.currentApplication() is null
           there, so the native side fell back to /sdcard/Download
           (unwritable under scoped storage on modern devices) while
           this copier read getFilesDir()/dextool-logs. The Java
           side knows its own dir with certainty. */
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

    /* implemented in main.cpp (JNI_OnLoad's library): calls
       logger_retarget() - files are reopened append-only in the
       new dir, so anything already logged is kept */
    private static native void nativeSetLogDir(String path);

    /* stream this process' logcat (own uid, allowed) so native
       abort messages that only reach logcat are captured too */
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

    private void copyLogs() throws IOException {
        String[] names = {"dextool.log", "dextool-debug.log", "dextool-crash.log"};
        File srcDir = new File(getFilesDir(), "dextool-logs");
        new File(DL).mkdirs();
        for (String n : names) {
            File s = new File(srcDir, n);
            if (!s.exists()) continue;
            File d = new File(DL, n);
            /* unchanged since the last pass -> skip the rewrite */
            if (d.exists() && d.length() == s.length() &&
                    s.lastModified() <= d.lastModified()) continue;
            /* r16: ATOMIC copy. The r15 version appended the WHOLE
               source file every 5s (FileOutputStream(d, true)), so
               every cycle duplicated the entire log again - a pulled
               log held one copy per 5s of uptime. Write a temp file
               and rename(2) it over the target: atomic, no
               duplication, no torn reads. */
            File t = new File(DL, n + ".tmp");
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
            /* r16: append to the SOURCE log (filesDir/dextool-logs),
               not to the /sdcard/Download copy - the copier now
               overwrites that copy atomically, so an entry written
               straight there was clobbered within one 5s cycle. A
               single append to the source file cannot duplicate
               anything (the native crash fd writes with O_APPEND
               too); an immediate copy pass lands the entry in
               Download even if the process dies before the next
               scheduled pass. */
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
