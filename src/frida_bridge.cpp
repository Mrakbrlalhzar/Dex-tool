#include "frida_bridge.h"
#include "logger.h"
#include "frida-gumjs.h"
#include "agent_bundle_embed.h"
#include <mutex>
#include <string>

static GumScriptBackend* g_backend = nullptr;
static GumScript*        g_script  = nullptr;
static GMainLoop*        g_loop    = nullptr;
static GMainContext*     g_ctx     = nullptr;
static std::mutex        g_pmtx;
static MsgCallback       g_cb;

static void on_message(const gchar* msg, GBytes*, gpointer) {
    if (g_cb && msg) g_cb(std::string(msg));
}

void frida_bridge_start(JavaVM*) {
    log_info("[frida] A: starting GumJS");
    g_ctx  = g_main_context_new();
    log_info("[frida] B: main_context_new");
    g_loop = g_main_loop_new(g_ctx, FALSE);
    log_info("[frida] C: main_loop_new");
    g_main_context_push_thread_default(g_ctx);
    log_info("[frida] D: push_thread_default");

    g_backend = gum_script_backend_obtain_qjs();
    log_info("[frida] E: obtain_qjs");
    if (!g_backend) {
        log_crash_raw("[frida] FATAL: no QJS backend\n");
        return;
    }

    GError* err = nullptr;
    g_script = gum_script_backend_create_sync(
                   g_backend, "dextool-agent",
                   g_agent_js, nullptr, nullptr, &err);
    log_info("[frida] F: create_sync returned");
    if (!g_script || err) {
        char m[512];
        snprintf(m, sizeof(m), "[frida] create failed: %s\n",
                 err ? err->message : "(null)");
        log_crash_raw(m);
        if (err) g_error_free(err);
        return;
    }

    gum_script_set_message_handler(g_script, on_message, nullptr, nullptr);
    log_info("[frida] G: msg handler set");

    gum_script_load_sync(g_script, nullptr);
    log_info("[frida] H: load_sync returned");
    log_info("[frida] agent running");
    g_main_loop_run(g_loop);
    log_info("[frida] loop exited");
}

void frida_post(const std::string& json) {
    if (!g_script) return;
    std::lock_guard<std::mutex> lk(g_pmtx);
    gum_script_post(g_script, json.c_str(), nullptr);
}

void frida_set_message_callback(MsgCallback cb) { g_cb = cb; }
