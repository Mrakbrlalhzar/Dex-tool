#include "logger.h"
#include "overlay.h"
#include "frida-gumjs.h"
#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>
JavaVM* g_jvm_global = nullptr;
static void write_marker(const char* path, const char* msg) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s ts=%ld\n", msg, (long)time(nullptr));
    write(fd, buf, (size_t)n);
    close(fd);
}
static void write_marker_in(const char* dir, const char* name,
                            const char* msg) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    write_marker(p, msg);
}
static void resolve_log_dir(char* out, size_t n) {
    snprintf(out, n, "/sdcard/Download");
    JNIEnv* env = nullptr;
    if (!g_jvm_global) return;
    if (g_jvm_global->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
        return;
    if (!env) return;
    do {
        jclass act = env->FindClass("android/app/ActivityThread");
        if (!act || env->ExceptionCheck()) break;
        jmethodID cur = env->GetStaticMethodID(act,
            "currentApplication", "()Landroid/app/Application;");
        if (!cur || env->ExceptionCheck()) break;
        jobject app = env->CallStaticObjectMethod(act, cur);
        if (!app || env->ExceptionCheck()) break;
        jclass ctx = env->FindClass("android/content/Context");
        if (!ctx || env->ExceptionCheck()) break;
        jmethodID gfd = env->GetMethodID(ctx, "getFilesDir",
                                         "()Ljava/io/File;");
        if (!gfd || env->ExceptionCheck()) break;
        jobject fdir = env->CallObjectMethod(app, gfd);
        if (!fdir || env->ExceptionCheck()) break;
        jclass fio = env->FindClass("java/io/File");
        if (!fio || env->ExceptionCheck()) break;
        jmethodID gap = env->GetMethodID(fio, "getAbsolutePath",
                                         "()Ljava/lang/String;");
        if (!gap || env->ExceptionCheck()) break;
        jstring js = (jstring)env->CallObjectMethod(fdir, gap);
        if (!js || env->ExceptionCheck()) break;
        const char* p = env->GetStringUTFChars(js, nullptr);
        if (p) {
            snprintf(out, n, "%s/dextool-logs", p);
            env->ReleaseStringUTFChars(js, p);
        }
    } while (0);
    if (env->ExceptionCheck()) env->ExceptionClear();
}
extern "C" JNIEXPORT void JNICALL
Java_com_dextool_app_DexToolApp_nativeSetLogDir(JNIEnv* env, jclass,
                                                jstring path) {
    if (!path) return;
    const char* p = env->GetStringUTFChars(path, nullptr);
    if (!p) return;
    logger_retarget(p);
    env->ReleaseStringUTFChars(path, p);
}
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm_global = vm;
    char base[512];
    resolve_log_dir(base, sizeof(base));
    mkdir(base, 0755);       /* entry marker + logs land even on the
                                very first boot */
    write_marker_in(base, "dextool-jni.marker", "checkpoint=entry");
    logger_init(base);        /* mkdir()s <filesDir>/dextool-logs */
    logger_install_abort_capture();
    {
        char m[600];
        snprintf(m, sizeof(m), "[JNI_OnLoad] entry (r16: log dir %s)",
                 base);
        log_info(m);
    }
    write_marker_in(base, "dextool-jni.marker", "checkpoint=logger_ready");
    overlay_set_java_vm(vm);
    overlay_start(vm);
    if (!install_swap_hook())
        log_crash_raw("[JNI_OnLoad] FATAL: install_swap_hook failed\n");
    write_marker_in(base, "dextool-jni.marker", "checkpoint=hooks_installed");
    log_info("[JNI_OnLoad] done — awaiting first eglSwapBuffers");
    write_marker_in(base, "dextool-jni.marker", "checkpoint=jni_return");
    return JNI_VERSION_1_6;
}
