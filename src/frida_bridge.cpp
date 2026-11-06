#include "frida_bridge.h"
#include "logger.h"
#include "frida-gumjs.h"
#include "agent_bundle_embed.h"
#include <mutex>
#include <string>
#include <cstdio>
#include <cstring>

static GumScriptBackend* g_backend = nullptr;
static GumScript*        g_script  = nullptr;
static GMainLoop*        g_loop    = nullptr;
static GMainContext*     g_ctx     = nullptr;
static std::mutex        g_pmtx;
static MsgCallback       g_cb;

static void on_message(const gchar* msg, GBytes*, gpointer) {
    if (msg) {
        const char* t = strstr(msg, "\"type\":\"");
        if (t) {
            char tag[72];
            snprintf(tag, sizeof(tag), "[msg] %.48s", t + 8);
            char* q = strchr(tag, '"');
            if (q) *q = '\0';
            log_debug(tag);
        }
    }
    MsgCallback local;
    {
        std::lock_guard<std::mutex> lk(g_pmtx);
        local = g_cb;
    }
    if (local && msg) local(std::string(msg));
}

void frida_bridge_start(JavaVM*) {
    log_info("[frida_bridge] starting GumJS");
    g_ctx  = g_main_context_new();
    g_loop = g_main_loop_new(g_ctx, FALSE);
    g_main_context_push_thread_default(g_ctx);
    g_backend = gum_script_backend_obtain_qjs();
    if (!g_backend) {
        log_crash_raw("[frida_bridge] FATAL: no QJS backend\n");
        return;
    }
    log_info("[frida_bridge] QJS backend obtained");
    GError* err = nullptr;
    g_script = gum_script_backend_create_sync(
                   g_backend, "dextool-agent",
                   g_agent_js, nullptr, nullptr, &err);
    if (!g_script || err) {
        char m[512];
        snprintf(m, sizeof(m), "[frida_bridge] create: %s\n",
                 err ? err->message : "(null)");
        log_crash_raw(m);
        if (err) g_error_free(err);
        return;
    }
    log_info("[frida_bridge] script created, loading agent");
    gum_script_set_message_handler(g_script, on_message, nullptr, nullptr);
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

void frida_set_message_callback(MsgCallback cb) {
    std::lock_guard<std::mutex> lk(g_pmtx);
    g_cb = cb;
}
