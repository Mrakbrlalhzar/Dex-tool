#include "logger.h"
#include "frida-gumjs.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <pthread.h>
static FILE* g_info_fp  = nullptr;
static FILE* g_debug_fp = nullptr;
static int   g_crash_fd = -1;
static char  g_base_dir[512] = "";      /* r11: for menu.cpp config IO */
const char* logger_base_dir() { return g_base_dir; }
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static void ts(char* buf, size_t n) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static struct sigaction g_prev_act[32];
static bool             g_prev_set[32];
static char             g_alt_stack[64 * 1024];
static int crash_ux(char* dst, unsigned long v) {
    /* r30 M2: async-signal-safe hex formatter (no stdio) */
    int n = 0;
    dst[n++] = '0';
    dst[n++] = 'x';
    char tmp[18];
    int t = 0;
    do {
        int d = (int)(v & 15ul);
        tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v && t < 16);
    while (t) dst[n++] = tmp[--t];
    return n;
}
static void crash_sig_action(int sig, siginfo_t* si, void* uc) {
    uintptr_t pc = 0;
    ucontext_t* u = (ucontext_t*)uc;
#if defined(__aarch64__)
    if (u) pc = (uintptr_t)u->uc_mcontext.pc;
#elif defined(__arm__)
    if (u) pc = (uintptr_t)u->uc_mcontext.arm_pc;
#endif
    {
        /* r30 M2: async-signal-safe only - no snprintf (stdio lock)
           and no dladdr (loader lock): they can deadlock exactly on
           the crash they are supposed to report. write() only. */
        char m[160];
        size_t n = 0;
        const char* q = "CRASH signal=";
        while (*q && n < sizeof(m) - 1) m[n++] = *q++;
        n += (size_t)crash_ux(m + n, (unsigned long)sig);
        q = " code=";
        while (*q && n < sizeof(m) - 1) m[n++] = *q++;
        n += (size_t)crash_ux(m + n, (unsigned long)(si ? si->si_code : 0));
        q = " addr=";
        while (*q && n < sizeof(m) - 1) m[n++] = *q++;
        n += (size_t)crash_ux(m + n,
                              (unsigned long)(si ? (uintptr_t)si->si_addr : 0));
        q = " pc=";
        while (*q && n < sizeof(m) - 1) m[n++] = *q++;
        n += (size_t)crash_ux(m + n, (unsigned long)pc);
        if (n < sizeof(m) - 1) m[n++] = '\n';
        m[n] = 0;
        log_crash_raw(m);
    }
    if (g_prev_set[sig]) {
        sigaction(sig, &g_prev_act[sig], NULL);
    } else {
        signal(sig, SIG_DFL);
    }
    raise(sig);
}
static void install_crash_sig(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sig_action;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    g_prev_set[sig] = (sigaction(sig, &sa, &g_prev_act[sig]) == 0);
}
static void mkdir_p(const char* path) {
    /* r30 M5: create the whole base-dir chain - a missing directory
       used to silence all logging (fopen just failed) */
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    for (char* q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = 0;
            mkdir(tmp, 0755);
            *q = '/';
        }
    }
    mkdir(tmp, 0755);
}
void logger_init(const char* base_dir) {
    mkdir_p(base_dir);
    snprintf(g_base_dir, sizeof(g_base_dir), "%s", base_dir);
    char p[640];
    snprintf(p, sizeof(p), "%s/dextool.log",       base_dir);
    g_info_fp  = fopen(p, "a");
    snprintf(p, sizeof(p), "%s/dextool-debug.log",  base_dir);
    g_debug_fp = fopen(p, "a");
    char pprev[640];
    snprintf(pprev, sizeof(pprev), "%s/dextool-crash.prev.log", base_dir);
    snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
    rename(p, pprev);
    g_crash_fd = open(p, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND, 0644);
    {
        stack_t old;
        memset(&old, 0, sizeof(old));
        if (sigaltstack(NULL, &old) != 0 || (old.ss_flags & SS_DISABLE) ||
            old.ss_size < 16 * 1024) {
            stack_t ss;
            memset(&ss, 0, sizeof(ss));
            ss.ss_sp    = g_alt_stack;
            ss.ss_size  = sizeof(g_alt_stack);
            ss.ss_flags = 0;
            sigaltstack(&ss, NULL);
        }
    }
    install_crash_sig(SIGSEGV);
    install_crash_sig(SIGABRT);
    install_crash_sig(SIGBUS);
    install_crash_sig(SIGILL);
    install_crash_sig(SIGFPE);
    {
        char boot[96];
        snprintf(boot, sizeof(boot), "--- boot %ld ---\n", (long)time(nullptr));
        log_crash_raw(boot);
    }
    {
        char ln[256];
        long avail = -1, rss = -1;
        FILE* mf = fopen("/proc/meminfo", "r");
        if (mf) {
            while (fgets(ln, sizeof(ln), mf))
                if (sscanf(ln, "MemAvailable: %ld kB", &avail) == 1) break;
            fclose(mf);
        }
        FILE* sf = fopen("/proc/self/status", "r");
        if (sf) {
            while (fgets(ln, sizeof(ln), sf))
                if (sscanf(ln, "VmRSS: %ld kB", &rss) == 1) break;
            fclose(sf);
        }
        char mm[160];
        snprintf(mm, sizeof(mm),
                 "mem: VmRSS=%ldkB MemAvailable=%ldkB", rss, avail);
        log_info(mm);
    }
    log_info("logger_init done (build r31: M4 real JSON tree parser with shallowest-match key lookup, surrogate pairs and a depth cap, M7 releaseHandle frees retained instances on object tab close, M8 lock-free frame pump via atomic mirror, CI warns on hand edits to generated files. r30: flash-loop lock, deque containers, hook-mode fix, dumpObject args, touch/GL/logger fixes, signal-safe crash handler)");
}
void logger_retarget(const char* base_dir) {
    if (!base_dir || !base_dir[0]) return;
    if (strcmp(base_dir, g_base_dir) == 0) return;
    pthread_mutex_lock(&g_mtx);
    if (g_info_fp)  { fclose(g_info_fp);  g_info_fp  = nullptr; }
    if (g_debug_fp) { fclose(g_debug_fp); g_debug_fp = nullptr; }
    mkdir_p(base_dir);   /* r30 M5: retarget may hit a fresh dir */
    snprintf(g_base_dir, sizeof(g_base_dir), "%s", base_dir);
    char p[640];
    snprintf(p, sizeof(p), "%s/dextool.log", base_dir);
    g_info_fp = fopen(p, "a");
    snprintf(p, sizeof(p), "%s/dextool-debug.log", base_dir);
    g_debug_fp = fopen(p, "a");
    {
        char pprev[640];
        snprintf(pprev, sizeof(pprev), "%s/dextool-crash.prev.log", base_dir);
        snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
        rename(p, pprev);
    }
    if (g_crash_fd >= 0) {
        snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
        int nfd = open(p, O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (nfd >= 0) {
            dup2(nfd, g_crash_fd);
            close(nfd);
        }
    } else {
        snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
        g_crash_fd = open(p, O_WRONLY|O_CREAT|O_APPEND, 0644);
    }
    pthread_mutex_unlock(&g_mtx);
    {
        char m[128];
        snprintf(m, sizeof(m), "[logger] retargeted to %s", base_dir);
        log_info(m);
    }
}
void log_info(const char* msg) {
    pthread_mutex_lock(&g_mtx);
    if (g_info_fp) {
        char t[32]; ts(t, sizeof(t));
        fprintf(g_info_fp, "[%s][I] %s\n", t, msg);
        fflush(g_info_fp);
    }
    pthread_mutex_unlock(&g_mtx);
}
void log_debug(const char* msg) {
    pthread_mutex_lock(&g_mtx);
    if (g_debug_fp) {
        char t[32]; ts(t, sizeof(t));
        fprintf(g_debug_fp, "[%s][D] %s\n", t, msg);
        fflush(g_debug_fp);
    }
    pthread_mutex_unlock(&g_mtx);
}
void log_crash_raw(const char* msg) {
    if (g_crash_fd >= 0) write(g_crash_fd, msg, strlen(msg));
}
static void glib_log_capture(const gchar* domain, GLogLevelFlags level,
                             const gchar* message, gpointer) {
    const char* lv =
        (level & G_LOG_LEVEL_ERROR)    ? "ERROR"    :
        (level & G_LOG_LEVEL_CRITICAL) ? "CRITICAL" :
        (level & G_LOG_LEVEL_WARNING)  ? "WARNING"  : "MSG";
    char m[1024];
    snprintf(m, sizeof(m), "[GLIB] %s-%s: %s\n",
             domain ? domain : "GLib", lv, message ? message : "(null)");
    log_crash_raw(m);
    if (level & G_LOG_FLAG_FATAL) {
        abort();
    }
}
static void dt_terminate() {
    log_crash_raw("std::terminate called\n");
    std::exception_ptr ep = std::current_exception();
    if (ep) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) {
            char m[512];
            snprintf(m, sizeof(m), "  uncaught exception: %s\n", e.what());
            log_crash_raw(m);
        } catch (...) {
            log_crash_raw("  uncaught exception: (unknown)\n");
        }
    } else {
        log_crash_raw("  (no active exception)\n");
    }
    abort();
}
void logger_install_abort_capture() {
    /* r30 M1: no longer hijack the host app's stdout/stderr - the
       crash log keeps its own fd and the app keeps its console */
    g_log_set_default_handler(glib_log_capture, nullptr);
    std::set_terminate(dt_terminate);
    log_info("abort capture installed (glib+terminate+stderr -> crash log)");
}
