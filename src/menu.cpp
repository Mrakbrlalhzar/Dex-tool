#include "menu.h"
#include "overlay.h"
#include "frida_bridge.h"
#include "logger.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <pthread.h>
#include <ctime>

/* =====================================================================
   Dex-tool menu (r12)
   - every tab of the spec: Settings, Patches, Hooks, Tracker, Recorder,
     Monitor, Script, Classes
   - Classes lists ALL loaded classes by default (the agent pages them
     in); the filter row is optional
   - text input via the PHONE's keyboard: focusing a field posts
     showIme to the agent (hidden EditText + TextWatcher -> add_char/
     backspace); the r11 drawn keyboard is gone
   - DEADLOCK FIX ("open a class and the app freezes"): g_mtx is a
     RECURSIVE mutex now. tab_classes() used to lock it a second time
     while already holding it, and class_tab_content() nested again -
     the render thread self-deadlocked inside the eglSwapBuffers hook,
     which froze the host's GL thread: the whole app hung.
   - cross-linking: clicking a row in Tracker / Recorder / Monitor opens
     that class in the Classes tab (menu_open_class)
   - the recorder/monitor lists are fed by the agent in throttled
     batches; the recorder is scoped (package filter + method cap)
   - scrollable areas are published to the overlay, which converts
     drags inside them into wheel events at the configured speed
   - UI state persists to <logdir>/dextool-ui.conf
   ===================================================================== */

/* recursive: tab_classes / class_tab_content / menu_open_class nest */
static pthread_mutex_t g_mtx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#define MLK()   pthread_mutex_lock(&g_mtx)
#define MULK()  pthread_mutex_unlock(&g_mtx)

/* ---------------- settings ---------------- */
static float s_font_size  = 18.0f;
static float s_opacity    = 0.92f;
static int   s_theme      = 0;
static float s_ovl_scroll = 1.5f;   /* whole-menu drag speed */
static float s_lst_scroll = 1.2f;   /* inner list drag speed */
static bool  s_visible    = true;
static bool  s_show_fab   = true;
static float s_y_off      = 0.0f;   /* manual touch Y bias (px); r15: default 0 - the screen-space touch map is mathematically exact, the slider stays for exotic hosts */

/* ---------------- agent / overlay status ---------------- */
static bool  s_agent_ready = false;
static char  s_agent_note[160] = "";
static bool  s_ovl_ready   = false;

/* ---------------- scroll region publication ---------------- */
static std::vector<float> s_regions;
static void mark_rect(float x, float y, float w, float h, float speed) {
    if (w <= 1 || h <= 1 || speed <= 0.01f) return;
    s_regions.push_back(x); s_regions.push_back(y);
    s_regions.push_back(w); s_regions.push_back(h);
    s_regions.push_back(speed);
}
/* call right after EndChild(): the child window is the last item */
static void mark_scroll_item(float speed) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    mark_rect(a.x, a.y, b.x - a.x, b.y - a.y, speed);
}

/* ---------------- data model ---------------- */
struct MethodSig { std::string name, params, ret; };
struct FieldInfo { std::string name, type, value; };
/* r23: instances arrive as structured JSON objects (see jobjarr);
   handle is the agent's retained-instance id for setInstanceField /
   callMethod - the old index-only addressing had no receiver at all */
struct InstInfo  { std::string str; long handle = -1;
                   std::vector<FieldInfo> fields; };
struct ClassTab {
    std::string cname;
    bool got_info = false, waiting = false;
    std::vector<MethodSig> methods;
    std::vector<FieldInfo> fields;
    std::vector<InstInfo>  instances;
    int  sel_inst = -1;
    int  gen = -1;                      /* r23: instance-list generation; part of the editor buffer key */
    int  sub = 0;                       /* 0 Instances 1 Methods 2 Fields */
    /* inline method-call editor */
    bool call_open = false;
    MethodSig call_sig;
    char call_args[8][128] = {};
    std::string call_result;
    /* r25: method input/output patch editor. The agent probes ONE
       call of the method (ioprobe layer), reports the captured
       current values, and only the rows the user P-marked are
       patched from then on. */
    bool io_open  = false;              /* editor visible            */
    bool io_wait  = false;              /* probe in flight           */
    MethodSig io_sig;                   /* which method is edited    */
    int  io_nargs = 0;
    char io_arg_typ[8][64]  = {};       /* parameter types           */
    char io_arg_cur[8][128] = {};       /* captured current values   */
    char io_arg[8][128]     = {};       /* patch values (editable)   */
    bool io_arg_on[8]       = {};       /* patch this argument       */
    char io_ret_typ[64]     = {};
    char io_ret_cur[128]    = {};
    char io_ret[128]        = {};
    bool io_ret_on          = false;
    std::string io_result;
    /* r25: members that contain the code-text search string - the
       Methods/Fields lists paint these rows green */
    std::vector<std::string> hits_m, hits_f;
    bool hit(const std::vector<std::string>& v, const std::string& n) const {
        for (auto& s : v) if (s == n) return true;
        return false;
    }
};
struct PatchRow { std::string cls, field, value; bool enabled=true;
                  std::string note; };
struct HookRow  { std::string cls, method; int mode=0; std::string code;
                  bool enabled=true; std::string note; };
struct TrackRow { std::vector<std::string> callers; std::string ret,
                  args, err; };
struct Tracker  { std::string name;              /* full Class.member */
                  bool is_field=false;
                  std::vector<TrackRow> log; };
struct RecRow   { std::string cls, method, args, ret, extra; };
/* r25: detail carries the payload/body preview shown in the detail
   window (request body, response body peek, stream note) */
struct MonRow   { std::string kind, url, method, origin, note, detail; };

static std::vector<ClassTab>  s_classes;
static std::vector<PatchRow>   s_patches;
static std::vector<HookRow>    s_hooks;
static std::vector<Tracker>    s_trackers;
static std::vector<RecRow>     s_rec;
static std::vector<MonRow>     s_mon;
static std::vector<std::string> s_cons;      /* script console */

static bool  s_rec_on=false;  static char s_rec_stat[128]="idle";
static int   s_rec_installed=0, s_rec_total=0;
static char  s_rec_filter[120]="";           /* empty = host package */
static int   s_rec_cap=1500;                 /* method cap */
static bool  s_mon_on=false;  static char s_mon_stat[160]="off";

static char s_pcls[200]={}, s_pfld[128]={}, s_pval[256]={};
static int  s_pedit=-1;
static char s_hcls[200]={}, s_hmth[128]={}, s_hcode[4096]={};
static int  s_hmode=0, s_hedit=-1;
static char s_tcls[200]={}, s_tmth[128]={};
static char s_script[16384]="// Frida JS - Java, send, console are available\n";
static char s_search[160]="";
/* r25: 0 class name, 1 method name, 2 field name, 3 code text.
   s_search_sys=false hides android/androidx/com.android/com.google/
   java/javax/dalvik/kotlin packages (the field report asked for
   "no Android and Google classes"); the checkbox brings them back. */
static int  s_search_mode = 0;
static bool s_search_sys  = false;
/* r25: read-only source panel of the Hooks tab (real smali via the
   bundled baksmali; Java decompilation is NOT bundled) */
static char s_hsrc[16384] = "";
static char s_hsrc_note[200] = "";
static bool s_hsrc_have = false;
/* r25: selected row of the Monitor list -> detail window */
static int  s_mon_sel = -1;
static std::vector<std::string> s_search_res;
static bool s_cls_auto_done=false;   /* full class list requested once
                                        per agent session */
static int  s_search_total=0, s_search_shown=0;
static bool s_search_wait=false;
static int  s_search_seq=0, s_search_qid=-1;
static bool s_search_page0=false;    /* r23: set when a NEW qid is sent;
                                        the first page OF THAT QID clears
                                        the list - stale pages of an older
                                        qid are dropped, not mixed in */
static std::string s_focus_cls;          /* class tab to select */
static std::string s_next_tab;           /* top tab to select */

/* ---------------- JSON helpers ---------------- */
static std::string jesc(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    r += b;
                } else r += (char)c;
        }
    }
    return r;
}

/* unescape a JSON string body (agent -> menu direction); r6/r7 fed
   escaped text (with \" and \\n) straight into the UI */
static std::string junesc(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { r += s[i]; continue; }
        char n = s[++i];
        switch (n) {
            case 'n': r += '\n'; break;
            case 't': r += '\t'; break;
            case 'r': r += '\r'; break;
            case 'b': r += '\b'; break;
            case 'f': r += '\f'; break;
            case '"': r += '"';  break;
            case '\\':r += '\\'; break;
            case '/': r += '/';  break;
            case 'u':
                if (i + 4 < s.size()) {
                    unsigned v = 0;
                    for (int k = 1; k <= 4; k++) {
                        char h = s[i + k];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                        else { v = 0xFFFF; break; }
                    }
                    i += 4;
                    if (v < 0x80) r += (char)v;
                    else if (v < 0x800) {
                        r += (char)(0xC0 | (v >> 6));
                        r += (char)(0x80 | (v & 0x3F));
                    } else {
                        r += (char)(0xE0 | (v >> 12));
                        r += (char)(0x80 | ((v >> 6) & 0x3F));
                        r += (char)(0x80 | (v & 0x3F));
                    }
                } else r += n;
                break;
            default: r += n; break;
        }
    }
    return r;
}

/* string array; skips \" so values containing quotes survive */
static std::vector<std::string> jarr(const std::string& j, const char* key) {
    std::vector<std::string> r;
    std::string k = std::string("\"")+key+"\":";
    auto p = j.find(k);
    if (p == std::string::npos) return r;
    p = j.find('[', p + k.size());
    if (p == std::string::npos) return r;
    size_t i = p + 1;
    while (i < j.size()) {
        while (i < j.size() && (j[i]==' '||j[i]==','||j[i]=='\n')) i++;
        if (i >= j.size() || j[i] != '"') break;
        size_t s = ++i;
        std::string val;
        while (i < j.size()) {
            if (j[i] == '\\' && i + 1 < j.size()) { i += 2; continue; }
            if (j[i] == '"') break;
            i++;
        }
        if (i > s) val = j.substr(s, i - s);
        r.push_back(junesc(val));
        i++;                      /* closing quote */
    }
    return r;
}

