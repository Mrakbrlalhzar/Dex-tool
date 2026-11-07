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
#include <pthread.h>

static FILE* g_info_fp  = nullptr;
static FILE* g_debug_fp = nullptr;
static int   g_crash_fd = -1;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static void ts(char* buf, size_t n) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void crash_sig(int sig) {
    char m[64];
    snprintf(m, sizeof(m), "CRASH signal=%d\n", sig);
    log_crash_raw(m);
    signal(sig, SIG_DFL);
    raise(sig);
}

void logger_init(const char* base_dir) {
    mkdir(base_dir, 0755);
    char p[640];
    snprintf(p, sizeof(p), "%s/dextool.log",      base_dir);
    g_info_fp  = fopen(p, "a");
    snprintf(p, sizeof(p), "%s/dextool-debug.log", base_dir);
    g_debug_fp = fopen(p, "a");
    snprintf(p, sizeof(p), "%s/dextool-crash.log", base_dir);
    g_crash_fd = open(p, O_WRONLY|O_CREAT|O_APPEND, 0644);
    signal(SIGSEGV, crash_sig);
    signal(SIGABRT, crash_sig);
    signal(SIGBUS,  crash_sig);
    log_info("logger_init done");
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
