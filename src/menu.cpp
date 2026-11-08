#include "menu.h"
#include "overlay.h"
#include "frida_bridge.h"
#include "logger.h"
#include "imgui.h"
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
   Dex-tool menu (r11)
   - every tab of the spec: Settings, Patches, Hooks, Tracker, Recorder,
     Monitor, Script, Classes
   - cross-linking: clicking a row in Tracker / Recorder / Monitor opens
     that class in the Classes tab (menu_open_class)
   - the recorder/monitor lists are fed by the agent in throttled
     batches; the r11 recorder is scoped (package filter + method cap),
     which is what stops the hook-everything install from exhausting
     the 32-bit process
   - scrollable areas are published to the overlay, which converts
     drags inside them into wheel events at the configured speed
   - fully native on-screen keyboard (r11): no system IME, no hidden
     EditText, no text polling - the r10 bridge stringified an
     android.text.Editable wrapper and typed the literal
     "[object Object]" into every field
   - UI state persists to <logdir>/dextool-ui.conf
   ===================================================================== */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
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
static float s_y_off      = 0.0f;   /* manual touch Y bias (px) */
static bool  s_kbd_on     = true;   /* on-screen keyboard enabled */

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
struct InstInfo  { std::string str; std::vector<FieldInfo> fields; };
struct ClassTab {
    std::string cname;
    bool got_info = false, waiting = false;
    std::vector<MethodSig> methods;
    std::vector<FieldInfo> fields;
    std::vector<InstInfo>  instances;
    int  sel_inst = -1;
    int  sub = 0;                       /* 0 Instances 1 Methods 2 Fields */
    /* inline method-call editor */
    bool call_open = false;
    MethodSig call_sig;
    char call_args[8][128] = {};
    std::string call_result;
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
struct MonRow   { std::string kind, url, method, origin, note; };

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
static bool s_search_on=false;
static std::vector<std::string> s_search_res;
static int  s_search_total=0, s_search_shown=0;
static bool s_search_wait=false;
static int  s_search_seq=0, s_search_qid=-1;
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
    return j.find(std::string("\"")+key+"\"") != std::string::npos;
}

static void post_rpc(const char* fn, const std::string& args) {
    std::string m = "{\"type\":\"rpc\",\"fn\":\"";
    m += fn; m += "\",\"args\":"; m += args; m += "}";
    frida_post(m);
}

static void push_console(const char* tag, const std::string& line) {
    char b[256];
    snprintf(b, sizeof(b), "%s %s", tag, line.c_str());
    if (s_cons.size() < 400) s_cons.push_back(b);
}

/* ---------------- cross-linking ---------------- */
void menu_open_class(const std::string& name) {
    if (name.empty()) return;
    MLK();
    bool have = false;
    for (auto& c : s_classes) if (c.cname == name) { have = true; break; }
    if (!have) s_classes.push_back(ClassTab{name});
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
/* "kind|url|method|origin|note" */
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
    m.note = s.substr(p4+1);
    return m;
}
/* instance string "toString##name=value;name=value" */
static InstInfo parse_inst(const std::string& s) {
    InstInfo i;
    auto h = s.find("##");
    if (h == std::string::npos) { i.str = s; return i; }
    i.str = s.substr(0, h);
    std::string rest = s.substr(h + 2), cur;
    size_t st = 0;
    while (st < rest.size()) {
        auto e = rest.find(';', st);
        cur = rest.substr(st, (e == std::string::npos ? rest.size() : e) - st);
        if (!cur.empty()) i.fields.push_back(parse_field(cur));
        if (e == std::string::npos) break;
        st = e + 1;
    }
    return i;
}

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
        else if (!strcmp(k, "yoff"))   s_y_off    = v;
        else if (!strcmp(k, "kbd"))    s_kbd_on   = v > 0.5f;
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
    fprintf(f, "yoff=%g\n",   s_y_off);
    fprintf(f, "kbd=%d\n",   s_kbd_on ? 1 : 0);
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
    ImGuiStyle& st = ImGui::GetStyle();
    st.Alpha = s_opacity;
    ImGui::GetIO().FontGlobalScale = 1.0f;   /* real size comes from the
                                               atlas, not from scaling */
}

