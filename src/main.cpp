#include "logger.h"
#include "overlay.h"
#include "frida-gumjs.h"
#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <cstdio>

JavaVM* g_jvm_global = nullptr;

static void write_marker(const char* path, const char* msg) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s ts=%ld\n", msg, (long)time(nullptr));
    write(fd, buf, (size_t)n);
    close(fd);
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm_global = vm;
    write_marker("/sdcard/Download/dextool-jni.marker", "checkpoint=entry");
    logger_init("/sdcard/Download");
    log_info("[JNI_OnLoad] entry");
    write_marker("/sdcard/Download/dextool-jni.marker", "checkpoint=logger_ready");
    overlay_set_java_vm(vm);
    overlay_start(vm);
    if (!install_swap_hook())
        log_crash_raw("[JNI_OnLoad] FATAL: install_swap_hook failed\n");
    write_marker("/sdcard/Download/dextool-jni.marker", "checkpoint=hooks_installed");
    log_info("[JNI_OnLoad] done — awaiting first eglSwapBuffers");
    write_marker("/sdcard/Download/dextool-jni.marker", "checkpoint=jni_return");
    return JNI_VERSION_1_6;
}
