#include "menu.h"
#include "frida_bridge.h"
#include "logger.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstring>

// ── Mini JSON extractors (flat object, string/array-of-string) ─
static std::string j_str(const std::string& j, const std::string& key) {
    std::string n = "\"" + key + "\":\"";
    size_t p = j.find(n);
    if (p == std::string::npos) return "";
    p += n.size();
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            char c = j[p+1];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                case 'u': {
                    if (p + 5 < j.size()) {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = j[p+2+k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        }
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        p += 4;
                    }
                    break;
                }
                default: out += c; break;
            }
            p += 2;
        } else out += j[p++];
    }
    return out;
}

static std::vector<std::string> j_arr(const std::string& j, const std::string& key) {
    std::vector<std::string> out;
    std::string n = "\"" + key + "\":[";
    size_t p = j.find(n);
    if (p == std::string::npos) return out;
    p += n.size();
    size_t e = j.find(']', p);
    if (e == std::string::npos) return out;
    std::string seg = j.substr(p, e - p);
    size_t i = 0;
    while (i < seg.size()) {
        while (i < seg.size() && seg[i] != '"') i++;
        if (i >= seg.size()) break;
        i++;
        std::string val;
        while (i < seg.size() && seg[i] != '"') {
            if (seg[i] == '\\' && i + 1 < seg.size()) {
                char c = seg[i+1];
                switch (c) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case 'r': val += '\r'; break;
                    case '"': val += '"';  break;
                    case '\\': val += '\\'; break;
                    case '/': val += '/';  break;
                    case 'u': {
                        if (i + 5 < seg.size()) {
                            unsigned cp = 0;
                            for (int k = 0; k < 4; k++) {
                                char h = seg[i+2+k];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= h - '0';
                                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            }
                            if (cp < 0x80) val += (char)cp;
                            else if (cp < 0x800) {
                                val += (char)(0xC0 | (cp >> 6));
                                val += (char)(0x80 | (cp & 0x3F));
                            } else {
                                val += (char)(0xE0 | (cp >> 12));
                                val += (char)(0x80 | ((cp >> 6) & 0x3F));
                                val += (char)(0x80 | (cp & 0x3F));
                            }
                            i += 4;
                        }
                        break;
                    }
                    default: val += c; break;
                }
                i += 2;
            } else val += seg[i++];
        }
        i++;
        out.push_back(val);
        while (i < seg.size() && seg[i] != ',') i++;
        if (i < seg.size()) i++;
    }
    return out;
}

// ── State ──────────────────────────────────────────────────
static float s_opacity = 0.92f;
static float s_font_sz = 14.f;
static int   s_theme   = 0;
static bool  s_visible = true;

struct PatchEntry  { std::string name, value; bool enabled = true; };
struct HookEntry   { std::string name, code;  bool enabled = true; int mode = 0; };
struct TrackerEntry{ std::string name; std::vector<std::string> log; };
struct ClassWin    { std::string cname; std::vector<std::string> methods, fields; bool loaded = false; };

static std::vector<PatchEntry>   s_patches;
static std::vector<HookEntry>    s_hooks;
static std::vector<TrackerEntry> s_trackers;
static std::vector<ClassWin>     s_classes;

static bool s_rec_on = false, s_mon_on = false;
static std::vector<std::string> s_rec_log, s_mon_log, s_script_out;
static std::mutex g_state_mtx;

static char s_pname[256]  = {};
static char s_pval[512]   = {};
static char s_hname[256]  = {};
static char s_hcode[2048] = {};
static int  s_hmode       = 0;
static char s_tname[256]  = {};
static char s_script[8192]= "// Frida JS\n";
static char s_cname[256]  = {};

static void post_rpc(const char* fn, const char* args) {
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"rpc\",\"fn\":\"%s\",\"args\":%s}", fn, args);
    frida_post(std::string(buf));
}

static void apply_theme() {
    switch (s_theme) {
        case 0: ImGui::StyleColorsDark();    break;
        case 1: ImGui::StyleColorsLight();   break;
        case 2: ImGui::StyleColorsClassic(); break;
    }
    ImGui::GetStyle().Alpha = s_opacity;
    ImGui::GetIO().FontGlobalScale = s_font_sz / 14.0f;
}

