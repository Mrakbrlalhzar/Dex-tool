#include "frida_bridge.h"
#include "overlay.h"
#include "logger.h"
#include "frida-gumjs.h"
#include "agent_bundle_embed.h"
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstring>

static GumScriptBackend* g_backend = nullptr;
static GumScript*        g_script  = nullptr;
static GMainLoop*        g_loop    = nullptr;
static GMainContext*     g_ctx     = nullptr;
static std::mutex        g_pmtx;
static MsgCallback       g_cb;

// ── JSON value helpers ───────────────────────────────────────────
// IMPORTANT: Named parse_float / parse_int to avoid clash with
// jni.h typedefs  "typedef float jfloat"  and  "typedef int32_t jint".
static float parse_float(const char* s, const char* key, float def = 0.f) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = strstr(s, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    char* end;
    float v = strtof(p, &end);
    return (end != p) ? v : def;
}

static int parse_int(const char* s, const char* key, int def = 0) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = strstr(s, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    char* end;
    long v = strtol(p, &end, 10);
    return (end != p) ? (int)v : def;
}

// ── Message handler (GumJS thread) ──────────────────────────────
// Touch and keyboard messages are routed directly to overlay here
// to avoid NativeFunction float param crash (libffi bug on Android).
static void on_message(const gchar* msg, GBytes*, gpointer) {
    if (!msg) return;

    // {type:'touch', x:..., y:..., action:...}
    if (strstr(msg, "\"type\":\"touch\"")) {
        float x = parse_float(msg, "x");
        float y = parse_float(msg, "y");
        int   a = parse_int(msg, "action");
        overlay_on_touch(x, y, a == 0 || a == 2);
        return;
    }

    // {type:'char', cp:...}
    if (strstr(msg, "\"type\":\"char\"")) {
        int cp = parse_int(msg, "cp");
        if (cp > 0) overlay_on_char(cp);
        return;
    }

    // {type:'backspace'}
    if (strstr(msg, "\"type\":\"backspace\"")) {
        overlay_on_backspace();
        return;
    }

    // All other messages → menu callback
    if (g_cb) g_cb(std::string(msg));
}

void frida_bridge_start(JavaVM*) {
    log_info("[frida_bridge] step1: ctx");
    g_ctx  = g_main_context_new();
    g_loop = g_main_loop_new(g_ctx, FALSE);
    g_main_context_push_thread_default(g_ctx);

    log_info("[frida_bridge] step2: backend");
    g_backend = gum_script_backend_obtain_qjs();
    if (!g_backend) {
        log_crash_raw("[frida_bridge] FATAL: no QJS backend\n");
        return;
    }

    log_info("[frida_bridge] step3: create_sync");
    GError* err = nullptr;
    g_script = gum_script_backend_create_sync(
         g_backend, "dextool-agent",
         g_agent_js, nullptr, nullptr, &err);
    if (!g_script || err) {
        char m[512];
        snprintf(m, sizeof(m), "[frida_bridge] create_sync failed: %s\n",
       err ? err->message : "(null)");
        log_crash_raw(m);
        if (err) g_error_free(err);
        return;
    }

    log_info("[frida_bridge] step4: set_message_handler");
    gum_script_set_message_handler(g_script, on_message, nullptr, nullptr);

    log_info("[frida_bridge] step5: load_sync");
    gum_script_load_sync(g_script, nullptr);

    log_info("[frida_bridge] agent running");
    g_main_loop_run(g_loop);
    log_info("[frida_bridge] loop exited");
}

void frida_post(const std::string& json) {
    if (!g_script) return;
    std::lock_guard<std::mutex> lk(g_pmtx);
    gum_script_post(g_script, json.c_str(), nullptr);
}

void frida_set_message_callback(MsgCallback cb) { g_cb = cb; }