static std::string jstr(const std::string& j, const char* key) {
    std::string k = std::string("\"")+key+"\":\"";
    auto p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    std::string val;
    size_t i = p;
    while (i < j.size()) {
        if (j[i] == '\\' && i + 1 < j.size()) { i += 2; continue; }
        if (j[i] == '"') break;
        i++;
    }
    if (i > p) val = j.substr(p, i - p);
    return junesc(val);
}

static long jnum(const std::string& j, const char* key, long dflt=0) {
    std::string k = std::string("\"")+key+"\":";
    auto p = j.find(k);
    if (p == std::string::npos) return dflt;
    p += k.size();
    while (p < j.size() && j[p]==' ') p++;
    bool neg = false;
    if (p < j.size() && j[p]=='-') { neg = true; p++; }
    long v = 0; bool any=false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        v = v*10 + (j[p]-'0'); p++; any=true;
    }
    return any ? (neg ? -v : v) : dflt;
}

static bool jhas(const std::string& j, const char* key) {
    /* r23: colon-anchored - the old substring test matched "key"
       INSIDE string values (a class named ...ok... satisfied
       jhas(j,"ok")); the key must now be followed by ':' to count */
    return j.find(std::string("\"") + key + "\":") != std::string::npos;
}

/* r23: EXACT value of the top-level "type" field. The whole message
   dispatch used to be a chain of jhas() substring tests, so a payload
   that merely CONTAINED the word ("...ready...", a URL holding
   "info", a class name ending in "error") hijacked the branch meant
   for another message. Every branch now compares this exact string. */
static std::string jtype(const std::string& j) {
    const std::string k = "\"type\":\"";
    auto p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t i = p;
    while (i < j.size() && j[i] != '"') {
        if (j[i] == '\\' && i + 1 < j.size()) i++;
        i++;
    }
    return j.substr(p, i - p);
}

/* r23: array of JSON OBJECTS; each element is returned as its
   balanced {...} substring (nesting + quoted braces handled).
   The instance list used a "toString##name=value;name=value" string
   protocol whose | # ; delimiters corrupted every value that
   contained those characters - the audit's "value corruption on
   edit". Fields now travel as structured JSON. */
static std::vector<std::string> jobjarr(const std::string& j,
                                        const char* key) {
    std::vector<std::string> r;
    std::string k = std::string("\"") + key + "\":[";
    auto p = j.find(k);
    if (p == std::string::npos) return r;
    p += k.size();
    while (p < j.size()) {
        while (p < j.size() && (j[p] == ' ' || j[p] == ',')) p++;
        if (p >= j.size() || j[p] != '{') break;
        size_t s = p;
        int depth = 0;
        bool instr = false;
        while (p < j.size()) {
            char c = j[p];
            if (instr) {
                if (c == '\\') p++;
                else if (c == '"') instr = false;
            } else {
                if (c == '"') instr = true;
                else if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) { p++; break; }
                }
            }
            p++;
        }
        r.push_back(j.substr(s, p - s));
    }
    return r;
}

static void post_rpc(const char* fn, const std::string& args) {
    std::string m = "{\"type\":\"rpc\",\"fn\":\"";
    m += fn; m += "\",\"args\":"; m += args; m += "}";
    frida_post(m);
}

static void push_console(const char* tag, const std::string& line) {
    char b[256];
    snprintf(b, sizeof(b), "%s %s", tag, line.c_str());
    /* r24: keep the NEWEST 400 lines - the old guard DROPPED every
       line past 400, so the console simply appeared to stop */
    s_cons.push_back(b);
    if (s_cons.size() > 400) s_cons.erase(s_cons.begin());
}

/* ---------------- cross-linking ---------------- */
void menu_open_class(const std::string& name) {
    if (name.empty()) return;
    MLK();
    bool have = false;
    for (auto& c : s_classes)
        if (c.cname == name) { have = true; c.waiting = true; break; }
    if (!have) {
        ClassTab nt{};
        nt.cname = name;
        nt.waiting = true;
        s_classes.push_back(nt);
    }
    s_focus_cls = name;
    s_next_tab  = "Classes";
    MULK();
    /* refresh from the agent (methods/fields); reply fills the tab */
    std::string a = "{\"class\":\"" + jesc(name) + "\"}";
    post_rpc("openClass", a);
}

static ClassTab* find_tab(const std::string& n) {
    for (auto& c : s_classes) if (c.cname == n) return &c;
    return nullptr;
}

static int find_tracker(const std::string& n) {
    for (size_t i = 0; i < s_trackers.size(); i++)
        if (s_trackers[i].name == n) return (int)i;
    return -1;
}

/* parse "Class.method(arg,args) -> ret" and "name:type=value" */
static MethodSig parse_sig(const std::string& s) {
    MethodSig m;
    auto po = s.find('(');
    if (po == std::string::npos) { m.name = s; return m; }
    m.name = s.substr(0, po);
    auto pc = s.find(')', po);
    if (pc != std::string::npos) {
        m.params = s.substr(po + 1, pc - po - 1);
        auto pr = s.find("->", pc);
        if (pr != std::string::npos) {
            size_t b = pr + 2;
            while (b < s.size() && s[b] == ' ') b++;
            m.ret = s.substr(b);
        }
    }
    return m;
}
static FieldInfo parse_field(const std::string& s) {
    FieldInfo f;
    auto c1 = s.find(':');
    if (c1 == std::string::npos) { f.name = s; return f; }
    f.name = s.substr(0, c1);
    auto e = s.find('=', c1);
    if (e == std::string::npos) { f.type = s.substr(c1 + 1); return f; }
    f.type  = s.substr(c1 + 1, e - c1 - 1);
    f.value = s.substr(e + 1);
    return f;
}
/* "cls.method|args|ret|extra" (agent batches) */
static RecRow parse_rec(const std::string& s) {
    RecRow r;
    auto p1 = s.find('|');
    if (p1 == std::string::npos) { r.method = s; return r; }
    std::string full = s.substr(0, p1);
    auto dot = full.rfind('.');
    r.cls   = (dot == std::string::npos) ? full : full.substr(0, dot);
    r.method= (dot == std::string::npos) ? "" : full.substr(dot + 1);
    auto p2 = s.find('|', p1 + 1);
    auto p3 = s.find('|', p2 + 1);
    if (p2 == std::string::npos) return r;
    r.args = s.substr(p1 + 1, p2 - p1 - 1);
    if (p3 == std::string::npos) { r.ret = s.substr(p2 + 1); return r; }
    r.ret   = s.substr(p2 + 1, p3 - p2 - 1);
    r.extra = s.substr(p3 + 1);
    return r;
}
/* "kind|url|method|origin|note|detail" (r25: + payload preview) */
static MonRow parse_mon(const std::string& s) {
    MonRow m;
    auto p1 = s.find('|'); if (p1 == std::string::npos) { m.note=s; return m; }
    m.kind = s.substr(0, p1);
    auto p2 = s.find('|', p1+1); if (p2 == std::string::npos) return m;
    m.url = s.substr(p1+1, p2-p1-1);
    auto p3 = s.find('|', p2+1); if (p3 == std::string::npos) return m;
    m.method = s.substr(p2+1, p3-p2-1);
    auto p4 = s.find('|', p3+1);
    if (p4 == std::string::npos) { m.origin = s.substr(p3+1); return m; }
    m.origin = s.substr(p3+1, p4-p3-1);
    auto p5 = s.find('|', p4+1);
    if (p5 == std::string::npos) { m.note = s.substr(p4+1); return m; }
    m.note   = s.substr(p4+1, p5-p4-1);
    m.detail = s.substr(p5+1);
    return m;
}
/* (r23: parse_inst() removed with the "a##n=v;n=v" instance protocol -
   instances are structured JSON now, see the instanceList branch) */

/* strip the trailing ".method" from a caller string "pkg.Cls.method" */
static std::string cls_of_member(const std::string& s) {
    auto d = s.rfind('.');
    if (d == std::string::npos || d == 0) return s;
    return s.substr(0, d);
}

/* ---------------- UI persistence ---------------- */
static char  s_conf_path[512] = "";
static bool  s_conf_dirty = false;
static long  s_conf_last_ms = 0;

static long conf_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void conf_load() {
    snprintf(s_conf_path, sizeof(s_conf_path), "%s/dextool-ui.conf",
             logger_base_dir());
    FILE* f = fopen(s_conf_path, "r");
    if (!f) return;
    char ln[128];
    while (fgets(ln, sizeof(ln), f)) {
        char* eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = ln;
        float v = strtof(eq + 1, nullptr);
        if      (!strcmp(k, "font"))   s_font_size = v;
        else if (!strcmp(k, "opacity")) s_opacity  = v;
        else if (!strcmp(k, "theme"))  s_theme    = (int)v;
        else if (!strcmp(k, "ovl"))    s_ovl_scroll = v;
        else if (!strcmp(k, "lst"))    s_lst_scroll = v;
        else if (!strcmp(k, "ybias"))  s_y_off    = v;   /* r15: key renamed from yoff so a stale saved -80 cannot fight the new mapping */
        else if (!strcmp(k, "fab"))    s_show_fab = v > 0.5f;
    }
    fclose(f);
}

