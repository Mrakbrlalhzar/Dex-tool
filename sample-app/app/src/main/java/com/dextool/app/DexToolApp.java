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
    }

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
        String[] names = {"dextool.log","dextool-debug.log","dextool-crash.log"};
        new File(DL).mkdirs();
        for (String n : names) {
            File s = new File(getFilesDir(), "dextool-logs/"+n);
            if (!s.exists()) continue;
            File d = new File(DL, n);
            try (InputStream  is = new FileInputStream(s);
                 OutputStream os = new FileOutputStream(d, true)) {
                byte[] buf = new byte[8192]; int len;
                while ((len=is.read(buf))!=-1) os.write(buf,0,len);
            }
        }
    }

    private void installUncaughtHandler() {
        Thread.UncaughtExceptionHandler prev =
            Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thr, ex) -> {
            try (FileOutputStream f =
                     new FileOutputStream(DL+"/dextool-crash.log",true)) {
                f.write(("JAVA_CRASH thr="+thr.getName()+" "+ex+"\n").getBytes());
            } catch (IOException ignored) {}
            if (prev!=null) prev.uncaughtException(thr, ex);
        });
    }
}