// ── RPC message dispatcher ──────────────────────────────────
static void handle_message(const std::string& j) {
    std::lock_guard<std::mutex> lk(g_state_mtx);
    std::string type = j_str(j, "type");
    if (type.empty()) return;

    if (type == "classInfo") {
        std::string cls = j_str(j, "class");
        auto methods = j_arr(j, "methods");
        auto fields  = j_arr(j, "fields");
        for (auto& c : s_classes) {
            if (c.cname == cls) {
                c.methods = std::move(methods);
                c.fields  = std::move(fields);
                c.loaded  = true;
                break;
            }
        }
    } else if (type == "instanceList") {
        std::string cls = j_str(j, "class");
        auto list = j_arr(j, "instances");
        for (auto& c : s_classes) {
            if (c.cname == cls) {
                for (auto& s : list) c.methods.push_back("instance: " + s);
                break;
            }
        }
    } else if (type == "trackerEvent") {
        std::string name = j_str(j, "name");
        auto args = j_arr(j, "args");
        std::string line = "[" + name + "]";
        for (auto& a : args) line += " " + a;
        for (auto& t : s_trackers)
            if (t.name == name) t.log.push_back(line);
    } else if (type == "recorded") {
        std::string line = j_str(j, "cls") + "." + j_str(j, "method");
        s_rec_log.push_back(line);
    } else if (type == "scriptResult" || type == "scriptError") {
        s_script_out.push_back(j_str(j, "result").empty()
            ? j_str(j, "msg") : j_str(j, "result"));
    } else if (type == "sslInit" || type == "monitorInfo") {
        s_mon_log.push_back(j_str(j, "note"));
    } else if (type == "error") {
        s_script_out.push_back("ERR[" + j_str(j, "fn") + "]: " + j_str(j, "msg"));
    } else if (type == "info" || type == "warn") {
        s_script_out.push_back(j_str(j, "msg"));
    }
}

void menu_init() {
    apply_theme();
    frida_set_message_callback([](const std::string& j) {
        handle_message(j);
    });
}

// ── Tabs ────────────────────────────────────────────────────
static void tab_settings() {
    if (ImGui::SliderFloat("Font size", &s_font_sz, 8, 32)) {
        ImGui::GetIO().FontGlobalScale = s_font_sz / 14.0f;
    }
    if (ImGui::SliderFloat("Opacity", &s_opacity, .3f, 1.f)) apply_theme();
    const char* th[] = {"Dark","Light","Classic"};
    if (ImGui::Combo("Theme", &s_theme, th, 3)) apply_theme();
    ImGui::Checkbox("Visible", &s_visible);
}

static void tab_patches() {
    ImGui::InputText("Name##pn",  s_pname, sizeof(s_pname));
    ImGui::InputText("Value##pv", s_pval,  sizeof(s_pval));
    if (ImGui::Button("Add Patch") && s_pname[0]) {
        char a[1024];
        snprintf(a, sizeof(a), "{\"name\":\"%s\",\"value\":\"%s\"}", s_pname, s_pval);
        post_rpc("addPatch", a);
        s_patches.push_back({s_pname, s_pval, true});
        s_pname[0] = s_pval[0] = 0;
    }
    ImGui::Separator();
    for (int i = 0; i < (int)s_patches.size(); i++) {
        ImGui::PushID(i);
        auto& p = s_patches[i];
        if (ImGui::Checkbox("##e", &p.enabled)) {
            char a[256];
            snprintf(a, sizeof(a), "{\"name\":\"%s\",\"enabled\":%s}",
                     p.name.c_str(), p.enabled ? "true" : "false");
            post_rpc("setPatchEnabled", a);
        }
        ImGui::SameLine();
        ImGui::Text("%s=%s", p.name.c_str(), p.value.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            char a[256]; snprintf(a, sizeof(a), "{\"name\":\"%s\"}", p.name.c_str());
            post_rpc("removePatch", a);
            s_patches.erase(s_patches.begin() + i);
            ImGui::PopID(); break;
        }
        ImGui::PopID();
    }
}

static void tab_hooks() {
    const char* modes[] = {"Java","Smali","noop"};
    ImGui::InputText("Method##hn", s_hname, sizeof(s_hname));
    ImGui::Combo("Mode", &s_hmode, modes, 3);
    ImGui::InputTextMultiline("Code##hc", s_hcode, sizeof(s_hcode), ImVec2(-1, 80));
    if (ImGui::Button("Add Hook") && s_hname[0]) {
        char a[2048];
        snprintf(a, sizeof(a), "{\"name\":\"%s\",\"mode\":\"%s\",\"code\":\"%s\"}",
                 s_hname, modes[s_hmode], s_hcode);
        post_rpc("addHook", a);
        s_hooks.push_back({s_hname, s_hcode, true, s_hmode});
        s_hname[0] = s_hcode[0] = 0;
    }
    ImGui::Separator();
    for (int i = 0; i < (int)s_hooks.size(); i++) {
        ImGui::PushID(i);
        auto& h = s_hooks[i];
        if (ImGui::Checkbox("##he", &h.enabled)) {
            char a[256];
            snprintf(a, sizeof(a), "{\"name\":\"%s\",\"enabled\":%s}",
                     h.name.c_str(), h.enabled ? "true" : "false");
            post_rpc("setHookEnabled", a);
        }
        ImGui::SameLine();
        ImGui::Text("[%s] %s", modes[h.mode], h.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            char a[256]; snprintf(a, sizeof(a), "{\"name\":\"%s\"}", h.name.c_str());
            post_rpc("removeHook", a);
            s_hooks.erase(s_hooks.begin() + i);
            ImGui::PopID(); break;
        }
        ImGui::PopID();
    }
}