static void conf_save() {
    FILE* f = fopen(s_conf_path, "w");
    if (!f) return;
    fprintf(f, "font=%g\n",   s_font_size);
    fprintf(f, "opacity=%g\n", s_opacity);
    fprintf(f, "theme=%d\n",  s_theme);
    fprintf(f, "ovl=%g\n",    s_ovl_scroll);
    fprintf(f, "lst=%g\n",    s_lst_scroll);
    fprintf(f, "ybias=%g\n",  s_y_off);
    fprintf(f, "fab=%d\n",   s_show_fab ? 1 : 0);
    fclose(f);
    s_conf_dirty = false;
    s_conf_last_ms = conf_now_ms();
}

/* ---------------- theme ---------------- */
static void apply_theme() {
    switch (s_theme) {
        case 0: ImGui::StyleColorsDark();    break;
        case 1: ImGui::StyleColorsLight();   break;
        case 2: ImGui::StyleColorsClassic(); break;
    }
    /* r24: style.Alpha is GONE - it faded EVERYTHING (text, child
       windows, the FAB) while the spec wants only the window
       BACKGROUND transparent. menu_draw() uses
       SetNextWindowBgAlpha(s_opacity), the single opacity source. */
    ImGui::GetIO().FontGlobalScale = 1.0f;   /* real size comes from the
                                               atlas, not from scaling */
}

/* ===================== system keyboard (IME) bridge =====================
   r12. The r11 drawn keyboard is GONE. A text field that gains focus
   asks the agent to pop the PHONE's own soft keyboard: the menu posts
   showIme / hideIme over the frida bridge; the agent focuses a hidden
   1x1 EditText on the resumed activity, and its TextWatcher converts
   IME edits into dextool_add_char()/dextool_backspace() calls.
   overlay.cpp drains those into ImGui as ordinary character events,
   so the ACTIVE field receives the text with no extra plumbing here.
   Closing the IME (Done / back) sends imeHidden, which releases the
   ImGui field - so the next tap on it re-opens the keyboard. */
static bool s_ime_active = false;   /* a text field owns ImGui focus */
static bool s_ime_clear  = false;   /* drop active id next frame      */
static ImGuiID s_ime_owner = 0;     /* r23: WHICH field owns the IME */
static int  s_ime_enter = 0;        /* r24: 2 = inject Enter DOWN, 1 = UP */
static bool s_ime_enter_rel = false; /* r24: release the field after the key */

static void ime_show() { post_rpc("showIme", "{}"); }
static void ime_hide() { post_rpc("hideIme", "{}"); }

static void ime_close_if_active() {
    if (s_ime_active) { s_ime_active = false; ime_hide(); }
}

/* once per frame, before the menu window: if the user closed the
   phone keyboard, release the ImGui field as well. r24: forward the
   phone IME's Enter (Done/Search) as a real ImGui key press - DOWN
   this frame, UP the next (both in one frame would be swallowed,
   key state is sampled once per frame). The active InputText sees
   the DOWN and, with EnterReturnsTrue (the class filter), fires
   its search. */
static void ime_frame_prelude() {
    bool clear = false, enter_down = false, enter_up = false;
    MLK();
    clear = s_ime_clear;
    s_ime_clear = false;
    if (clear) s_ime_active = false;
    if (s_ime_enter == 2) { s_ime_enter = 1; enter_down = true; }
    else if (s_ime_enter == 1) {
        s_ime_enter = 0; enter_up = true;
        if (s_ime_enter_rel) {
            s_ime_enter_rel = false;
            if (s_ime_active) { s_ime_active = false; clear = true; }
        }
    }
    MULK();
    if (clear) ImGui::ClearActiveID();
    if (enter_down || enter_up) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Enter, enter_down);
    }
}

/* plain InputText wired to the system keyboard. r23: `extra` lets a
   caller add flags (the class filter uses EnterReturnsTrue so a
   keystroke no longer re-queries enumerateLoadedClasses). */
static bool ime_textfield(const char* label, char* buf, size_t cap,
                          ImGuiInputTextFlags extra = 0) {
    bool used = ImGui::InputText(label, buf, cap,
                                 ImGuiInputTextFlags_AutoSelectAll | extra);
    if (ImGui::IsItemActivated()) {
        s_ime_active = true;
        s_ime_owner  = ImGui::GetItemID();
        ime_show();
    } else if (ImGui::IsItemDeactivated()) {
        /* r23 IME focus order: ImGui fires the OLD field's deactivation
           one frame AFTER the NEW field activated - the old code called
           ime_close_if_active() then, which hid the keyboard the new
           field had just opened ("tapping a field while another is
           active closes the keyboard"). Only the OWNING field
           deactivating releases the IME. */
        if (s_ime_active && ImGui::GetItemID() == s_ime_owner) {
            s_ime_active = false;
            ime_hide();
        }
    } else if (s_ime_active && ImGui::GetItemID() == s_ime_owner &&
               ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
        ime_show();     /* re-tap while active: keyboard was closed */
    }
    return used;
}

/* same for multiline editors (script editor, hook code) */
static bool ime_multiline(const char* label, char* buf, size_t cap,
                          ImVec2 size) {
    bool used = ImGui::InputTextMultiline(label, buf, cap, size);
    if (ImGui::IsItemActivated()) {
        s_ime_active = true;
        s_ime_owner  = ImGui::GetItemID();
        ime_show();
    } else if (ImGui::IsItemDeactivated()) {
        if (s_ime_active && ImGui::GetItemID() == s_ime_owner) {
            s_ime_active = false;
            ime_hide();
        }
    } else if (s_ime_active && ImGui::GetItemID() == s_ime_owner &&
               ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
        ime_show();
    }
    return used;
}


