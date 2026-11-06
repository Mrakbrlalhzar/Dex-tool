#include "frida_bridge.h"
#include "logger.h"
#include "frida-gumjs.h"
#include "agent_bundle_embed.h"
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

static GumScriptBackend*          g_backend = nullptr;
static std::atomic<GumScript*>    g_script {nullptr};
static GMainLoop*                 g_loop    = nullptr;
static GMainContext*              g_ctx     = nullptr;
static std::mutex                 g_post_mtx;
static std::mutex                 g_msg_mtx;
static std::vector<std::string>   g_msg_queue;

static void on_message(const gchar* msg, GBytes*, gpointer) {
    if (!msg) return;
    std::lock_guard<std::mutex> lk(g_msg_mtx);
    if (g_msg_queue.size() > 1024) {
        g_msg_queue.erase(g_msg_queue.begin(),
                          g_msg_queue.begin() + 512);
    }
    g_msg_queue.push_back(std::string(msg));
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
    GumScript* script = gum_script_backend_create_sync(
                   g_backend, "dextool-agent",
                   g_agent_js, nullptr, nullptr, &err);
    log_info("[frida] F: create_sync returned");
    if (!script || err) {
        char m[512];
        snprintf(m, sizeof(m), "[frida] create failed: %s\n",
                 err ? err->message : "(null)");
        log_crash_raw(m);
        if (err) g_error_free(err);
        return;
    }
    g_script.store(script);

    gum_script_set_message_handler(script, on_message, nullptr, nullptr);
    log_info("[frida] G: msg handler set");

    gum_script_load_sync(script, nullptr);
    log_info("[frida] H: load_sync returned");
    log_info("[frida] agent running");
    g_main_loop_run(g_loop);
    log_info("[frida] loop exited");
}

void frida_post(const std::string& json) {
    GumScript* s = g_script.load();
    if (!s) return;
    std::lock_guard<std::mutex> lk(g_post_mtx);
    gum_script_post(s, json.c_str(), nullptr);
}

int frida_drain_messages(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(g_msg_mtx);
    out.swap(g_msg_queue);
    return (int)out.size();
}