static void tab_tracker() {
    ImGui::InputText("Method##tn", s_tname, sizeof(s_tname));
    if (ImGui::Button("Track") && s_tname[0]) {
        char a[256]; snprintf(a, sizeof(a), "{\"name\":\"%s\"}", s_tname);
        post_rpc("addTracker", a);
        s_trackers.push_back({s_tname, {}});
        s_tname[0] = 0;
    }
    ImGui::Separator();
    for (int i = 0; i < (int)s_trackers.size(); i++) {
        ImGui::PushID(i);
        auto& t = s_trackers[i];
        if (ImGui::CollapsingHeader(t.name.c_str())) {
            for (auto& l : t.log) ImGui::TextUnformatted(l.c_str());
            if (ImGui::SmallButton("Remove")) {
                char a[256]; snprintf(a, sizeof(a), "{\"name\":\"%s\"}", t.name.c_str());
                post_rpc("removeTracker", a);
                s_trackers.erase(s_trackers.begin() + i);
                ImGui::PopID(); break;
            }
        }
        ImGui::PopID();
    }
}

static void tab_recorder() {
    if (!s_rec_on) {
        if (ImGui::Button("Start")) {
            s_rec_on = true; s_rec_log.clear();
            post_rpc("startRecorder", "{}");
        }
    } else {
        if (ImGui::Button("Stop")) { s_rec_on = false; post_rpc("stopRecorder", "{}"); }
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1), "REC");
    }
    ImGui::Separator();
    ImGui::BeginChild("rl", ImVec2(0, 180), true);
    for (auto& l : s_rec_log) ImGui::TextUnformatted(l.c_str());
    ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

static void tab_monitor() {
    ImGui::TextDisabled("(best-effort: Java SSLContext.init)");
    if (!s_mon_on) {
        if (ImGui::Button("Start")) {
            s_mon_on = true; s_mon_log.clear();
            post_rpc("startMonitor", "{}");
        }
    } else {
        if (ImGui::Button("Stop")) { s_mon_on = false; post_rpc("stopMonitor", "{}"); }
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1), "MON");
    }
    ImGui::Separator();
    ImGui::BeginChild("ml", ImVec2(0, 180), true);
    for (auto& l : s_mon_log) ImGui::TextUnformatted(l.c_str());
    ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

static void tab_script() {
    ImGui::InputTextMultiline("##sc", s_script, sizeof(s_script), ImVec2(-1, 140));
    if (ImGui::Button("Run")) {
        s_script_out.clear();
        char a[8256]; snprintf(a, sizeof(a), "{\"src\":\"%s\"}", s_script);
        post_rpc("evalScript", a);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) s_script_out.clear();
    ImGui::Separator();
    ImGui::BeginChild("so", ImVec2(0, 120), true);
    for (auto& l : s_script_out) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();
}

static void tab_classes() {
    ImGui::InputText("Class##cn", s_cname, sizeof(s_cname));
    ImGui::SameLine();
    if (ImGui::Button("Open") && s_cname[0]) {
        char a[256]; snprintf(a, sizeof(a), "{\"class\":\"%s\"}", s_cname);
        post_rpc("openClass", a);
        s_classes.push_back({s_cname, {}, {}, false});
        s_cname[0] = 0;
    }
    for (int i = 0; i < (int)s_classes.size(); i++) {
        ImGui::PushID(i);
        auto& c = s_classes[i];
        if (ImGui::CollapsingHeader(c.cname.c_str())) {
            if (!c.loaded) ImGui::TextDisabled("Loading…");
            if (ImGui::Button("List Instances")) {
                char a[256]; snprintf(a, sizeof(a), "{\"class\":\"%s\"}", c.cname.c_str());
                post_rpc("listInstances", a);
            }
            ImGui::Text("Methods:");
            for (auto& m : c.methods) {
                ImGui::BulletText("%s", m.c_str()); ImGui::SameLine();
                if (ImGui::SmallButton("Hook")) {
                    char a[256];
                    snprintf(a, sizeof(a), "{\"class\":\"%s\",\"method\":\"%s\"}",
                             c.cname.c_str(), m.c_str());
                    post_rpc("hookMethod", a);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Track")) {
                    char a[256];
                    snprintf(a, sizeof(a), "{\"class\":\"%s\",\"method\":\"%s\"}",
                             c.cname.c_str(), m.c_str());
                    post_rpc("trackMethod", a);
                }
            }
            ImGui::Text("Fields:");
            for (auto& f : c.fields) {
                ImGui::BulletText("%s", f.c_str()); ImGui::SameLine();
                if (ImGui::SmallButton("Patch")) {
                    char a[256];
                    snprintf(a, sizeof(a), "{\"class\":\"%s\",\"field\":\"%s\"}",
                             c.cname.c_str(), f.c_str());
                    post_rpc("patchField", a);
                }
            }
        }
        ImGui::PopID();
    }
}

void menu_draw() {
    if (!s_visible) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * .75f, 500), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(s_opacity);
    ImGui::Begin("Dex-tool v1.1", &s_visible, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("tb")) {
        if (ImGui::BeginTabItem("Settings")) { tab_settings(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Patches"))  { tab_patches();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Hooks"))    { tab_hooks();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Tracker"))  { tab_tracker();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Recorder")) { tab_recorder(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Monitor"))  { tab_monitor();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Script"))   { tab_script();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Classes"))  { tab_classes();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
