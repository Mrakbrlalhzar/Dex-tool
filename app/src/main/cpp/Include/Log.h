#pragma once
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <unwind.h>
#include <sys/stat.h>
#include <android/log.h>

static char g_log_path[512] = {0};

static inline void flog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "DexTool", "%s", buf);
    if (g_log_path[0]) {
        int fd = open(g_log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }
}

static inline const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default:      return "SIG?";
    }
}

struct BtState { void** cur; void** end; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg) {
    BtState* s = (BtState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (s->cur == s->end) return _URC_END_OF_STACK;
        *s->cur++ = (void*)pc;
    }
    return _URC_NO_REASON;
}

static void crash_sig(int sig, siginfo_t* si, void*) {
    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "\n=== CRASH sig=%d (%s) addr=%p ===\n",
        sig, sig_name(sig), si ? si->si_addr : nullptr);
    int fd = -1;
    if (g_log_path[0]) {
        fd = open(g_log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    }
    if (fd < 0) {
        fd = open("/sdcard/Android/data/com.example.tool/files/DexTool.log",
                  O_WRONLY | O_APPEND | O_CREAT, 0666);
    }
    if (fd < 0) {
        fd = open("/data/local/tmp/DexTool.log",
                  O_WRONLY | O_APPEND | O_CREAT, 0666);
    }
    if (fd >= 0) {
        write(fd, buf, len);
        void* frames[64];
        BtState st = { frames, frames + 64 };
        _Unwind_Backtrace(bt_cb, &st);
        size_t n = (size_t)(st.cur - frames);
        for (size_t i = 0; i < n; i++) {
            Dl_info info;
            if (dladdr(frames[i], &info) && info.dli_sname) {
                len = snprintf(buf, sizeof(buf), "  #%zu %p %s+%td (%s)\n",
                    i, frames[i], info.dli_sname,
                    (char*)frames[i] - (char*)info.dli_saddr,
                    info.dli_fname ? info.dli_fname : "?");
            } else {
                len = snprintf(buf, sizeof(buf), "  #%zu %p\n", i, frames[i]);
            }
            write(fd, buf, len);
        }
        write(fd, "=== END CRASH ===\n", 18);
        close(fd);
    }
    __android_log_print(ANDROID_LOG_FATAL, "DexTool",
        "CRASH sig=%d addr=%p", sig, si ? si->si_addr : nullptr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static inline void install_crash_handler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sig;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}