/* ===================== native on-screen keyboard =====================
   r11. The r10 bridge used a hidden EditText + the system IME and
   forwarded the text delta from the frida thread. String(EditText
   .getText()) is NOT the text - it is an android.text.Editable whose
   String() form is "[object Object]", so every field filled with the
   keyboard literally received that string (and the IME of a game host
   resizes/recreates the EGL context, feeding the abort class).
   The r11 keyboard is plain ImGui: while a kbd-enabled text field is
   active, menu_draw() lays it out at the bottom of the display and
   publishes its rect; menu_kbd_feed() (called by the overlay for every
   touch, BEFORE the event reaches ImGui) maps taps onto keys and turns
   them into InputText events. */
struct KbdState {
    bool  open = false;        /* visible and ready to take taps   */
    bool  focus = false;       /* a kbd-enabled field is active    */
    bool  shift = false;
    bool  page2 = false;       /* symbol page                      */
    bool  dragging = false;    /* the current gesture started on   */
    float x=0, y=0, w=0, h=0;  /* last drawn window rect           */
    std::string last;          /* label of the last key hit        */
} static s_kbd;

/* every key as actually laid out by draw_kbd this frame; feed()
   hit-tests against these, so the drawn geometry IS the hit geometry */
struct KbdKey { float x, y, w, h; int act; char ch; };
enum { KBD_CHAR=0, KBD_SHIFT, KBD_BKSP, KBD_ENTER, KBD_HIDE,
       KBD_SPACE, KBD_PAGE };
static std::vector<KbdKey> s_kbd_keys;

/* Which text field gets the keys? Every kbd-enabled InputText in this
   file carries a KbdTag { char* buf; size_t cap; } next to it; a helper
   draws the field and registers it while it is active. */
struct KbdField {
    char*  buf = nullptr;
    size_t cap = 0;
};
static KbdField s_kbd_field;      /* the active field, owner thread */
static bool     s_kbd_field_req = false;   /* want to grab focus now */

/* draw an InputText that grabs the on-screen keyboard when active */
static bool kbd_textfield(const char* label, char* buf, size_t cap) {
    bool used = ImGui::InputText(label, buf, cap,
                                 ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemActivated()) {
        /* user tapped the field: focus it and pop the keyboard */
        s_kbd_field = { buf, cap };
        s_kbd_field_req = true;
    }
    return used;
}

/* same for multiline editors (script editor, hook code) */
static bool kbd_multiline(const char* label, char* buf, size_t cap,
                          ImVec2 size) {
    bool used = ImGui::InputTextMultiline(label, buf, cap, size);
    if (ImGui::IsItemActivated()) {
        s_kbd_field = { buf, cap };
        s_kbd_field_req = true;
    }
    return used;
}

/* delete one character before the caret the way the user typed it
   (a UTF-16 surrogate pair is deleted together) */
static void kbd_del_char(char* buf) {
    size_t n = strlen(buf);
    if (!n) return;
    unsigned char prev = (unsigned char)buf[n - 1];
    if (n >= 3 && prev >= 0x80 && (unsigned char)buf[n-2] == 0xED &&
        (unsigned char)buf[n-3] == 0xED)
        n -= 3;                    /* broken pair, drop both halves */
    else if (n >= 2 && prev >= 0x80 && (prev & 0xC0) == 0x80 &&
             (unsigned char)buf[n-2] >= 0xC0)
        n -= 2;                    /* 2-byte sequence */
    else
        n -= 1;
    if (n < 0) n = 0;
    buf[n] = 0;
}

/* append a UTF-8 char if it fits */
static void kbd_put_char(char* buf, size_t cap, const char* u8, int len) {
    size_t n = strlen(buf);
    if (n + (size_t)len + 1 > cap - 1) return;
    memcpy(buf + n, u8, (size_t)len);
    buf[n + (size_t)len] = 0;
}

/* run one key press; used by both the tap path (menu_kbd_feed) and
   the mouse path (ImGui::Button returning true) */
