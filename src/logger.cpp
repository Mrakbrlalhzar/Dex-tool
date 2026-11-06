#include "logger.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>

static FILE* g_info_fp  = nullptr;
static FILE* g_debug_fp = nullptr;
static int   g_crash_fd = -1;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_altstack[65536];

static void ts(char* buf, size_t n) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static size_t put_hex(char* dst, size_t cap, uintptr_t v) {
    char tmp[20]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    size_t p = 0;
    if (cap < 3) return 0;
    dst[p++] = '0'; dst[p++] = 'x';
    while (n > 0 && p < cap - 1) dst[p++] = tmp[--n];
    dst[p] = 0;
    return p;
}

static void crash_handler(int sig, siginfo_t* info, void* ucv) {
    char buf[192]; size_t p = 0;
    auto append = [&](const char* s) {
        while (*s && p < sizeof(buf) - 1) buf[p++] = *s++;
    };
    append("CRASH sig=");
    if (sig >= 10) buf[p++] = '0' + ((sig / 10) % 10);
    buf[p++] = '0' + (sig % 10);
    append(" addr=");
    char hx[24];
    put_hex(hx, sizeof(hx), (uintptr_t)info->si_addr);
    append(hx);
    append(" pc=");
    uintptr_t pc = 0;
#if defined(__aarch64__)
    if (ucv) pc = (uintptr_t)((ucontext_t*)ucv)->uc_mcontext.pc;
#elif defined(__arm__)
    if (ucv) pc = (uintptr_t)((ucontext_t*)ucv)->uc_mcontext.arm_pc;
#endif
    put_hex(hx, sizeof(hx), pc);
    append(hx);
    buf[p++] = '\n';
    if (g_crash_fd >= 0) write(g_crash_fd, buf, p);
    _exit(128 + sig);
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

    stack_t ss;
    ss.ss_sp    = g_altstack;
    ss.ss_size  = sizeof(g_altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
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