/* ---------------- message callback (GumJS thread) ---------------- */
void menu_init() {
    apply_theme();
    conf_load();
    overlay_set_touch_bias(s_y_off);
    overlay_request_font_rebuild(s_font_size);
    frida_set_message_callback([](const std::string& j) {
        MLK();
        /* r23: dispatch on the EXACT "type" value (jtype) instead of
           jhas() substring tests - see jtype() above. */
        std::string ty = jtype(j);
        if (ty == "ready") {
            s_agent_ready = true;
            s_cls_auto_done = false;  /* new agent: re-list classes */
            snprintf(s_agent_note, sizeof(s_agent_note), "%s",
                     jstr(j, "msg").c_str());
        } else if (ty == "imeEnter") {
            /* r24: the phone IME's Enter/Search - the menu injects a
               real Enter key press over the next two frames so
               fields with EnterReturnsTrue (class filter) search */
            s_ime_enter = 2;
        } else if (ty == "imeHidden") {
            /* user closed the phone keyboard (Done / back):
               drop ImGui focus so the next tap re-opens it.
               r24: with an Enter forward in flight the release is
               DEFERRED - clearing now would deactivate the field
               before the key arrives and the search would be lost. */
            if (s_ime_active) {
                if (s_ime_enter > 0) s_ime_enter_rel = true;
                else { s_ime_active = false; s_ime_clear = true; }
            }
        } else if (ty == "staticValues") {
            /* r12: static values are read on demand only - reading
               them during openClass() ran <clinit> of arbitrary
               classes and could block the agent forever */
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                for (auto& f : jarr(j, "fields")) {
                    FieldInfo nf = parse_field(f);
                    for (auto& old : c->fields)
                        if (old.name == nf.name) { old.value = nf.value; break; }
                }
            }
        } else if (ty == "classInfo") {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->got_info = true; c->waiting = false;
                c->methods.clear();
                for (auto& m : jarr(j, "methods"))
                    c->methods.push_back(parse_sig(m));
                c->fields.clear();
                for (auto& f : jarr(j, "fields"))
                    c->fields.push_back(parse_field(f));
                /* r25: members whose code contains the active
                   code-text search string (empty unless the last
                   search was mode 3) - painted green below */
                c->hits_m = jarr(j, "hits_m");
                c->hits_f = jarr(j, "hits_f");
            }
        } else if (ty == "methodSrc") {
            /* r25: real smali of one method (bundled baksmali) for
               the read-only source panel of the Hooks tab */
            if (jstr(j, "class") == s_hcls &&
                jstr(j, "method") == s_hmth) {
                if (jnum(j, "ok")) {
                    std::string src = jstr(j, "src");
                    snprintf(s_hsrc, sizeof(s_hsrc), "%s", src.c_str());
                    snprintf(s_hsrc_note, sizeof(s_hsrc_note),
                             "smali (bytecode) of %s.%s",
                             s_hcls, s_hmth);
                    s_hsrc_have = true;
                } else {
                    s_hsrc[0] = 0;
                    s_hsrc_have = false;
                    snprintf(s_hsrc_note, sizeof(s_hsrc_note),
                             "source: %s", jstr(j, "error").c_str());
                }
            }
        } else if (ty == "methodIO") {
            /* r25: captured input/output values of ONE call - fills
               the per-method IO patch editor. The reply must match
               the method the user probed, or an IO opened on a
               second method would be filled by the first probe. */
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c && c->io_wait && jstr(j, "method") == c->io_sig.name) {
                c->io_wait = false;
                if (!jnum(j, "ok")) {
                    c->io_result = "probe: " + jstr(j, "error");
                } else {
                    int idx = 0;
                    for (auto& o : jobjarr(j, "params")) {
                        if (idx >= 8) break;
                        snprintf(c->io_arg_typ[idx], sizeof(c->io_arg_typ[idx]),
                                 "%s", jstr(o, "t").c_str());
                        std::string v = jstr(o, "v");
                        snprintf(c->io_arg_cur[idx], sizeof(c->io_arg_cur[idx]),
                                 "%s", v.c_str());
                        snprintf(c->io_arg[idx], sizeof(c->io_arg[idx]),
                                 "%s", v.c_str());
                        c->io_arg_on[idx] = false;   /* only edited+P rows patch */
                        idx++;
                    }
                    c->io_nargs = idx;
                    snprintf(c->io_ret_typ, sizeof(c->io_ret_typ),
                             "%s", jstr(j, "rettyp").c_str());
                    std::string rv = jstr(j, "ret");
                    snprintf(c->io_ret_cur, sizeof(c->io_ret_cur), "%s", rv.c_str());
                    snprintf(c->io_ret, sizeof(c->io_ret), "%s", rv.c_str());
                    c->io_ret_on = false;
                    c->io_open = true;
                    c->io_result.clear();
                }
            }
        } else if (ty == "classList") {
            /* r11: the agent streams the full result in pages of 400;
               the r10 single message was capped at 800 names, which is
               why "Classes" never displayed all classes */
            long qid  = jnum(j, "qid");
            long page = jnum(j, "page");
            /* r23: generation counter - pages of an OUTDATED qid are
               DROPPED, never mixed in. The old `page == 0` force-adopt
               let a slow first page of an older query clobber the
               fresh list (stale search pages). The first page of the
               CURRENT qid clears the buffer once. */
            if (qid == (long)s_search_qid) {
                if (!s_search_page0) {
                    s_search_res.clear();
                    s_search_page0 = true;
                }
                for (auto& c : jarr(j, "classes")) s_search_res.push_back(c);
                s_search_total = (int)jnum(j, "total");
                s_search_shown = (int)jnum(j, "shown");
                if (page + 1 >= jnum(j, "pages")) s_search_wait = false;
            }
        } else if (ty == "instanceList") {
            /* r23: structured instances - {str, handle, fields:[{n,t,v}]}.
               The old "##"/";" string protocol corrupted every value
               containing | # or ;, and index-only addressing gave
               setInstanceField/callMethod NO receiver at all. gen keys
               the editor buffers; handle addresses the retained
               instance on the agent side. */
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->instances.clear();
                c->gen = (int)jnum(j, "gen");
                for (auto& o : jobjarr(j, "instances")) {
                    InstInfo in;
                    in.str    = jstr(o, "str");
                    in.handle = jnum(o, "handle", -1);
                    for (auto& fo : jobjarr(o, "fields")) {
                        FieldInfo fi;
                        fi.name  = jstr(fo, "n");
                        fi.type  = jstr(fo, "t");
                        fi.value = jstr(fo, "v");
                        in.fields.push_back(std::move(fi));
                    }
                    c->instances.push_back(std::move(in));
                }
                c->sel_inst = -1;
            }
        } else if (ty == "callResult") {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->call_result = jhas(j, "ok") && jnum(j, "ok") ?
                    jstr(j, "result") :
                    ("ERR: " + jstr(j, "error"));
            }
        } else if (ty == "trackerEvent") {
            std::string n = jstr(j, "name");
            int ti = find_tracker(n);
            if (ti >= 0) {
                TrackRow r;
                r.callers = jarr(j, "callers");
                r.ret  = jstr(j, "ret");
                r.args = jstr(j, "args");
                r.err  = jstr(j, "err");
                Tracker& t = s_trackers[ti];
                t.log.push_back(std::move(r));
                if (t.log.size() > 200) t.log.erase(t.log.begin());
            }
        } else if (ty == "recBatch") {
            for (auto& it : jarr(j, "items")) {
                if (s_rec.size() >= 600) s_rec.erase(s_rec.begin(),
                                                     s_rec.begin() + 100);
                s_rec.push_back(parse_rec(it));
            }
        } else if (ty == "recorderProgress") {
            s_rec_installed = (int)jnum(j, "installed");
            s_rec_total     = (int)jnum(j, "total");
            snprintf(s_rec_stat, sizeof(s_rec_stat), "installing %ld/%ld",
                     (long)s_rec_installed, (long)s_rec_total);
        } else if (ty == "recorderStarted") {
            snprintf(s_rec_stat, sizeof(s_rec_stat), "live (%ld hooks)",
                     (long)jnum(j, "installed"));
        } else if (ty == "recorderStopped") {
            snprintf(s_rec_stat, sizeof(s_rec_stat), "stopped");
        } else if (ty == "monitorEvent") {
            for (auto& it : jarr(j, "items")) {
                if (s_mon.size() >= 400) s_mon.erase(s_mon.begin(),
                                                     s_mon.begin() + 100);
                s_mon.push_back(parse_mon(it));
            }
        } else if (ty == "monitorInfo" || ty == "sslInit") {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "%s",
                     jstr(j, "note").c_str());
        } else if (ty == "monitorStarted") {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "on");
        } else if (ty == "monitorStopped") {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "off");
        } else if (ty == "console") {
            push_console("[js]", jstr(j, "line"));
        } else if (ty == "scriptResult") {
            push_console("=>", jstr(j, "result"));
        } else if (ty == "scriptError") {
            push_console("ERR", jstr(j, "msg"));
        } else if (ty == "patchApplied") {
            std::string n = jstr(j, "name");
            for (auto& p : s_patches)
                if (p.cls + "." + p.field == n) p.note = "";
        } else if (ty == "patchFailed") {
            std::string n = jstr(j, "name");
            for (auto& p : s_patches)
                if (p.cls + "." + p.field == n) p.note = jstr(j, "error");
        } else if (ty == "hookAdded") {
            std::string n = jstr(j, "name");
            for (auto& h : s_hooks)
                if (h.cls + "." + h.method == n) h.note = "";
        } else if (ty == "hookFailed") {
            std::string n = jstr(j, "name");
            for (auto& h : s_hooks)
                if (h.cls + "." + h.method == n) h.note = jstr(j, "error");
        } else if (ty == "smaliReady") {
            char b[200];
            snprintf(b, sizeof(b), "smali %s: %s",
                     jnum(j, "ok") ? "OK" : "FAILED",
                     jstr(j, "error").c_str());
            push_console("[smali]", b);
            if (!jnum(j, "ok")) {
                std::string n = jstr(j, "name");
                for (auto& h : s_hooks)
                    if (h.cls + "." + h.method == n) h.note = jstr(j, "error");
            }
        } else if (ty == "agentStatus") {
            /* r23: the native bridge posts this when the GumJS script
               itself failed to start - it used to only land in the
               crash log while the menu kept saying "waiting" forever. */
            if (!jnum(j, "ok")) {
                s_agent_ready = false;
                snprintf(s_agent_note, sizeof(s_agent_note), "FAILED: %.120s",
                         jstr(j, "msg").c_str());
                push_console("[agent]", std::string("start failed: ") +
                             jstr(j, "msg").c_str());
            }
        } else if (ty == "warn") {
            push_console("[warn]", jstr(j, "msg"));
        } else if (ty == "error") {
            push_console("[agent-err]", jstr(j, "msg"));
        } else if (ty == "info") {
            push_console("[info]", jstr(j, "msg"));
        }
        MULK();
    });
}

/* ===================== Settings ===================== */
static void tab_settings() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::SliderFloat("Font Size", &s_font_size, 12.f, 32.f, "%.0f px")) {
        /* apply on release: rebuilding the atlas mid-drag every frame
           would stutter */
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        overlay_request_font_rebuild(s_font_size);
        s_conf_dirty = true;
    }
    if (ImGui::SliderFloat("Opacity", &s_opacity, .3f, 1.f))
        s_conf_dirty = true;
    const char* th[] = {"Dark", "Light", "Classic"};
    if (ImGui::Combo("Theme", &s_theme, th, 3))
        s_conf_dirty = true;
    ImGui::Separator();
    if (ImGui::SliderFloat("Overlay Scroll Speed", &s_ovl_scroll, .2f, 4.f))
        s_conf_dirty = true;
    if (ImGui::SliderFloat("List Scroll Speed",    &s_lst_scroll, .2f, 4.f))
        s_conf_dirty = true;
    /* r11: manual correction for the status-bar class of offsets. The
       measured/estimated view map already subtracts the bar; this knob
       is for hosts where neither can be derived. */
    if (ImGui::SliderFloat("Touch Y offset", &s_y_off, -80.f, 80.f, "%+.0f px")) {
        overlay_set_touch_bias(s_y_off);
        s_conf_dirty = true;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Text input: the phone keyboard (system IME) "
                        "opens when a field is focused.");
    if (ImGui::Checkbox("Show toggle button", &s_show_fab))
        s_conf_dirty = true;
    if (ImGui::Checkbox("Menu visible", &s_visible))
        s_conf_dirty = true;
    if (!s_visible && !s_show_fab)
        ImGui::TextColored(ImVec4(1, .6f, 0, 1),
            "Menu hidden with no toggle button - it was re-enabled.");
    if (!s_show_fab && !s_visible) { s_show_fab = true; s_conf_dirty = true; }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(.5f, .8f, 1.f, 1), "Status");
    s_ovl_ready = overlay_ready();
    float dw=0, dh=0;
    overlay_get_display_size(&dw, &dh);
    ImGui::Text("Overlay: %s (%.0fx%.0f)",
                s_ovl_ready ? "up" : "waiting",
                dw, dh);
    /* r23: failed agent starts are visible now (frida_bridge posts
       agentStatus) - it used to stay "waiting" forever */
    if (!s_agent_ready && s_agent_note[0] &&
        strncmp(s_agent_note, "FAILED", 6) == 0)
        ImGui::TextColored(ImVec4(1, .4f, .4f, 1),
                           "Agent:   %s", s_agent_note);
    else
        ImGui::Text("Agent:   %s %s",
                    s_agent_ready ? "ready" : "waiting",
                    s_agent_note);
    char mb[128];
    overlay_touch_map_desc(mb, sizeof(mb));
    ImGui::Text("Input:   %s", mb);
    ImGui::Text("Display: %.0fx%.0f  FPS %.0f", io.DisplaySize.x,
                io.DisplaySize.y, io.Framerate);
}