static void kbd_apply(const KbdKey& k) {
    switch (k.act) {
    case KBD_CHAR: {
        char c = k.ch;
        if (!s_kbd.page2 && s_kbd.shift && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
            s_kbd.shift = false;          /* one-shot shift */
        }
        char u8[2] = { c, 0 };
        if (s_kbd_field.buf)
            kbd_put_char(s_kbd_field.buf, s_kbd_field.cap, u8, 1);
        s_kbd.last = std::string(1, c);
        break;
    }
    case KBD_SHIFT:
        s_kbd.shift = !s_kbd.shift; s_kbd.last = "shift"; break;
    case KBD_BKSP:
        if (s_kbd_field.buf) kbd_del_char(s_kbd_field.buf);
        s_kbd.last = "bksp"; break;
    case KBD_ENTER:
    case KBD_HIDE:
        s_kbd.open = false;
        s_kbd.last = (k.act == KBD_ENTER) ? "enter" : "hide"; break;
    case KBD_SPACE:
        if (s_kbd_field.buf)
            kbd_put_char(s_kbd_field.buf, s_kbd_field.cap, " ", 1);
        s_kbd.last = "space"; break;
    case KBD_PAGE:
        s_kbd.page2 = !s_kbd.page2; s_kbd.last = "page"; break;
    }
}

/* the r11 input path: every touch first passes through here */
bool menu_kbd_feed(int phase, float x, float y) {
    if (!s_kbd_on || !s_kbd.open) return false;
    if (phase == 0) {                       /* press */
        s_kbd.dragging = false;
        if (x >= s_kbd.x && x <= s_kbd.x + s_kbd.w &&
            y >= s_kbd.y && y <= s_kbd.y + s_kbd.h) {
            s_kbd.dragging = true;          /* gesture is ours */
            return true;
        }
        return false;                       /* tap outside: pass through */
    }
    if (!s_kbd.dragging) return false;      /* move/release of others */
    if (phase == 2) return true;            /* drag inside: swallow */
    /* release: map onto the drawn keys */
    s_kbd.dragging = false;
    for (const KbdKey& k : s_kbd_keys) {
        if (x >= k.x && x <= k.x + k.w &&
            y >= k.y && y <= k.y + k.h) {
            kbd_apply(k);
            return true;
        }
    }
    return true;                            /* between keys: swallow */
}

/* draw the keyboard at the bottom of the display; called from
   menu_draw() every frame while a field is focused. Layout constants:
   pitch 40 (34px keys + 6 spacing), 20px status line, 8px padding -
   content height 8+20+5*40+8 = 236. */
