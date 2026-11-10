#include "frida_bridge.h"
#include "logger.h"
#include "frida-gumjs.h"
#include "agent_bundle_embed.h"
#include <deque>
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
/* r22/audit: agent messages that arrive before menu_init()
   registers the callback (the one-shot "ready" usually does) are
   kept and replayed when it does */
static std::deque<std::string> g_backlog;

/* GumJS `msg` for send(x) is: {"type":"send","payload":<x>}.
   menu.cpp wants <x> (the agent's own JSON object), not the whole
   envelope, so extract the payload substring. Log every agent
   "error" message verbatim - r6 could never match it because the
   escaping inside the envelope hid the quotes. */
static std::string extract_payload(const char* msg) {
    if (!msg) return "";
    std::string m(msg);
    size_t p = m.find("\"payload\":");
    if (p == std::string::npos) return "";
    p += 10;   /* "payload": is 10 chars incl. quotes+colon */
    while (p < m.size() && m[p] == ' ') p++;
    /* payload runs to the end of the envelope object */
    size_t e = m.rfind('}');
    if (e == std::string::npos || e < p) return "";
    return m.substr(p, e - p);
}

/* raw (still JSON-escaped) value of a string key, or "" */
static std::string raw_json_string(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t i = p;
    while (i < j.size() && j[i] != '"') {
        if (j[i] == '\\' && i + 1 < j.size()) i++;
        i++;
    }
    return j.substr(p, i - p);
}

static const char kErrEnvelope[] = "{\"type\":\"error\"";

static void on_message(const gchar* msg, GBytes*, gpointer) {
    if (!msg) return;
    std::string pl = extract_payload(msg);
    if (pl.empty() && strncmp(msg, kErrEnvelope, sizeof(kErrEnvelope) - 1) == 0) {
        /* r22/audit: an uncaught JS exception raised by GumJS itself
           arrives as {"type":"error",...} with NO "payload" key, so
           it used to be dropped silently. Log it and forward the
           description to the menu in the agent's own shape. */
        std::string d  = raw_json_string(msg, "description");
        std::string st = raw_json_string(msg, "stack");
        char em[1200];
        snprintf(em, sizeof(em), "[js error] %.500s | %.600s\n",
                 d.c_str(), st.c_str());
        log_crash_raw(em);
        pl = "{\"type\":\"error\",\"payload\":{\"fn\":\"js\",\"msg\":\"" + d + "\"}}";
    }
    if (!pl.empty()) {
        const char* t = strstr(pl.c_str(), "\"type\":\"");
        if (t) {
            char tag[72];
            snprintf(tag, sizeof(tag), "[msg] %.48s", t + 8);
            char* q = strchr(tag, '"');
            if (q) *q = '\0';
            log_debug(tag);
            if (strncmp(tag, "[msg] error", 11) == 0) {
                char em[640];
                snprintf(em, sizeof(em), "[agent error] %.600s\n", pl.c_str());
                log_crash_raw(em);
            }
        }
    }
    MsgCallback local;
    {
        std::lock_guard<std::mutex> lk(g_pmtx);
        local = g_cb;
        if (!local && !pl.empty()) {       /* menu not up yet: keep for it */
            if (g_backlog.size() < 256) g_backlog.push_back(pl);
            return;
        }
    }
    if (local && !pl.empty()) local(pl);
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
    if (!g_script) {
        /* r22/audit: say so instead of vanishing - the menu's first
           messages used to disappear without a trace */
        log_debug("[frida_post] dropped: agent not loaded yet");
        return;
    }
    std::lock_guard<std::mutex> lk(g_pmtx);
    gum_script_post(g_script, json.c_str(), nullptr);
}

void frida_set_message_callback(MsgCallback cb) {
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(g_pmtx);
        g_cb = cb;
        if (cb) pending.swap(g_backlog);
    }
    if (cb) for (const std::string& m : pending) cb(m);   /* replay early ones */
}
