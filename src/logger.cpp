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

/* Fatal-signal capture. SA_SIGINFO logs signal/si_code/fault
   addr/PC, then the PREVIOUS handler is restored and the signal
   re-raised so debuggerd still writes its tombstone and the
   "Fatal signal" backtrace to logcat (streamed to
   dextool-logcat.txt by DexToolApp). */
static struct sigaction g_prev_act[32];
static bool             g_prev_set[32];
static char             g_alt_stack[64 * 1024];

static void crash_sig_action(int sig, siginfo_t* si, void* uc) {
    uintptr_t pc = 0;
    ucontext_t* u = (ucontext_t*)uc;
#if defined(__aarch64__)
    if (u) pc = (uintptr_t)u->uc_mcontext.pc;
#elif defined(__arm__)
    if (u) pc = (uintptr_t)u->uc_mcontext.arm_pc;
#endif
    char m[160];
    snprintf(m, sizeof(m), "CRASH signal=%d code=%d addr=%p pc=%p\n",
   sig, si ? si->si_code : 0,
             si ? si->si_addr : NULL, (void*)pc);
    log_crash_raw(m);
    {
        Dl_info di;
        if (dladdr((const void*)pc, &di) && di.dli_fname) {
            char lm[256];
            snprintf(lm, sizeof(lm),
                     "pc=%p in %s base=%p off=+%zx\n",
                     (void*)pc, di.dli_fname, di.dli_fbase,
                     (size_t)(pc - (uintptr_t)di.dli_fbase));
            log_crash_raw(lm);
        }
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

void logger_init(const char* base_dir) {
    mkdir(base_dir, 0755);
    snprintf(g_base_dir, sizeof(g_base_dir), "%s", base_dir);
    char p[640];
    snprintf(p, sizeof(p), "%s/dextool.log",       base_dir);
    g_info_fp  = fopen(p, "a");
    snprintf(p, sizeof(p), "%s/dextool-debug.log",  base_dir);
    g_debug_fp = fopen(p, "a");
    /* keep last boot's crash log; start fresh for this boot so
       entries map 1:1 to runs */
    char pprev[640];
    snprintf(pprev, sizeof(pprev), "%s/dextool-crash.prev.log", base_dir);
    snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
    rename(p, pprev);
    g_crash_fd = open(p, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp    = g_alt_stack;
        ss.ss_size  = sizeof(g_alt_stack);
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
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
    /* r14: memory snapshot at boot. lmkd kills (the "crash when I
       open any app" class) are invisible to signal handlers, so the
       field report must show how much headroom the process had. */
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
    log_info("logger_init done (build r14: IME show fix, phased agent init, small-surface ownership gate)");
}

void log_info(const char* msg) {
    if (!g_info_fp) return;
    pthread_mutex_lock(&g_mtx);
    char t[32]; ts(t, sizeof(t));
    fprintf(g_info_fp, "[%s][I] %s\n", t, msg);
    fflush(g_info_fp);
    pthread_mutex_unlock(&g_mtx);
}

void log_debug(const char* msg) {
    if (!g_debug_fp) return;
    pthread_mutex_lock(&g_mtx);
    char t[32]; ts(t, sizeof(t));
    fprintf(g_debug_fp, "[%s][D] %s\n", t, msg);
    fflush(g_debug_fp);
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
        /* custom handler must abort fatal messages itself */
        abort();
    }
}

static void dt_terminate() {
    log_crash_raw("std::terminate called\n");
    try { std::rethrow_exception(std::current_exception()); }
    catch (const std::exception& e) {
        char m[512];
        snprintf(m, sizeof(m), "  uncaught exception: %s\n", e.what());
        log_crash_raw(m);
    } catch (...) {
        log_crash_raw("  uncaught exception: (unknown)\n");
    }
    abort();
}

void logger_install_abort_capture() {
    if (g_crash_fd >= 0) {
        dup2(g_crash_fd, STDOUT_FILENO);
        dup2(g_crash_fd, STDERR_FILENO);
    }
    g_log_set_default_handler(glib_log_capture, nullptr);
    std::set_terminate(dt_terminate);
    log_info("abort capture installed (glib+terminate+stderr -> crash log)");
}