static void draw_kbd(ImVec2 ds) {
    if (s_kbd_field_req) {
        s_kbd_field_req = false;
        if (!s_kbd.open) { s_kbd.open = true; s_kbd.page2 = false; }
    }
    s_kbd.focus = s_kbd_field.buf != nullptr;
    if (!s_kbd.open) {
        overlay_publish_kbd_rect(0, 0, 0, 0);
        return;
    }
    s_kbd.w = ds.x;
    s_kbd.h = 236.f;
    s_kbd.x = 0;
    s_kbd.y = ds.y - s_kbd.h;
    if (s_kbd.y < 0) s_kbd.y = 0;
    s_kbd_keys.clear();

    ImGui::SetNextWindowPos(ImVec2(s_kbd.x, s_kbd.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(s_kbd.w, s_kbd.h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    ImGui::Begin("##kbd", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    char tag[96];
    snprintf(tag, sizeof(tag), "r11 kbd%s%s%s  %s",
             s_kbd.page2 ? " sym" : "", s_kbd.shift ? " SHIFT" : "",
             s_kbd_field.buf ? "" : " (no field!)",
             s_kbd.last.empty() ? "" : s_kbd.last.c_str());
    ImGui::TextDisabled("%s", tag);

    /* one helper draws a key AND records its rect for the tap path */
    #define KBD_KEY(LBL, W, ACT, CH) do {                               \
        if (ImGui::Button(LBL, ImVec2((W), 34))) {                       \
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(); \
            kbd_apply({ a.x, a.y, b.x - a.x, b.y - a.y, (ACT), (CH) }); \
        }                                                                \
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(); \
        s_kbd_keys.push_back({ a.x, a.y, b.x - a.x, b.y - a.y,           \
                               (ACT), (CH) });                           \
    } while (0)

    static const char* R0N = "qwertyuiop";
    static const char* R1N = "asdfghjkl";
    static const char* R2N = "zxcvbnm";
    static const char* R0S = "1234567890";
    static const char* R1S = "-/:;()$&\"@.,?!'";
    static const char* R2S = "#%\\*+=_~<>";
    const char* rows[3];
    int nkey[3];
    if (s_kbd.page2) {
        rows[0] = R0S; nkey[0] = 10;
        rows[1] = R1S; nkey[1] = 14;
        rows[2] = R2S; nkey[2] = 12;
    } else {
        rows[0] = R0N; nkey[0] = 10;
        rows[1] = R1N; nkey[1] = 9;
        rows[2] = R2N; nkey[2] = 7;
    }
    for (int r = 0; r < 3; r++) {
        float avail = ImGui::GetContentRegionAvail().x - 10.f;
        float bw = avail / (float)nkey[r] - 4.f;
        for (int k = 0; k < nkey[r]; k++) {
            char lb[8], c = rows[r][k];
            if (!s_kbd.page2 && s_kbd.shift && c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            snprintf(lb, sizeof(lb), "%c", c);
            KBD_KEY(lb, bw, KBD_CHAR, rows[r][k]);   /* base char, apply() upcases */
            if (k < nkey[r] - 1) ImGui::SameLine();
        }
    }
    {   /* modifiers row */
        float bw = ImGui::GetContentRegionAvail().x - 10.f;
        KBD_KEY(s_kbd.shift ? "SHIFT" : "shift", bw * 0.30f, KBD_SHIFT, 0);
        ImGui::SameLine();
        KBD_KEY("bksp",  bw * 0.40f, KBD_BKSP, 0);
        ImGui::SameLine();
        KBD_KEY("enter", bw * 0.29f, KBD_ENTER, 0);
    }
    {   /* bottom row */
        float bw = ImGui::GetContentRegionAvail().x - 10.f;
        KBD_KEY("hide",  bw * 0.22f, KBD_HIDE, 0);
        ImGui::SameLine();
        KBD_KEY("space", bw * 0.56f, KBD_SPACE, 0);
        ImGui::SameLine();
        KBD_KEY(s_kbd.page2 ? "abc" : "#+=", bw * 0.22f, KBD_PAGE, 0);
    }
    #undef KBD_KEY
    ImGui::End();
    ImGui::PopStyleVar(2);
    overlay_publish_kbd_rect(s_kbd.x, s_kbd.y, s_kbd.w, s_kbd.h);
}

/* ---------------- message callback (GumJS thread) ---------------- */
void menu_init() {
    apply_theme();
    conf_load();
    overlay_set_touch_bias(s_y_off);
    overlay_request_font_rebuild(s_font_size);
    frida_set_message_callback([](const std::string& j) {
        MLK();
        if (jhas(j, "ready")) {
            s_agent_ready = true;
            snprintf(s_agent_note, sizeof(s_agent_note), "%s",
                     jstr(j, "msg").c_str());
        } else if (jhas(j, "classInfo")) {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->got_info = true; c->waiting = false;
                c->methods.clear();
                for (auto& m : jarr(j, "methods"))
                    c->methods.push_back(parse_sig(m));
                c->fields.clear();
                for (auto& f : jarr(j, "fields"))
                    c->fields.push_back(parse_field(f));
            }
        } else if (jhas(j, "classList")) {
            /* r11: the agent streams the full result in pages of 400;
               the r10 single message was capped at 800 names, which is
               why "Classes" never displayed all classes */
            long qid  = jnum(j, "qid");
            long page = jnum(j, "page");
            if (qid != s_search_qid || page == 0) {
                s_search_res.clear();
                s_search_qid = (int)qid;
            }
            for (auto& c : jarr(j, "classes")) s_search_res.push_back(c);
            s_search_total = (int)jnum(j, "total");
            s_search_shown = (int)jnum(j, "shown");
            if (page + 1 >= jnum(j, "pages")) s_search_wait = false;
        } else if (jhas(j, "instanceList")) {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->instances.clear();
                for (auto& i : jarr(j, "instances"))
                    c->instances.push_back(parse_inst(i));
                c->sel_inst = -1;
            }
        } else if (jhas(j, "callResult")) {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->call_result = jhas(j, "ok") && jnum(j, "ok") ?
                    jstr(j, "result") :
                    ("ERR: " + jstr(j, "error"));
            }
        } else if (jhas(j, "trackerEvent")) {
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
        } else if (jhas(j, "recBatch")) {
            for (auto& it : jarr(j, "items")) {
                if (s_rec.size() >= 600) s_rec.erase(s_rec.begin(),
                                                     s_rec.begin() + 100);
                s_rec.push_back(parse_rec(it));
            }
        } else if (jhas(j, "recorderProgress")) {
            s_rec_installed = (int)jnum(j, "installed");
            s_rec_total     = (int)jnum(j, "total");
            snprintf(s_rec_stat, sizeof(s_rec_stat), "installing %ld/%ld",
                     (long)s_rec_installed, (long)s_rec_total);
        } else if (jhas(j, "recorderStarted")) {
            snprintf(s_rec_stat, sizeof(s_rec_stat), "live (%ld hooks)",
                     (long)jnum(j, "installed"));
        } else if (jhas(j, "recorderStopped")) {
            snprintf(s_rec_stat, sizeof(s_rec_stat), "stopped");
        } else if (jhas(j, "monitorEvent")) {
            for (auto& it : jarr(j, "items")) {
                if (s_mon.size() >= 400) s_mon.erase(s_mon.begin(),
                                                     s_mon.begin() + 100);
                s_mon.push_back(parse_mon(it));
            }
        } else if (jhas(j, "monitorInfo") || jhas(j, "sslInit")) {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "%s",
                     jstr(j, "note").c_str());
        } else if (jhas(j, "monitorStarted")) {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "on");
        } else if (jhas(j, "monitorStopped")) {
            snprintf(s_mon_stat, sizeof(s_mon_stat), "off");
        } else if (jhas(j, "console")) {
            push_console("[js]", jstr(j, "line"));
        } else if (jhas(j, "scriptResult")) {
            push_console("=>", jstr(j, "result"));
        } else if (jhas(j, "scriptError")) {
            push_console("ERR", jstr(j, "msg"));
        } else if (jhas(j, "patchApplied")) {
            std::string n = jstr(j, "name");
            for (auto& p : s_patches)
                if (p.cls + "." + p.field == n) p.note = "";
        } else if (jhas(j, "patchFailed")) {
            std::string n = jstr(j, "name");
            for (auto& p : s_patches)
                if (p.cls + "." + p.field == n) p.note = jstr(j, "error");
        } else if (jhas(j, "hookAdded")) {
            std::string n = jstr(j, "name");
            for (auto& h : s_hooks)
                if (h.cls + "." + h.method == n) h.note = "";
        } else if (jhas(j, "hookFailed")) {
            std::string n = jstr(j, "name");
            for (auto& h : s_hooks)
                if (h.cls + "." + h.method == n) h.note = jstr(j, "error");
        } else if (jhas(j, "smaliReady")) {
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
        } else if (jhas(j, "warn")) {
            push_console("[warn]", jstr(j, "msg"));
        } else if (jhas(j, "error")) {
            push_console("[agent-err]", jstr(j, "msg"));
        } else if (jhas(j, "info")) {
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
    if (ImGui::Checkbox("On-screen keyboard", &s_kbd_on))
        s_conf_dirty = true;
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
    kbd_textfield("Class##pc",  s_pcls, sizeof(s_pcls));
    kbd_textfield("Field##pf",  s_pfld, sizeof(s_pfld));
    kbd_textfield("Value##pv",  s_pval, sizeof(s_pval));
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
    const char* modes[] = {"Java(JS)", "Smali", "noop"};
    if (s_hedit >= 0 && s_hedit < (int)s_hooks.size()) {
        HookRow& e = s_hooks[s_hedit];
        snprintf(s_hcls,  sizeof(s_hcls), "%s", e.cls.c_str());
        snprintf(s_hmth,  sizeof(s_hmth), "%s", e.method.c_str());
        snprintf(s_hcode, sizeof(s_hcode), "%s", e.code.c_str());
        s_hmode = e.mode;
        s_hedit = -1;
    }
    kbd_textfield("Class##hc",  s_hcls, sizeof(s_hcls));
    kbd_textfield("Method##hm", s_hmth, sizeof(s_hmth));
    ImGui::Combo("Mode##hmd", &s_hmode, modes, 3);
    kbd_multiline("Code##hcd", s_hcode, sizeof(s_hcode),
                              ImVec2(-1, 90));
    ImGui::TextDisabled("Java: fn(Java,send,console,thiz,args,orig). "
                        "Smali: body of static hook(Object thiz, Object[] args); "
                        "p0=thiz p1=args, must return an Object.");
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
    kbd_textfield("Class##tc",  s_tcls, sizeof(s_tcls));
    kbd_textfield("Method##tm", s_tmth, sizeof(s_tmth));
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
    kbd_textfield("Package filter", s_rec_filter, sizeof(s_rec_filter));
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
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
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
            if (ImGui::Selectable(lbl) && !m.origin.empty())
                menu_open_class(cls_of_member(m.origin));  /* cross-link */
            if (!m.note.empty())
                ImGui::TextDisabled("    %s", m.note.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}

/* ===================== Script ===================== */
static void tab_script() {
    kbd_multiline("##sc", s_script, sizeof(s_script),
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
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
    MULK();
}

/* ===================== Classes ===================== */
static void class_tab_content(ClassTab& c) {
    const char* subs[] = {"Instances", "Methods", "Fields"};
    if (ImGui::BeginTabBar("cls")) {
        for (int si = 0; si < 3; si++) {
            bool sel = (c.sub == si);
            if (ImGui::BeginTabItem(subs[si], nullptr,
                    sel ? ImGuiTabItemFlags_SetSelected : 0)) {
                c.sub = si;
                ImGui::EndTabItem();
            } else if (sel) {
                c.sub = si;
            }
        }
        ImGui::EndTabBar();
    }
    if (c.waiting && !c.got_info)
        ImGui::TextDisabled("(waiting for agent...)");
    if (!c.got_info && !c.waiting)
        ImGui::TextDisabled("(no data - tap Refresh)");

    if (ImGui::Button("Refresh"))
        menu_open_class(c.cname);
    ImGui::SameLine();
    if (ImGui::Button("Find instances")) {
        std::string a = "{\"class\":\"" + jesc(c.cname) + "\"}";
        post_rpc("listInstances", a);
    }
    ImGui::Separator();

    if (c.sub == 0) {                        /* ---- Instances ---- */
        if (c.instances.empty()) {
            ImGui::TextDisabled("no instances loaded - Find instances");
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
               would be overwritten before it ever reaches the agent */
            static char nv[32][128];
            static int  nv_for = -1;
            if (nv_for != c.sel_inst) {
                nv_for = c.sel_inst;
                for (int f = 0; f < 32; f++) {
                    nv[f][0] = 0;
                    if (f < (int)in.fields.size())
                        snprintf(nv[f], sizeof(nv[f]), "%s",
                                 in.fields[f].value.c_str());
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
                ImGui::InputText("##fv", nv[f], sizeof(nv[f]));
                ImGui::SameLine();
                if (ImGui::Button("Set##fs")) {
                    std::string a = "{\"class\":\"" + jesc(c.cname)
                                  + "\",\"index\":" + std::to_string(c.sel_inst)
                                  + ",\"field\":\"" + jesc(fi.name)
                                  + "\",\"value\":\"" + jesc(nv[f]) + "\"}";
                    post_rpc("setInstanceField", a);
                }
                ImGui::PopID();
            }
        }
    } else if (c.sub == 1) {                 /* ---- Methods ---- */
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
                    ImGui::InputText(lb, c.call_args[k], sizeof(c.call_args[k]));
                    na++;
                }
            }
            if (ImGui::Button("Invoke")) {
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(c.call_sig.name)
                              + "\",\"args\":[";
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
        ImGui::BeginChild("clsm", ImVec2(0, 0), true);
        ImGuiListClipper clip;
        clip.Begin((int)c.methods.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                MethodSig& m = c.methods[i];
                ImGui::PushID(i);
                ImGui::BulletText("%s(%s) -> %s",
                                  m.name.c_str(), m.params.c_str(),
                                  m.ret.c_str());
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
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
    } else {                                 /* ---- Fields ---- */
        MLK();
        for (int i = 0; i < (int)c.fields.size(); i++) {
            FieldInfo& f = c.fields[i];
            ImGui::PushID(i);
            ImGui::BulletText("%s : %s = %s", f.name.c_str(),
                              f.type.c_str(), f.value.c_str());
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
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.2f, .5f, .2f, .6f));
    bool plus = ImGui::Button("+ search");
    ImGui::PopStyleColor();
    if (plus) s_search_on = !s_search_on;
    ImGui::SameLine();
    MLK();
    ImGui::TextDisabled("%lu open | agent %s",
                        (unsigned long)s_classes.size(),
                        s_agent_ready ? "ready" : "...");
    MULK();

    if (s_search_on) {
        ImGui::Separator();
        kbd_textfield("Filter##sf", s_search, sizeof(s_search));
        ImGui::SameLine();
        if (ImGui::Button("Search")) {
            MLK();
            s_search_wait = true;
            int qid = ++s_search_seq;
            MULK();
            std::string a = "{\"filter\":\"" + jesc(s_search)
                           + "\",\"qid\":" + std::to_string(qid) + "}";
            post_rpc("listLoadedClasses", a);
        }
        MLK();
        if (s_search_wait) ImGui::TextDisabled("(searching...)");
        else ImGui::TextDisabled("%ld matched, showing %ld",
                                 (long)s_search_total, (long)s_search_shown);
        ImGui::BeginChild("srch", ImVec2(0, 220), true);
        ImGuiListClipper clip;
        clip.Begin((int)s_search_res.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(s_search_res[i].c_str())) {
                    menu_open_class(s_search_res[i]);
                    s_search_on = false;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
        MULK();
        ImGui::Separator();
    }

    MLK();
    if (!s_classes.empty()) {
        /* which open class is visible? ImGui has no query for "current
           tab", so remember the last one the user clicked */
        static int s_cls_cur = 0;
        if (ImGui::BeginTabBar("cls")) {
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
        MLK();
        if (cur >= 0 && cur < (int)s_classes.size()) {
            class_tab_content(s_classes[cur]);
        }
        MULK();
    } else {
        ImGui::TextDisabled("no class tabs - use + search");
    }
    MULK();
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
        /* publish empty region list: drags elsewhere must not scroll */
        overlay_publish_scroll_regions(s_regions);
        s_regions.clear();
        overlay_publish_kbd_rect(0, 0, 0, 0);
        return;
    }
    draw_fab(ds);
    draw_kbd(ds);

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(420, 300), ImVec2(ds.x * .95f, ds.y * .95f));
    ImGui::SetNextWindowSize(ImVec2(ds.x * .8f, ds.y * .85f), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(s_opacity);
    bool open = vis;
    ImGui::Begin("Dex-tool v1.2 (r11)", &open,
                 ImGuiWindowFlags_NoCollapse);
    if (!open) { MLK(); s_visible = false; s_conf_dirty = true; MULK(); }

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

/* ---- desktop unit-test backdoor (compiled only with -DDXT_KBD_TEST;
   never defined for the Android build) ---- */
#ifdef DXT_KBD_TEST
extern "C" void kbd_test_setup(float w, float h) {
    s_kbd.open = true; s_kbd.shift = false; s_kbd.page2 = false;
    s_kbd.dragging = false; s_kbd.last.clear();
    s_kbd.w = w; s_kbd.h = 236.f;
    s_kbd.x = 0; s_kbd.y = h - s_kbd.h; if (s_kbd.y < 0) s_kbd.y = 0;
    s_kbd_keys.clear();
}
extern "C" void kbd_test_field(char* buf, size_t cap) {
    s_kbd_field = { buf, cap };
}
extern "C" bool kbd_test_feed(int phase, float x, float y) {
    return menu_kbd_feed(phase, x, y);
}
extern "C" bool kbd_test_is_open() { return s_kbd.open; }
extern "C" void kbd_test_draw(float w, float h) { draw_kbd(ImVec2(w, h)); }
#endif