/* ===================== Patches ===================== */
static void tab_patches() {
    if (s_pedit >= 0 && s_pedit < (int)s_patches.size()) {
        PatchRow& e = s_patches[s_pedit];
        snprintf(s_pcls, sizeof(s_pcls), "%s", e.cls.c_str());
        snprintf(s_pfld, sizeof(s_pfld), "%s", e.field.c_str());
        snprintf(s_pval, sizeof(s_pval), "%s", e.value.c_str());
        s_pedit = -1;
    }
    ime_textfield("Class##pc",  s_pcls, sizeof(s_pcls));
    ime_textfield("Field##pf",  s_pfld, sizeof(s_pfld));
    ime_textfield("Value##pv",  s_pval, sizeof(s_pval));
    if (ImGui::Button("Add Patch") && s_pcls[0] && s_pfld[0]) {
        std::string a = "{\"class\":\"" + jesc(s_pcls)
                      + "\",\"field\":\"" + jesc(s_pfld)
                      + "\",\"value\":\"" + jesc(s_pval) + "\"}";
        post_rpc("addPatch", a);
        MLK();
        s_patches.push_back({s_pcls, s_pfld, s_pval, true, "..."});
        MULK();
        s_pcls[0] = s_pfld[0] = s_pval[0] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) s_pcls[0]=s_pfld[0]=s_pval[0]=0;
    ImGui::Separator();

    MLK();
    for (int i = 0; i < (int)s_patches.size(); i++) {
        ImGui::PushID(i);
        PatchRow& p = s_patches[i];
        if (ImGui::Checkbox("##pe", &p.enabled)) {
            char a[600];
            snprintf(a, sizeof(a),
                     "{\"class\":\"%s\",\"field\":\"%s\",\"enabled\":%s}",
                     jesc(p.cls).c_str(), jesc(p.field).c_str(),
                     p.enabled ? "true" : "false");
            post_rpc("setPatchEnabled", a);
            p.note = "...";
        }
        ImGui::SameLine();
        ImGui::Text("%s.%s = %s", p.cls.c_str(), p.field.c_str(),
                    p.value.c_str());
        if (!p.note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "[%s]", p.note.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit##pu")) { s_pedit = i; }
        ImGui::SameLine();
        if (ImGui::SmallButton("X##px")) {
            char a[600];
            snprintf(a, sizeof(a),
                     "{\"class\":\"%s\",\"field\":\"%s\"}",
                     jesc(p.cls).c_str(), jesc(p.field).c_str());
            post_rpc("removePatch", a);
            s_patches.erase(s_patches.begin() + i);
            ImGui::PopID(); MULK(); return;
        }
        ImGui::PopID();
    }
    MULK();
}

/* ===================== Hooks ===================== */
static void tab_hooks() {
    const char* modes[] = {"Java(JS)", "Smali", "Nop (auto)"};
    if (s_hedit >= 0 && s_hedit < (int)s_hooks.size()) {
        HookRow& e = s_hooks[s_hedit];
        snprintf(s_hcls,  sizeof(s_hcls), "%s", e.cls.c_str());
        snprintf(s_hmth,  sizeof(s_hmth), "%s", e.method.c_str());
        snprintf(s_hcode, sizeof(s_hcode), "%s", e.code.c_str());
        s_hmode = e.mode;
        s_hedit = -1;
    }
    ime_textfield("Class##hc",  s_hcls, sizeof(s_hcls));
    ime_textfield("Method##hm", s_hmth, sizeof(s_hmth));
    ImGui::Combo("Mode##hmd", &s_hmode, modes, 3);
    ime_multiline("Code##hcd", s_hcode, sizeof(s_hcode),
                              ImVec2(-1, 90));
    ImGui::TextDisabled("Java: fn(Java,send,console,thiz,args,orig). "
                        "Smali: body of static hook(Object thiz, Object[] args); "
                        "p0=thiz p1=args, must return an Object. "
                        "Nop(auto): skips the body and returns the "
                        "default for the method's return type.");
    /* r25: read-only SOURCE panel - the real method body, disassembled
       on-device by the bundled baksmali. Java decompilation is not
       bundled (a decompiler is heavy and would run inside the host
       process), so BOTH modes show the bytecode: in Smali mode it is
       the exact language the hook is written in; in Java mode it is
       the same logic the Frida hook wraps. Copy the body into the
       editor and adapt it. */
    if (ImGui::CollapsingHeader("Method source##hsrc")) {
        if (ImGui::SmallButton("Load source")) {
            if (s_hcls[0] && s_hmth[0]) {
                std::string a = "{\"class\":\"" + jesc(s_hcls)
                              + "\",\"method\":\"" + jesc(s_hmth) + "\"}";
                post_rpc("getMethodSrc", a);
                snprintf(s_hsrc_note, sizeof(s_hsrc_note),
                         "disassembling...");
                s_hsrc_have = false;
            } else {
                snprintf(s_hsrc_note, sizeof(s_hsrc_note),
                         "fill Class + Method first");
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s_hsrc_note);
        if (s_hsrc_have) {
            ImGui::BeginChild("hsrcbx", ImVec2(0, 170), true);
            ImGui::TextUnformatted(s_hsrc);
            /* r24 pattern kept: auto-scroll must run INSIDE the child */
            ImGui::EndChild();
            mark_scroll_item(s_lst_scroll);
            if (s_hmode == 1 && ImGui::SmallButton("Copy body -> editor")) {
                /* strip the .method header and .end method so the text
                   fits the smali hook skeleton (which adds its own
                   signature); .locals/labels come along */
                std::string src = s_hsrc;
                auto b = src.find('\n');
                if (b != std::string::npos) src = src.substr(b + 1);
                auto e = src.rfind(".end method");
                if (e != std::string::npos) src = src.substr(0, e);
                snprintf(s_hcode, sizeof(s_hcode), "%s", src.c_str());
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Add Hook") && s_hcls[0] && s_hmth[0]) {
        std::string a = "{\"class\":\"" + jesc(s_hcls)
                      + "\",\"method\":\"" + jesc(s_hmth)
                      + "\",\"mode\":\"" + modes[s_hmode]
                      + "\",\"code\":\"" + jesc(s_hcode) + "\"}";
        post_rpc("addHook", a);
        MLK();
        s_hooks.push_back({s_hcls, s_hmth, s_hmode, s_hcode, true, "..."});
        MULK();
        s_hcls[0] = s_hmth[0] = s_hcode[0] = 0;
    }
    ImGui::Separator();

    MLK();
    for (int i = 0; i < (int)s_hooks.size(); i++) {
        ImGui::PushID(i);
        HookRow& h = s_hooks[i];
        if (ImGui::Checkbox("##he", &h.enabled)) {
            char a[700];
            snprintf(a, sizeof(a),
                     "{\"class\":\"%s\",\"method\":\"%s\",\"enabled\":%s}",
                     jesc(h.cls).c_str(), jesc(h.method).c_str(),
                     h.enabled ? "true" : "false");
            post_rpc("setHookEnabled", a);
            h.note = "...";
        }
        ImGui::SameLine();
        ImGui::Text("[%s] %s.%s", modes[h.mode], h.cls.c_str(),
                    h.method.c_str());
        if (!h.note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "[%s]", h.note.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit##hu")) s_hedit = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("X##hx")) {
            char a[700];
            snprintf(a, sizeof(a),
                     "{\"class\":\"%s\",\"method\":\"%s\"}",
                     jesc(h.cls).c_str(), jesc(h.method).c_str());
            post_rpc("removeHook", a);
            s_hooks.erase(s_hooks.begin() + i);
            ImGui::PopID(); MULK(); return;
        }
        ImGui::PopID();
    }
    MULK();
}

/* ===================== Tracker ===================== */
static void tab_tracker() {
    ime_textfield("Class##tc",  s_tcls, sizeof(s_tcls));
    ime_textfield("Method##tm", s_tmth, sizeof(s_tmth));
    if (ImGui::Button("Track") && s_tcls[0] && s_tmth[0]) {
        std::string a = "{\"class\":\"" + jesc(s_tcls)
                      + "\",\"method\":\"" + jesc(s_tmth) + "\"}";
        post_rpc("addTracker", a);
        MLK();
        s_trackers.push_back({std::string(s_tcls) + "." + std::string(s_tmth),
                              false, {}});
        MULK();
        s_tcls[0] = s_tmth[0] = 0;
    }
    ImGui::Separator();
    MLK();
    for (int i = 0; i < (int)s_trackers.size(); i++) {
        ImGui::PushID(i);
        Tracker& t = s_trackers[i];
        if (ImGui::CollapsingHeader(t.name.c_str())) {
            ImGui::TextDisabled("%lu events", (unsigned long)t.log.size());
            ImGui::BeginChild(("tl" + t.name).c_str(), ImVec2(0, 200), true);
            for (auto& r : t.log) {
                /* every caller line is clickable -> opens that class */
                for (auto& c : r.callers) {
                    if (ImGui::SmallButton(("@ " + c).c_str()))
                        menu_open_class(cls_of_member(c));
                }
                ImGui::BulletText("args=%s ret=%s%s%s",
                                  r.args.c_str(), r.ret.c_str(),
                                  r.err.empty() ? "" : " ERR=",
                                  r.err.c_str());
                ImGui::Separator();
            }
            ImGui::EndChild();
            mark_scroll_item(s_lst_scroll);
            if (ImGui::SmallButton("Remove##tr")) {
                std::string a = "{\"name\":\"" + jesc(t.name) + "\"}";
                post_rpc("removeTracker", a);
                s_trackers.erase(s_trackers.begin() + i);
                ImGui::PopID(); MULK(); return;
            }
        }
        ImGui::PopID();
    }
    MULK();
}

/* ===================== Recorder ===================== */
static void tab_recorder() {
    MLK();
    if (!s_rec_on) {
        MULK();
        if (ImGui::Button("Start")) {
            s_rec_on = true;
            /* r11: scoped recorder. The Start button passes the package
               filter and the method cap; the agent enumerates classes
               of that package ONLY and stops installing once the cap is
               reached, instead of hooking every method of every class in
               the process (which could not finish in the 32-bit field
               run and looked like a crash). */
            std::string a = "{\"filter\":\"" + jesc(s_rec_filter)
                           + "\",\"cap\":" + std::to_string(s_rec_cap) + "}";
            post_rpc("startRecorder", a);
            snprintf(s_rec_stat, sizeof(s_rec_stat), "starting...");
            MLK();
            s_rec.clear();
            MULK();
        }
    } else {
        if (ImGui::Button("Stop")) {
            s_rec_on = false;
            post_rpc("stopRecorder", "{}");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "REC");
        MULK();
    }
    MLK();
    ime_textfield("Package filter", s_rec_filter, sizeof(s_rec_filter));
    ImGui::SameLine();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("empty = the app's own package only; "
                          "com.foo = that prefix; . = everything (huge!)");
    ImGui::SliderInt("Method cap", &s_rec_cap, 100, 6000, "%d");
    ImGui::SameLine();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("stop installing hooks after this many methods");
    ImGui::TextDisabled("status: %s | %lu rows (newest last)",
                        s_rec_stat, (unsigned long)s_rec.size());
    ImGui::BeginChild("recl", ImVec2(0, 260), true);
    ImGuiListClipper clip;
    clip.Begin((int)s_rec.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            RecRow& r = s_rec[i];
            ImGui::PushID(i);
            std::string ext = r.extra.empty() ?
                "" : (" [" + r.extra + "]");
            char lbl[512];
            snprintf(lbl, sizeof(lbl), "%s.%s(%s) %s%s",
                     r.cls.c_str(), r.method.c_str(), r.args.c_str(),
                     r.ret.c_str(), ext.c_str());
            if (ImGui::Selectable(lbl))
                menu_open_class(r.cls);          /* cross-link */
            ImGui::PopID();
        }
    }
    /* r24: auto-scroll must run INSIDE the child - after EndChild()
       the GetScrollY/SetScrollHereY pair operated on the PARENT
       window (the whole menu jumped instead of the list following
       the new rows) */
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}

/* ===================== Monitor ===================== */
static void tab_monitor() {
    MLK();
    if (!s_mon_on) {
        MULK();
        if (ImGui::Button("Start")) {
            s_mon_on = true;
            post_rpc("startMonitor", "{}");
            MLK();
            s_mon.clear();
            MULK();
        }
    } else {
        if (ImGui::Button("Stop")) {
            s_mon_on = false;
            post_rpc("stopMonitor", "{}");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "MON");
        MULK();
    }
    MLK();
    ImGui::TextDisabled("best-effort | %s | %lu rows",
                        s_mon_stat, (unsigned long)s_mon.size());
    ImGui::BeginChild("monl", ImVec2(0, 260), true);
    ImGuiListClipper clip;
    clip.Begin((int)s_mon.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            MonRow& m = s_mon[i];
            ImGui::PushID(i);
            std::string org = m.origin.empty() ?
                "" : ("  <- " + m.origin);
            char lbl[512];
            snprintf(lbl, sizeof(lbl), "[%s] %s %s%s",
                     m.kind.c_str(), m.method.c_str(), m.url.c_str(),
                     org.c_str());
            /* r25: a click opens the DETAIL window (payload + origin
               method); the cross-link moved into that window */
            if (ImGui::Selectable(lbl)) s_mon_sel = i;
            if (!m.note.empty())
                ImGui::TextDisabled("    %s", m.note.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    /* r25: detail window - more info about the clicked log row:
       kind, HTTP method, URL, the app method that sent/received it
       (clickable), note and the payload preview. */
    if (s_mon_sel >= 0) {
        if (s_mon_sel >= (int)s_mon.size()) s_mon_sel = -1;
        else ImGui::OpenPopup("monDetail");
    }
    if (ImGui::BeginPopupModal("monDetail", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_mon_sel < 0 || s_mon_sel >= (int)s_mon.size()) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        } else {
            MonRow& m = s_mon[s_mon_sel];
            ImGui::Text("type:    %s", m.kind.c_str());
            ImGui::Text("http:    %s", m.method.c_str());
            ImGui::Text("url:     %s", m.url.c_str());
            ImGui::Text("origin:  %s",
                        m.origin.empty() ? "(unknown)" : m.origin.c_str());
            if (!m.origin.empty() && ImGui::SmallButton("Open class")) {
                menu_open_class(cls_of_member(m.origin));
            }
            if (!m.note.empty())
                ImGui::Text("note:    %s", m.note.c_str());
            if (!m.detail.empty()) {
                ImGui::TextDisabled("payload preview:");
                ImGui::BeginChild("mondet", ImVec2(460, 200), true);
                ImGui::TextWrapped("%s", m.detail.c_str());
                ImGui::EndChild();
                mark_scroll_item(s_lst_scroll);
            } else {
                ImGui::TextDisabled("(no payload captured)");
            }
            if (ImGui::Button("Close##mond")) {
                s_mon_sel = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    MULK();
}

/* ===================== Script ===================== */
static void tab_script() {
    ime_multiline("##sc", s_script, sizeof(s_script),
                              ImVec2(-1, 160));
    if (ImGui::Button("Run")) {
        MLK(); s_cons.clear(); MULK();
        std::string a = "{\"src\":\"" + jesc(s_script) + "\"}";
        post_rpc("evalScript", a);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear console")) { MLK(); s_cons.clear(); MULK(); }
    ImGui::Separator();
    MLK();
    ImGui::BeginChild("consl", ImVec2(0, 200), true);
    for (auto& l : s_cons) ImGui::TextUnformatted(l.c_str());
    /* r24: auto-scroll inside the child (see the Recorder tab) */
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}

/* ===================== Classes ===================== */
static void class_tab_content(ClassTab& c) {
    const char* subs[] = {"Instances", "Fields", "Methods"};
    /* r13: unique tab-bar id - it used to be "cls", the same id as
       the outer class-tab bar of tab_classes(); the two bars shared
       one ImGuiTabBar state and the selection oscillated every frame
       (lists flashed and disappeared). */
    if (ImGui::BeginTabBar("clssub")) {
        for (int si = 0; si < 3; si++) {
            if (ImGui::BeginTabItem(subs[si], nullptr, 0)) {
                c.sub = si;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    if (!c.got_info)
        ImGui::TextDisabled(c.waiting ? "(waiting for agent...)"
                                      : "(no data - tap Refresh)");

    if (ImGui::Button("Refresh"))
        menu_open_class(c.cname);
    ImGui::Separator();

    if (c.sub == 0) {                        /* ---- Instances ---- */
        if (ImGui::Button("Find instances##fi")) {
            std::string a = "{\"class\":\"" + jesc(c.cname) + "\"}";
            post_rpc("listInstances", a);
        }
        if (c.instances.empty()) {
            ImGui::TextDisabled("no instances loaded - tap Find instances");
        }
        for (int i = 0; i < (int)c.instances.size(); i++) {
            ImGui::PushID(i);
            bool sel = (c.sel_inst == i);
            char lbl[256];
            snprintf(lbl, sizeof(lbl), "#%d %s", i,
                     c.instances[i].str.c_str());
            if (ImGui::Selectable(lbl, sel)) c.sel_inst = i;
            ImGui::PopID();
        }
        if (c.sel_inst >= 0 && c.sel_inst < (int)c.instances.size()) {
            InstInfo& in = c.instances[c.sel_inst];
            ImGui::Separator();
            ImGui::Text("fields of #%d:", c.sel_inst);
            /* edit buffers are (re)filled only when a different instance
               is picked, not every frame - otherwise the user's typing
               would be overwritten before it ever reaches the agent.
               r23: the key used to be the BARE index - picking instance
               0 of another class kept the stale text of the previous
               class (nv_for still matched). The key is class + list
               generation + index now. */
            static char        nv[32][128];
            static std::string nv_key;
            static int         nv_idx = -1;
            {
                std::string key = c.cname + "#" + std::to_string(c.gen);
                if (key != nv_key || nv_idx != c.sel_inst) {
                    nv_key = key;
                    nv_idx = c.sel_inst;
                    for (int f = 0; f < 32; f++) {
                        nv[f][0] = 0;
                        if (f < (int)in.fields.size())
                            snprintf(nv[f], sizeof(nv[f]), "%s",
                                     in.fields[f].value.c_str());
                    }
                }
            }
            for (int f = 0; f < (int)in.fields.size() && f < 32; f++) {
                FieldInfo& fi = in.fields[f];
                ImGui::PushID(1000 + f);
                ImGui::Text("%s (%s)", fi.name.c_str(), fi.type.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Track##ft")) {
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"field\":\"" + jesc(fi.name) + "\"}";
                    post_rpc("trackField", a);
                    MLK();
                    s_trackers.push_back({c.cname + "." + fi.name,
                                          true, {}});
                    MULK();
                }
                ImGui::Text("  value:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ime_textfield("##fv", nv[f], sizeof(nv[f]));   /* r15: IME-aware wrapper - raw InputText never opened the keyboard */
                ImGui::SameLine();
                if (ImGui::Button("Set##fs")) {
                    /* r23: address the instance by the agent's retained
                       handle when one exists - the index-only request
                       had NO receiver, so f.set() failed on every
                       instance field (same fix as callMethod below) */
                    long h = in.handle;
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"handle\":" + std::to_string(h)
                                  + ",\"index\":" + std::to_string(c.sel_inst)
                                  + ",\"field\":\"" + jesc(fi.name)
                                  + "\",\"value\":\"" + jesc(nv[f]) + "\"}";
                    post_rpc("setInstanceField", a);
                }
                ImGui::PopID();
            }
        }
    } else if (c.sub == 2) {                 /* ---- Methods ---- */
        if (c.call_open) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "call %s.%s",
                               c.cname.c_str(), c.call_sig.name.c_str());
            int na = 0;
            {   /* one input per parameter, parsed from the signature */
                std::vector<std::string> ps;
                std::string p = c.call_sig.params, cur;
                int depth = 0;
                for (char ch : p) {
                    if (ch == ',' && depth == 0) { ps.push_back(cur); cur.clear(); continue; }
                    cur += ch;
                }
                ps.push_back(cur);
                if (ps.size() == 1 && ps[0].empty()) ps.clear();
                for (int k = 0; k < (int)ps.size() && k < 8; k++) {
                    char lb[64];
                    snprintf(lb, sizeof(lb), "arg%d (%s)", k, ps[k].c_str());
                    ime_textfield(lb, c.call_args[k], sizeof(c.call_args[k]));   /* r15: IME-aware wrapper */
                    na++;
                }
            }
            if (ImGui::Button("Invoke")) {
                /* r23: instance methods need a RECEIVER - when an
                   instance is selected its retained handle rides along
                   (Java.retain-ed on the agent side), otherwise the
                   agent refuses a non-static call with a hint instead
                   of the old silent ov.apply(null) failure */
                long h = (c.sel_inst >= 0 &&
                          c.sel_inst < (int)c.instances.size())
                             ? c.instances[c.sel_inst].handle : -1;
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(c.call_sig.name)
                              + "\",\"handle\":" + std::to_string(h)
                              + ",\"args\":[";
                for (int k = 0; k < na; k++) {
                    if (k) a += ",";
                    a += "\"" + jesc(c.call_args[k]) + "\"";
                }
                a += "]}";
                post_rpc("callMethod", a);
                c.call_result = "...";
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) c.call_open = false;
            if (!c.call_result.empty()) {
                ImGui::TextWrapped("%s", c.call_result.c_str());
            }
            ImGui::Separator();
        }
        /* r25: method INPUT/OUTPUT patch editor - the agent probes
           one call (ioprobe), the captured values are shown, only
           the P-marked rows are patched from then on. */
        if (c.io_open) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "patch IO %s.%s",
                               c.cname.c_str(), c.io_sig.name.c_str());
            ImGui::TextDisabled("P = patch this item; only marked "
                                "items are changed. Values refresh "
                                "on the next probed call.");
            for (int k = 0; k < c.io_nargs && k < 8; k++) {
                ImGui::PushID(3000 + k);
                ImGui::Checkbox("##ioe", &c.io_arg_on[k]);
                ImGui::SameLine();
                ImGui::Text("arg%d (%s) = %s", k, c.io_arg_typ[k],
                            c.io_arg_cur[k]);
                ImGui::SetNextItemWidth(220);
                ime_textfield("##iov", c.io_arg[k], sizeof(c.io_arg[k]));
                ImGui::PopID();
            }
            ImGui::Checkbox("##ioeR", &c.io_ret_on);
            ImGui::SameLine();
            ImGui::Text("ret (%s) = %s", c.io_ret_typ, c.io_ret_cur);
            ImGui::SetNextItemWidth(220);
            ime_textfield("##iovR", c.io_ret, sizeof(c.io_ret));
            if (ImGui::Button("Apply patch")) {
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(c.io_sig.name)
                              + "\",\"args\":[";
                int n = 0;
                for (int k = 0; k < c.io_nargs && k < 8; k++) {
                    if (!c.io_arg_on[k]) continue;
                    if (n++) a += ",";
                    a += "{\"i\":" + std::to_string(k)
                       + ",\"v\":\"" + jesc(c.io_arg[k]) + "\"}";
                }
                a += "],\"ret\":{";
                a += std::string("\"on\":") + (c.io_ret_on ? "true" : "false");
                a += ",\"v\":\"" + jesc(c.io_ret) + "\"}}";
                post_rpc("setMethodIO", a);
                c.io_result = "...";
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop patching")) {
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(c.io_sig.name)
                              + "\",\"args\":[],\"ret\":{\"on\":false,\"v\":\"\"}}";
                post_rpc("setMethodIO", a);
                c.io_result = "patching stopped";
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh values")) {
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(c.io_sig.name) + "\"}";
                post_rpc("getMethodIO", a);
                c.io_wait = true;
                c.io_result = "waiting for the next call...";
            }
            ImGui::SameLine();
            if (ImGui::Button("Close##ioc")) c.io_open = false;
            if (!c.io_result.empty())
                ImGui::TextWrapped("%s", c.io_result.c_str());
            ImGui::Separator();
        }
        ImGui::BeginChild("clsm", ImVec2(0, 0), true);
        ImGuiListClipper clip;
        clip.Begin((int)c.methods.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                MethodSig& m = c.methods[i];
                ImGui::PushID(i);
                /* r25: code-text hits are painted green (field spec:
                   "methods or fields that have this string should be
                   marked in green") */
                bool hitm = c.hit(c.hits_m, m.name);
                if (hitm) ImGui::PushStyleColor(ImGuiCol_Text,
                           ImVec4(0.25f, 0.95f, 0.35f, 1.f));
                ImGui::BulletText("%s(%s) -> %s",
                                  m.name.c_str(), m.params.c_str(),
                                  m.ret.c_str());
                if (hitm) ImGui::PopStyleColor();
                if (ImGui::SmallButton("Call##mc")) {
                    c.call_open  = true;
                    c.call_sig   = m;
                    c.call_result.clear();
                    for (int k = 0; k < 8; k++) c.call_args[k][0] = 0;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Track##mt")) {
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"method\":\"" + jesc(m.name) + "\"}";
                    post_rpc("addTracker", a);
                    MLK();
                    s_trackers.push_back({c.cname + "." + m.name, false, {}});
                    MULK();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Hook##mh")) {
                    /* prefill the Hooks form and jump there */
                    snprintf(s_hcls, sizeof(s_hcls), "%s", c.cname.c_str());
                    snprintf(s_hmth, sizeof(s_hmth), "%s", m.name.c_str());
                    s_hcode[0] = 0;
                    s_next_tab = "Hooks";
                }
                ImGui::SameLine();
                /* r25: one-tap Nop - auto return value by the method's
                   return type (void -> return, numeric -> 0, boolean
                   -> false, object/array -> null), no code needed */
                if (ImGui::SmallButton("Nop##mn")) {
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"method\":\"" + jesc(m.name)
                                  + "\",\"mode\":\"noop\",\"code\":\"\"}";
                    post_rpc("addHook", a);
                    MLK();
                    s_hooks.push_back({c.cname, m.name, 2, "", true, "..."});
                    MULK();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Src##ms")) {
                    /* load the real method body (baksmali) and jump to
                       the Hooks tab source panel */
                    snprintf(s_hcls, sizeof(s_hcls), "%s", c.cname.c_str());
                    snprintf(s_hmth, sizeof(s_hmth), "%s", m.name.c_str());
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"method\":\"" + jesc(m.name) + "\"}";
                    post_rpc("getMethodSrc", a);
                    snprintf(s_hsrc_note, sizeof(s_hsrc_note),
                             "disassembling...");
                    s_hsrc_have = false;
                    s_next_tab = "Hooks";
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("IO##mi")) {
                    /* probe one call and open the IO patch editor */
                    c.io_sig   = m;
                    c.io_wait  = true;
                    c.io_open  = true;
                    c.io_nargs = 0;
                    c.io_result = "waiting for the next call...";
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"method\":\"" + jesc(m.name) + "\"}";
                    post_rpc("getMethodIO", a);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
    } else if (c.sub == 1) {                 /* ---- Fields ---- */
        MLK();
        /* r12: values are NOT read while opening the class anymore
           (reading a static can run the class's <clinit> and hang
           the agent - the "open a class and the app freezes" bug).
           This button reads them on demand, for this class only. */
        if (ImGui::SmallButton("Read static values"))
            post_rpc("readStaticFields",
                     "{\"class\":\"" + jesc(c.cname) + "\"}");
        ImGui::SameLine();
        ImGui::TextDisabled("%lu fields", (unsigned long)c.fields.size());
        for (int i = 0; i < (int)c.fields.size(); i++) {
            FieldInfo& f = c.fields[i];
            ImGui::PushID(i);
            /* r25: code-text hits are painted green */
            bool hitf = c.hit(c.hits_f, f.name);
            if (hitf) ImGui::PushStyleColor(ImGuiCol_Text,
                       ImVec4(0.25f, 0.95f, 0.35f, 1.f));
            ImGui::BulletText("%s : %s = %s", f.name.c_str(),
                              f.type.c_str(), f.value.c_str());
            if (hitf) ImGui::PopStyleColor();
            if (ImGui::SmallButton("Patch##fp")) {
                snprintf(s_pcls, sizeof(s_pcls), "%s", c.cname.c_str());
                snprintf(s_pfld, sizeof(s_pfld), "%s", f.name.c_str());
                snprintf(s_pval, sizeof(s_pval), "%s", f.value.c_str());
                s_next_tab = "Patches";
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Track##ftf")) {
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"field\":\"" + jesc(f.name) + "\"}";
                post_rpc("trackField", a);
                s_trackers.push_back({c.cname + "." + f.name, true, {}});
            }
            ImGui::PopID();
        }
        MULK();
    }
}

static void tab_classes() {
    /* r12: the full class list loads BY DEFAULT - no "+ search" gate.
       The agent streams every loaded class in pages of 400; the
       filter row just re-queries with a substring. */
    MLK();
    bool agent_ready = s_agent_ready;
    bool need_auto   = !s_cls_auto_done && !s_search_wait &&
                       s_search_res.empty();
    int  qid         = 0;
    if (agent_ready && need_auto) {
        s_cls_auto_done = true;
        s_search_wait   = true;
        qid = ++s_search_seq;
        s_search_qid   = qid;      /* r23: generation armed at SEND time */
        s_search_page0 = false;
    }
    MULK();
    /* r25: the AUTO default list is always a plain, sorted class
       listing (mode 0) - a user who left the mode on Code text
       would otherwise trigger a full dex scan just by opening the
       tab. The System gate still applies. */
    if (qid)
        post_rpc("listLoadedClasses",
                 "{\"filter\":\"\",\"qid\":" + std::to_string(qid)
                 + ",\"mode\":0"
                 + ",\"sys\":" + (s_search_sys ? "true" : "false") + "}");

    bool do_search = false;
    MLK();
    /* r25: 4 search modes + the system-class gate. Mode 3 (code
       text) runs the dexlib2/baksmali scan on the agent; modes 0-2
       are name matches. Search fires on Enter / button only. */
    const char* smodes[] = {"Class name", "Method name",
                            "Field name", "Code text"};
    ImGui::SetNextItemWidth(150.f);
    if (ImGui::Combo("Mode##smode", &s_search_mode, smodes, 4))
        do_search = !s_search_wait;      /* re-run with the new mode */
    ImGui::SameLine();
    if (ImGui::Checkbox("System", &s_search_sys))
        do_search = !s_search_wait;      /* re-run with the new gate */
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("show android/androidx/com.android/com.google/"
                          "java/javax/dalvik/kotlin classes too (off by "
                          "default - the field spec asked them hidden)");
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 150.f);
    /* r23: search on Enter / button ONLY - every keystroke used to
       re-run enumerateLoadedClasses on the agent (30k+ names per
       letter, flooding the bridge while typing). */
    do_search |= ime_textfield("Filter##sf", s_search, sizeof(s_search),
                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Search##cls")) do_search = true;
    ImGui::SameLine();
    if (s_search_wait) ImGui::TextDisabled("(listing...)");
    else ImGui::TextDisabled("%ld of %ld loaded classes",
                             (long)s_search_shown, (long)s_search_total);
    ImGui::BeginChild("srch", ImVec2(0, 220), true);
    ImGuiListClipper clip;
    clip.Begin((int)s_search_res.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(s_search_res[i].c_str()))
                menu_open_class(s_search_res[i]);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
    if (do_search) {
        MLK();
        s_search_wait = true;
        int sqid = ++s_search_seq;
        s_search_qid   = sqid;     /* r23: generation armed at SEND time */
        s_search_page0 = false;
        MULK();
        std::string a = "{\"filter\":\"" + jesc(s_search)
                       + "\",\"qid\":" + std::to_string(sqid)
                       + ",\"mode\":" + std::to_string(s_search_mode)
                       + ",\"sys\":" + (s_search_sys ? "true" : "false") + "}";
        post_rpc("listLoadedClasses", a);
    }
    ImGui::Separator();

    MLK();
    if (!s_classes.empty()) {
        /* which open class is visible? ImGui has no query for "current
           tab", so remember the last one the user clicked */
        static int s_cls_cur = 0;
        if (ImGui::BeginTabBar("clsTabs")) {
            for (int i = 0; i < (int)s_classes.size(); i++) {
                bool open = true;
                ImGuiTabItemFlags fl = 0;
                if (s_focus_cls == s_classes[i].cname)
                    fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(s_classes[i].cname.c_str(), &open,
                                        fl)) {
                    s_cls_cur = i;
                    if (s_focus_cls == s_classes[i].cname)
                        s_focus_cls.clear();
                    ImGui::EndTabItem();
                }
                if (!open) {
                    s_classes.erase(s_classes.begin() + i);
                    if (s_cls_cur >= (int)s_classes.size())
                        s_cls_cur = (int)s_classes.size() - 1;
                    ImGui::EndTabBar();
                    MULK();
                    return;
                }
            }
            ImGui::EndTabBar();
        }
        int cur = s_cls_cur;
        /* r12 deadlock fix: the r11 code locked g_mtx a SECOND time
           right here (and class_tab_content locks again inside) -
           with the plain mutex that self-deadlocked the render
           thread on the first tap on a class. g_mtx is recursive
           now and this stray lock is gone. */
        if (cur >= 0 && cur < (int)s_classes.size())
            class_tab_content(s_classes[cur]);
        MULK();
    } else {
        ImGui::TextDisabled("no class tabs - tap a class above");
        MULK();
    }
}


/* ===================== floating toggle button =====================
   Always available (unless disabled in Settings), draggable, and a
   plain tap toggles the menu - r7 only showed it while the menu was
   hidden and it could not be moved. */
static ImVec2 s_fab_pos(-1, -1);
static bool    s_fab_drag = false;
static bool    s_fab_moved = false;
static ImVec2  s_fab_grab(0, 0), s_fab_orig(0, 0);

static void draw_fab(ImVec2 ds) {
    if (!s_show_fab) return;
    if (s_fab_pos.x < 0) s_fab_pos = ImVec2(ds.x - 90.f, ds.y - 110.f);
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(s_fab_pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.f);
    ImGui::PushStyleColor(ImGuiCol_Button,
        s_visible ? ImVec4(.25f, .55f, .25f, .85f)
                  : ImVec4(.55f, .30f, .55f, .85f));
    ImGui::Begin("##fab", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    if (ImGui::Button(s_visible ? "HIDE" : "MENU", ImVec2(64, 44))) {
        /* click handled on release below for taps; drags reposition */
    }
    if (ImGui::IsItemActive()) {
        if (!s_fab_drag) {
            s_fab_drag  = true;
            s_fab_moved = false;
            s_fab_grab  = io.MousePos;
            s_fab_orig  = s_fab_pos;
        }
        ImVec2 d(io.MousePos.x - s_fab_grab.x, io.MousePos.y - s_fab_grab.y);
        if (fabsf(d.x) > 8.f || fabsf(d.y) > 8.f) s_fab_moved = true;
        if (s_fab_moved) {
            s_fab_pos = ImVec2(s_fab_orig.x + d.x, s_fab_orig.y + d.y);
            if (s_fab_pos.x < 0)  s_fab_pos.x = 0;
            if (s_fab_pos.y < 0)  s_fab_pos.y = 0;
            if (s_fab_pos.x > ds.x - 80) s_fab_pos.x = ds.x - 80;
            if (s_fab_pos.y > ds.y - 60) s_fab_pos.y = ds.y - 60;
            ImGui::SetWindowPos(s_fab_pos);
        }
    } else if (s_fab_drag) {
        s_fab_drag = false;
        if (!s_fab_moved) {           /* it was a tap, not a drag */
            s_visible = !s_visible;
            s_conf_dirty = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/* ===================== frame pump (r25) =====================
   0/1/2 level for dextool_want_frames(); the agent postInvalidate()s
   the host DecorView at this rate so render-on-demand hosts keep
   producing frames while the menu is up. Without it the menu only
   advanced when the host happened to swap ("menu hangs, lags and
   freezes on other apps ... when I touch the app, the touch I had
   previously made on the menu works"). */
int menu_frame_pump_level() {
    MLK();
    bool vis = s_visible;
    MULK();
    if (vis) return 2;                    /* full rate */
    return overlay_ready() ? 1 : 0;       /* FAB-only slow pump  */
}

/* ===================== main window ===================== */
void menu_draw() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    apply_theme();

    MLK();
    bool vis = s_visible;
    MULK();

    if (!vis) {
        draw_fab(ds);
        MLK();
        bool ime_was    = s_ime_active;
        s_ime_active    = false;
        bool clear_req  = s_ime_clear;
        s_ime_clear     = false;
        MULK();
        if (ime_was) ime_hide();  /* menu hidden: close the phone kb */
        if (clear_req) ImGui::ClearActiveID();
        /* publish empty region list: drags elsewhere must not scroll */
        overlay_publish_scroll_regions(s_regions);
        s_regions.clear();
        return;
    }
    draw_fab(ds);
    ime_frame_prelude();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(420, 300), ImVec2(ds.x * .95f, ds.y * .95f));
    ImGui::SetNextWindowSize(ImVec2(ds.x * .8f, ds.y * .85f), ImGuiCond_Once);
    /* r24: the r23 "style.Alpha is the single source" call was wrong -
       style.Alpha faded EVERYTHING (text included) while the spec
       wants only the window BACKGROUND transparent. apply_theme() no
       longer touches style.Alpha; SetNextWindowBgAlpha(s_opacity)
       here is the single opacity source. */
    ImGui::SetNextWindowBgAlpha(s_opacity);
    bool open = vis;
    ImGui::Begin("Dex-tool v1.2 (r25)", &open,
                 ImGuiWindowFlags_NoCollapse);
    if (!open) {
        MLK();
        s_visible = false;
        s_conf_dirty = true;
        /* r23 UI polish: pressing the window X with the toggle button
           disabled stranded the menu (nothing could bring it back) -
           the FAB force-returns, same policy as the Settings rescue */
        if (!s_show_fab) s_show_fab = true;
        MULK();
    }

    if (ImGui::BeginTabBar("tb")) {
        struct TabDef { const char* name; void (*fn)(); };
        TabDef tabs[] = {
            {"Settings", tab_settings}, {"Patches", tab_patches},
            {"Hooks",    tab_hooks},    {"Tracker", tab_tracker},
            {"Recorder", tab_recorder}, {"Monitor", tab_monitor},
            {"Script",   tab_script},   {"Classes", tab_classes},
        };
        MLK();
        std::string want = s_next_tab;
        s_next_tab.clear();
        MULK();
        for (auto& t : tabs) {
            ImGuiTabItemFlags fl = 0;
            if (want == t.name) fl |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(t.name, nullptr, fl)) {
                t.fn();          /* the body was missing: tabs drew empty */
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::End();
    /* the whole window body pans at the overlay speed */
    mark_rect(wp.x, wp.y, ws.x, ws.y, s_ovl_scroll);
    overlay_publish_scroll_regions(s_regions);
    s_regions.clear();

    /* debounce the conf write so sliders do not hit the disk 60x/s */
    if (s_conf_dirty && s_conf_path[0] &&
        conf_now_ms() - s_conf_last_ms > 700) {
        conf_save();
        MLK();
        overlay_set_touch_bias(s_y_off);
        MULK();
    }
}
