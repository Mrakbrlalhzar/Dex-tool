/* Dex-tool menu (r31): M4 real JSON tree parser (shallowest-match
   lookup, surrogate pairs, depth cap), M7 releaseHandle on object
   tab close, M8 lock-free frame pump via atomic mirror. r30:
   flash-loop lock, deque tab containers, dead trigger removed. */
#include "menu.h"
#include "overlay.h"
#include "frida_bridge.h"
#include "logger.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <deque>   /* r30 C2: stable refs across push_back */
#include <atomic>  /* r31 M8: lock-free frame-pump mirror */
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <pthread.h>
#include <ctime>
static pthread_mutex_t g_mtx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#define MLK()   pthread_mutex_lock(&g_mtx)
#define MULK()  pthread_mutex_unlock(&g_mtx)
static float s_font_size  = 18.0f;
static float s_opacity    = 0.92f;
static int   s_theme      = 0;
static float s_ovl_scroll = 1.5f;   /* whole-menu drag speed */
static float s_lst_scroll = 1.2f;   /* inner list drag speed */
static bool  s_visible    = true;
static std::atomic<bool> s_visible_pub{ true };  /* r31 M8 */
static bool  s_show_fab   = true;
static float s_y_off      = 0.0f;   /* manual touch Y bias (px); r15: default 0 - the screen-space touch map is mathematically exact, the slider stays for exotic hosts */
static bool  s_agent_ready = false;
static char  s_agent_note[160] = "";
static bool  s_ovl_ready   = false;
static std::vector<float> s_regions;
static float fit_h(float frac, float min_h) {
    /* r29: list heights follow the menu window size */
    float h = ImGui::GetContentRegionAvail().y * frac;
    return h < min_h ? min_h : h;
}
static void mark_rect(float x, float y, float w, float h, float speed) {
    if (w <= 1 || h <= 1 || speed <= 0.01f) return;
    s_regions.push_back(x); s_regions.push_back(y);
    s_regions.push_back(w); s_regions.push_back(h);
    s_regions.push_back(speed);
}
static void mark_scroll_item(float speed) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    mark_rect(a.x, a.y, b.x - a.x, b.y - a.y, speed);
}
struct MethodSig { std::string name, params, ret; };
struct FieldInfo { std::string name, type, value; };
struct InstInfo  { std::string str; long handle = -1;
                   std::vector<FieldInfo> fields; };
struct ClassTab {
    std::string cname;
    int  uid = 0;                       /* r26: stable window/tab id */
    bool win_close = false;             /* r26: X pressed -> erase */
    bool got_info = false, waiting = false;
    std::vector<MethodSig> methods;
    std::vector<FieldInfo> fields;
    std::vector<InstInfo>  instances;
    int  sel_inst = -1;
    bool inst_wait = false;             /* r29: heap scan in flight */
    bool inst_creat = false;            /* r29: $new in flight      */
    int  gen = -1;                      /* r23: instance-list generation; part of the editor buffer key */
    int  sub = 0;                       /* 0 Instances 1 Static Fields 2 Methods 3 Fields */
    int  expanded = -1;                 /* index into methods, -1 none   */
    int  msub = 0;                      /* 0 Call 1 Patch 2 Trace 3 Hook */
    MethodSig call_sig;
    char call_args[8][128] = {};
    char call_typ[8][64]   = {};        /* r26: parameter types          */
    int  call_nargs = 0;
    int  call_count = 1;                /* r26: Call Count input         */
    std::vector<std::string> call_log;  /* r26: per-call results         */
    char patch_val[256] = {};           /* manual value                  */
    std::string patch_note;
    int  hook_mode = 0;
    char hook_code[4096] = {};
    long of_handle = -1;
    std::string of_note;
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
struct MonRow   { std::string kind, url, method, origin, note, detail; };
struct ExplorerWin {
    int  id = 0;
    bool open = true;
    char search[160] = {};
    char fam_search[96] = {};     /* r27: "Search families..." filter */
    int  family = 0;              /* index into s_families           */
    bool all_fam = true;          /* "All" checkbox = every family   */
    bool case_sensitive = false;
    int  filter_by = 0;           /* 0 Class 1 Method 2 Field        */
    bool advanced = false;        /* string search inside class code */
};
static std::deque<ExplorerWin> s_explorers;
static std::vector<std::string> s_families;   /* com.game, com.android... */
static bool s_expl_req = false;               /* "+" pressed               */
static int  s_expl_seq = 0;
static struct ParamScan {
    int uid = -1;                 /* inspector window the dropdown is in */
    int arg = -1;                 /* parameter index                     */
    std::string cls;              /* parameter type                      */
    std::vector<InstInfo> list;
    bool waiting = false;
} s_pscan;
static int  s_tab_uid_seq = 0;
static int  s_focus_expl = 0;                 /* explorer id to raise     */
static std::string s_focus_win;               /* inspector title to raise */
static bool        s_fm_open = false;
static std::string s_fm_cls, s_fm_mth;
static char        s_fm_src[16384] = "";
static char        s_fm_note[200] = "";
static bool        s_fm_have = false;
static char s_clip_msg[192] = "";
static long s_clip_ms = 0;
static std::deque<ClassTab>  s_classes;
static std::vector<PatchRow>   s_patches;
static std::vector<HookRow>    s_hooks;
static std::deque<Tracker>    s_trackers;
static std::deque<RecRow>     s_rec;
static std::deque<MonRow>     s_mon;
static std::deque<std::string> s_cons;      /* script console */
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
static int  s_search_mode = 0;
static bool s_search_sys  = false;
static bool s_search_cs   = false;
static char s_hsrc[16384] = "";
static char s_hsrc_note[200] = "";
static bool s_hsrc_have = false;
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
static std::string s_next_tab;           /* top tab to select */
static bool    s_fullscreen = false;
static ImVec2  s_fs_last_pos(0, 0), s_fs_last_size(0, 0);
static bool    s_fs_restore = false;
static int     s_scale = 3;
static const char* k_scale_names[7] = { "Smallest", "Smaller", "Small",
                                        "Default", "Large", "Larger",
                                        "Largest" };
static const float k_scale_px[7]    = { 10.f, 13.f, 16.f, 18.f, 22.f,
                                        26.f, 32.f };
struct HitFlash { std::string name; long count; float time; float gone; };
static std::vector<HitFlash> s_flash;
static void flash_hit(const std::string& name, long count) {
    for (auto& f : s_flash) {
        if (f.name == name) {
            f.count = count; f.time = 2.f; f.gone = 10.f;
            return;
        }
    }
    s_flash.push_back({name, count, 2.f, 10.f});
    if (s_flash.size() > 10) s_flash.erase(s_flash.begin());
}
static std::unordered_map<std::string, long> s_hits;
struct ObjTab {
    long handle = -1;
    std::string cls, str;
    std::vector<FieldInfo> fields;
    int  sel = -1;               /* field row being edited            */
    char edit[256] = {};         /* its edit buffer                   */
};
static std::deque<ObjTab> s_objtabs;
static long s_objfocus = -1;     /* handle of the tab to select       */
struct SavedObj { std::string cls, str; long handle = -1; };
static std::vector<SavedObj> s_saved;
static bool  s_dump_wait = false;
static bool  s_dump_done = false;
static long  s_dump_count = 0;
static char  s_dump_path[512] = "";
static char  s_dump_note[192] = "";
static char  s_dobj_note[192] = "";
static std::string s_pkg = "(unknown)";
/* r29: instances live in the Inspector's Instances sub-tab; these
   hold the "pick for method arg" target while a pick is active */
static int  s_arg_uid = -1;      /* class tab owning the arg      */
static int  s_arg_idx = -1;      /* arg N                          */
static char s_arg_typ[96] = {};
static char s_arg_cls[200] = {}; /* class being picked for        */
static bool s_fam_open = false;  /* r29: Class Families window    */
static int  s_fam_uid = -1;      /* explorer that opened it       */
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
/* ---- r31 M4: minimal scoped JSON tree --------------------------
   Replaces the old substring scans (jstr / jnum / jhas / jarr /
   jtype / jobjarr / junesc): keys resolve to the SHALLOWEST match,
   so nested duplicate keys can no longer return the wrong value;
   escapes are handled once, with UTF-16 surrogate pairs encoded as
   real UTF-8; parse depth is capped so hostile input cannot blow
   the stack. ---- */
struct JVal {
    int kind = 0;   /* 0=undef 1=obj 2=arr 3=str 4=num 5=bool 6=null */
    std::vector<std::pair<std::string, JVal>> obj;
    std::vector<JVal> arr;
    std::string str;
    double num = 0;
    bool b = false;
};
static void jws(const std::string& s, size_t& i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
}
static bool jparse_str(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    i++;
    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { i++; return true; }
        if (c != '\\') { out += c; i++; continue; }
        i++;
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/';  break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                unsigned v = 0;
                for (int k = 0; k < 4; k++) {
                    char h = s[i + k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        v |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                i += 4;
                if (v >= 0xD800 && v <= 0xDBFF &&
                    i + 6 <= s.size() &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    /* r31: pair the high surrogate before encoding */
                    unsigned lo = 0;
                    int ok = 1;
                    for (int k = 2; k < 6; k++) {
                        char h = s[i + k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9')
                            lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            lo |= (unsigned)(h - 'A' + 10);
                        else { ok = 0; break; }
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        v = 0x10000u + ((v - 0xD800u) << 10)
                          + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                if (v < 0x80) out += (char)v;
                else if (v < 0x800) {
                    out += (char)(0xC0 | (v >> 6));
                    out += (char)(0x80 | (v & 0x3F));
                } else if (v < 0x10000) {
                    out += (char)(0xE0 | (v >> 12));
                    out += (char)(0x80 | ((v >> 6) & 0x3F));
                    out += (char)(0x80 | (v & 0x3F));
                } else {
                    out += (char)(0xF0 | (v >> 18));
                    out += (char)(0x80 | ((v >> 12) & 0x3F));
                    out += (char)(0x80 | ((v >> 6) & 0x3F));
                    out += (char)(0x80 | (v & 0x3F));
                }
                break;
            }
            default: return false;
        }
    }
    return false;
}
static bool jparse_val(const std::string& s, size_t& i, int depth,
                       JVal& out) {
    if (depth > 64) return false;   /* r31: stack guard */
    jws(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
        out.kind = 1;
        i++;
        jws(s, i);
        if (i < s.size() && s[i] == '}') { i++; return true; }
        for (;;) {
            jws(s, i);
            std::string k;
            if (!jparse_str(s, i, k)) return false;
            jws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            i++;
            JVal v;
            if (!jparse_val(s, i, depth + 1, v)) return false;
            out.obj.push_back({k, std::move(v)});
            jws(s, i);
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return true; }
            return false;
        }
    }
    if (c == '[') {
        out.kind = 2;
        i++;
        jws(s, i);
        if (i < s.size() && s[i] == ']') { i++; return true; }
        for (;;) {
            JVal v;
            if (!jparse_val(s, i, depth + 1, v)) return false;
            out.arr.push_back(std::move(v));
            jws(s, i);
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return true; }
            return false;
        }
    }
    if (c == '"') { out.kind = 3; return jparse_str(s, i, out.str); }
    if (c == 't' && s.compare(i, 4, "true") == 0) {
        i += 4; out.kind = 5; out.b = true; return true;
    }
    if (c == 'f' && s.compare(i, 5, "false") == 0) {
        i += 5; out.kind = 5; return true;
    }
    if (c == 'n' && s.compare(i, 4, "null") == 0) {
        i += 4; out.kind = 6; return true;
    }
    size_t st = i;
    if (c == '-' || c == '+') i++;
    while (i < s.size() &&
           ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
            s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+'))
        i++;
    if (i == st) return false;
    out.kind = 4;
    out.num = strtod(s.c_str() + st, nullptr);
    return true;
}
static const JVal* jfind_bfs(const JVal& root, const char* key) {
    /* breadth-first: the shallowest occurrence of key wins */
    std::vector<const JVal*> level;
    level.push_back(&root);
    while (!level.empty()) {
        std::vector<const JVal*> next;
        for (const JVal* n : level) {
            if (n->kind == 1) {
                for (auto& kv : n->obj) {
                    if (kv.first == key) return &kv.second;
                    if (kv.second.kind == 1 || kv.second.kind == 2)
                        next.push_back(&kv.second);
                }
            } else if (n->kind == 2) {
                for (auto& v : n->arr)
                    if (v.kind == 1 || v.kind == 2) next.push_back(&v);
            }
        }
        level.swap(next);
    }
    return nullptr;
}
static bool jparse_root(const std::string& j, JVal& root) {
    size_t i = 0;
    if (!jparse_val(j, i, 0, root)) return false;
    jws(j, i);
    return true;
}
static std::string jdump(const JVal& v) {
    /* re-serialise a parsed value (jobjarr returns object strings) */
    std::string r;
    if (v.kind == 1) {
        r += '{';
        bool first = true;
        for (auto& kv : v.obj) {
            if (!first) r += ',';
            first = false;
            r += '"'; r += jesc(kv.first); r += "\":";
            r += jdump(kv.second);
        }
        r += '}';
    } else if (v.kind == 2) {
        r += '[';
        bool first = true;
        for (auto& e : v.arr) {
            if (!first) r += ',';
            first = false;
            r += jdump(e);
        }
        r += ']';
    } else if (v.kind == 3) {
        r += '"'; r += jesc(v.str); r += '"';
    } else if (v.kind == 4) {
        char b[40];
        if (v.num == (double)(long long)v.num &&
            v.num > -1e15 && v.num < 1e15)
            snprintf(b, sizeof(b), "%lld", (long long)v.num);
        else
            snprintf(b, sizeof(b), "%.9g", v.num);
        r += b;
    } else if (v.kind == 5) {
        r += v.b ? "true" : "false";
    } else {
        r += "null";
    }
    return r;
}
static std::vector<std::string> jarr(const std::string& j, const char* key) {
    std::vector<std::string> r;
    JVal root;
    if (!jparse_root(j, root)) return r;
    const JVal* v = jfind_bfs(root, key);
    if (!v || v->kind != 2) return r;
    for (auto& e : v->arr)
        if (e.kind == 3) r.push_back(e.str);
    return r;
}
static std::string jstr(const std::string& j, const char* key) {
    JVal root;
    if (!jparse_root(j, root)) return "";
    const JVal* v = jfind_bfs(root, key);
    return (v && v->kind == 3) ? v->str : "";
}
static long jnum(const std::string& j, const char* key, long dflt = 0) {
    JVal root;
    if (!jparse_root(j, root)) return dflt;
    const JVal* v = jfind_bfs(root, key);
    return (v && v->kind == 4) ? (long)v->num : dflt;
}
static bool jhas(const std::string& j, const char* key) {
    JVal root;
    if (!jparse_root(j, root)) return false;
    return jfind_bfs(root, key) != nullptr;
}
static std::string jtype(const std::string& j) {
    return jstr(j, "type");
}
static std::vector<std::string> jobjarr(const std::string& j,
                                        const char* key) {
    std::vector<std::string> r;
    JVal root;
    if (!jparse_root(j, root)) return r;
    const JVal* v = jfind_bfs(root, key);
    if (!v || v->kind != 2) return r;
    for (auto& e : v->arr)
        if (e.kind == 1) r.push_back(jdump(e));
    return r;
}
/* ---- end r31 JSON tree ---- */
static void post_rpc(const char* fn, const std::string& args) {
    std::string m = "{\"type\":\"rpc\",\"fn\":\"";
    m += fn; m += "\",\"args\":"; m += args; m += "}";
    frida_post(m);
}
static void push_console(const char* tag, const std::string& line) {
    char b[256];
    snprintf(b, sizeof(b), "%s %s", tag, line.c_str());
    s_cons.push_back(b);
    if (s_cons.size() > 400) s_cons.erase(s_cons.begin());
}
static std::string short_name(const std::string& cls) {
    auto d = cls.rfind('.');
    return (d == std::string::npos) ? cls : cls.substr(d + 1);
}
static std::string win_title(const ClassTab& c) {
    return short_name(c.cname) + "##t" + std::to_string(c.uid);
}
static ClassTab* open_inspector(const std::string& name, bool switch_tab) {
    if (name.empty()) return nullptr;
    ClassTab* out = nullptr;
    MLK();
    for (auto& c : s_classes)
        if (c.cname == name) { c.waiting = true; out = &c; break; }
    if (!out) {
        ClassTab nt{};
        nt.cname   = name;
        nt.uid     = ++s_tab_uid_seq;
        nt.waiting = true;
        s_classes.push_back(nt);
        out = &s_classes.back();
    }
    s_focus_win = win_title(*out);
    if (switch_tab) s_next_tab = "Tools";   /* r28 */
    MULK();
    std::string a = "{\"class\":\"" + jesc(name) + "\"}";
    post_rpc("openClass", a);
    return out;
}
void menu_open_class(const std::string& name) {
    (void)open_inspector(name, true);
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
static std::string cls_of_member(const std::string& s) {
    auto d = s.rfind('.');
    if (d == std::string::npos || d == 0) return s;
    return s.substr(0, d);
}
static std::string family_of(const std::string& cls) {
    auto d1 = cls.find('.');
    if (d1 == std::string::npos) return cls;
    auto d2 = cls.find('.', d1 + 1);
    if (d2 == std::string::npos) return cls;
    return cls.substr(0, d2);
}
static bool contains_ci(const std::string& hay, const char* nd) {
    if (!nd || !nd[0]) return true;
    std::string a, b = nd;
    a.reserve(hay.size());
    for (char ch : hay) a += (char)tolower((unsigned char)ch);
    for (char& ch : b)  ch = (char)tolower((unsigned char)ch);
    return a.find(b) != std::string::npos;
}
static std::vector<std::string> split_params(const std::string& p) {
    std::vector<std::string> ps;
    std::string cur;
    int depth = 0;
    for (char ch : p) {
        if (ch == '<' || ch == '[') depth++;
        else if (ch == '>' || ch == ']') { if (depth > 0) depth--; }
        if (ch == ',' && depth == 0) { ps.push_back(cur); cur.clear(); continue; }
        cur += ch;
    }
    ps.push_back(cur);
    if (ps.size() == 1 && ps[0].empty()) ps.clear();
    for (auto& s : ps) {
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ')  s.pop_back();
    }
    return ps;
}
static bool is_prim_type(const std::string& t) {
    std::string s = t;
    if (s == "int32")   s = "int";
    if (s == "int64")   s = "long";
    if (s == "string" || s == "String") s = "java.lang.String";
    return s == "int" || s == "long" || s == "short" || s == "byte" ||
           s == "char" || s == "float" || s == "double" ||
           s == "boolean" || s == "java.lang.String";
}
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
    bool have_scale = false;   /* r30 H4: honor a legacy font= value */
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
        else if (!strcmp(k, "scale"))  { s_scale = (int)v; have_scale = true; }
    }
    fclose(f);
    if (s_scale < 0 || s_scale > 6) s_scale = 3;
    if (!have_scale) {
        /* r30 H4: a legacy font= value was parsed then discarded -
           derive the scale from it instead */
        float best = 1e9f;
        for (int i = 0; i < 7; i++) {
            float d = k_scale_px[i] - s_font_size;
            if (d < 0) d = -d;
            if (d < best) { best = d; s_scale = i; }
        }
    }
    s_font_size = k_scale_px[s_scale];
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
    fprintf(f, "scale=%d\n", s_scale);              /* r28 */
    fclose(f);
    s_conf_dirty = false;
    s_conf_last_ms = conf_now_ms();
}
static void copy_text(const std::string& what, const std::string& text) {
    ImGui::SetClipboardText(text.c_str());
    std::string t = text;
    if (t.size() > 140) t = t.substr(0, 137) + "...";
    snprintf(s_clip_msg, sizeof(s_clip_msg), "copied %s: %s",
             what.c_str(), t.c_str());
    s_clip_ms = conf_now_ms();
}
static void apply_theme() {
    static int applied = -1;
    if (applied != s_theme) {   /* r30 M9: on change, not every frame */
        applied = s_theme;
        switch (s_theme) {
            case 0: ImGui::StyleColorsDark();    break;
            case 1: ImGui::StyleColorsLight();   break;
            case 2: ImGui::StyleColorsClassic(); break;
        }
    }
    ImGui::GetIO().FontGlobalScale = 1.0f;   /* real size comes from the
                                               atlas, not from scaling */
}
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
static bool ime_textfield(const char* label, char* buf, size_t cap,
                          ImGuiInputTextFlags extra = 0) {
    bool used = ImGui::InputText(label, buf, cap,
                                 ImGuiInputTextFlags_AutoSelectAll | extra);
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
        ime_show();     /* re-tap while active: keyboard was closed */
    }
    return used;
}
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
void menu_init() {
    apply_theme();
    conf_load();
    overlay_set_touch_bias(s_y_off);
    overlay_request_font_rebuild(s_font_size);
    frida_set_message_callback([](const std::string& j) {
        MLK();
        std::string ty = jtype(j);
        if (ty == "ready") {
            s_agent_ready = true;
            s_cls_auto_done = false;  /* new agent: re-list classes */
            snprintf(s_agent_note, sizeof(s_agent_note), "%s",
                     jstr(j, "msg").c_str());
            s_pkg = jstr(j, "pkg");
        } else if (ty == "imeEnter") {
            s_ime_enter = 2;
        } else if (ty == "imeHidden") {
            if (s_ime_active) {
                if (s_ime_enter > 0) s_ime_enter_rel = true;
                else { s_ime_active = false; s_ime_clear = true; }
            }
        } else if (ty == "hookHit") {
            {
                std::string nm = jstr(j, "name");
                long ct = jnum(j, "count");
                if (!nm.empty()) {
                    s_hits[nm] = ct;
                    flash_hit(nm, ct);
                }
            }
        } else if (ty == "dumpReady") {
            std::string derr = jstr(j, "error");
            s_dump_wait = false;
            s_dump_done = derr.empty();
            s_dump_count = jnum(j, "count");
            snprintf(s_dump_path, sizeof(s_dump_path), "%s",
                     jstr(j, "path").c_str());
            snprintf(s_dump_note, sizeof(s_dump_note), "%s",
                     derr.empty() ? ""
                                  : ("dump: " + derr).c_str());
        } else if (ty == "staticValues") {
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
                c->hits_m = jarr(j, "hits_m");
                c->hits_f = jarr(j, "hits_f");
            }
        } else if (ty == "methodSrc") {
            if (s_fm_open && jstr(j, "class") == s_fm_cls &&
                jstr(j, "method") == s_fm_mth) {
                if (jnum(j, "ok")) {
                    std::string src = jstr(j, "src");
                    snprintf(s_fm_src, sizeof(s_fm_src), "%s", src.c_str());
                    snprintf(s_fm_note, sizeof(s_fm_note),
                             "smali (bytecode) of %s.%s",
                             s_fm_cls.c_str(), s_fm_mth.c_str());
                    s_fm_have = true;
                } else {
                    s_fm_src[0] = 0;
                    s_fm_have = false;
                    snprintf(s_fm_note, sizeof(s_fm_note),
                             "source: %s", jstr(j, "error").c_str());
                }
            } else if (jstr(j, "class") == s_hcls &&
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
            long qid  = jnum(j, "qid");
            long page = jnum(j, "page");
            if (qid == (long)s_search_qid) {
                if (!s_search_page0) {
                    s_search_res.clear();
                    s_search_page0 = true;
                }
                for (auto& c : jarr(j, "classes")) s_search_res.push_back(c);
                s_search_total = (int)jnum(j, "total");
                s_search_shown = (int)jnum(j, "shown");
                if (page + 1 >= jnum(j, "pages")) {
                    s_search_wait = false;
                    s_families.clear();
                    for (auto& n : s_search_res) {
                        std::string f = family_of(n);
                        if (!f.empty() &&
                            std::find(s_families.begin(), s_families.end(),
                                      f) == s_families.end())
                            s_families.push_back(f);
                    }
                    std::sort(s_families.begin(), s_families.end());
                }
            }
        } else if (ty == "instanceList") {
            std::vector<InstInfo> list;
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
                list.push_back(std::move(in));
            }
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                c->instances = list;
                c->gen = (int)jnum(j, "gen");
                c->sel_inst = -1;
                c->inst_wait = false;
            }
            for (auto& t : s_objtabs) {
                if (t.cls != jstr(j, "class")) continue;
                for (auto& in2 : list) {
                    if (in2.handle == t.handle) {
                        t.fields = in2.fields;
                        if (!in2.str.empty()) t.str = in2.str;
                        break;
                    }
                }
            }
            if (s_pscan.cls == jstr(j, "class")) {
                s_pscan.list = list;
                s_pscan.waiting = false;
            }
        } else if (ty == "instanceCreated") {
            InstInfo in;
            in.str    = jstr(j, "str");
            in.handle = jnum(j, "handle", -1);
            ClassTab* cc = find_tab(jstr(j, "class"));
            if (cc) {
                cc->instances.push_back(in);
                cc->inst_creat = false;
                cc->inst_wait  = false;
            }
            push_console("[finder]", "created " + jstr(j, "class") +
                         " -> handle " + std::to_string(in.handle));
        } else if (ty == "callResult") {
            ClassTab* c = find_tab(jstr(j, "class"));
            if (c) {
                long idx = jnum(j, "idx", 0), of = jnum(j, "of", 0);
                std::string body = jhas(j, "ok") && jnum(j, "ok") ?
                    jstr(j, "result") : ("ERR: " + jstr(j, "error"));
                char hdr[32] = "";
                if (of > 1) snprintf(hdr, sizeof(hdr), "call %ld/%ld: ",
                                     idx + 1, of);
                c->call_log.push_back(std::string(hdr) + body);
                if (c->call_log.size() > 200)
                    c->call_log.erase(c->call_log.begin());
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
static void tab_settings() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Separator();
    ImGui::Text("Display");
    if (s_scale < 0 || s_scale > 6) s_scale = 3;
    if (ImGui::BeginCombo("Scale##Global", k_scale_names[s_scale])) {
        for (int i = 0; i < 7; i++) {
            bool sel = (i == s_scale);
            if (ImGui::Selectable(k_scale_names[i], sel)) {
                s_scale    = i;
                s_font_size = k_scale_px[i];
                overlay_request_font_rebuild(s_font_size);
                s_conf_dirty = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("font size preset (the reference's "
                          "Smallest..Largest scale)");
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
    ImGui::Text("Info");
    ImGui::Text("Package: %s", s_pkg.c_str());
    s_ovl_ready = overlay_ready();
    float dw=0, dh=0;
    overlay_get_display_size(&dw, &dh);
    ImGui::Text("Overlay: %s (%.0fx%.0f)",
                s_ovl_ready ? "up" : "waiting",
                dw, dh);
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
#ifdef __aarch64__
    ImGui::Text("Arch: %s", "arm64-v8a");
#else
    ImGui::Text("Arch: %s", "armeabi-v7a");
#endif
    ImGui::Separator();
}
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
static void tab_tracer() {
    MLK();
    int ntr = (int)s_trackers.size(), nhk = (int)s_hooks.size();
    MULK();
    ImGui::Text("Traced method count : %d", ntr + nhk);
    if (ntr + nhk == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
        ImGui::Text("No traced method has been called yet");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("open a class in Tools, expand a method and "
                            "use its Trace tab - or add one below");
    } else if (ImGui::Button("Quick Restore")) {
        ImGui::OpenPopup("QuickRestorePopup");
    }
    if (ImGui::BeginPopup("QuickRestorePopup",
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1),
                           "tap a row to remove its trace");
        ImGui::Separator();
        for (int i = 0; i < ntr; ) {
            MLK();
            std::string nm = s_trackers[i].name;
            bool isf = s_trackers[i].is_field;
            MULK();
            ImGui::PushID(7000 + i);
            if (ImGui::Button((nm + (isf ? " (field)" : ""))
                                  .c_str(),
                              ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                std::string a = "{\"name\":\"" + jesc(nm) + "\"}";
                post_rpc("removeTracker", a);
                MLK();
                s_trackers.erase(s_trackers.begin() + i);
                MULK();
                ntr--;
            } else { ImGui::PopID(); i++; continue; }
            ImGui::PopID();
        }
        for (int i = 0; i < nhk; ) {
            MLK();
            std::string nm = s_hooks[i].cls + "." + s_hooks[i].method;
            MULK();
            ImGui::PushID(7500 + i);
            if (ImGui::Button(nm.c_str(),
                              ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                std::string a = "{\"class\":\"" + jesc(s_hooks[i].cls)
                              + "\",\"method\":\"" + jesc(s_hooks[i].method)
                              + "\"}";
                post_rpc("removeHook", a);
                MLK();
                s_hooks.erase(s_hooks.begin() + i);
                MULK();
                nhk--;
            } else { ImGui::PopID(); i++; continue; }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::BeginChild("tracerlist", ImVec2(0, 0), false);
    MLK();
    for (int i = 0; i < (int)s_trackers.size(); i++) {
        ImGui::PushID(8000 + i);
        Tracker& t = s_trackers[i];
        long hits = 0;
        auto hi = s_hits.find(t.name);
        if (hi != s_hits.end()) hits = hi->second;
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "%s (%ldx)##tr%d", t.name.c_str(),
                 hits, i);
        if (ImGui::CollapsingHeader(hdr)) {
            ImGui::TextDisabled("%lu events", (unsigned long)t.log.size());
            ImGui::BeginChild(("tl" + std::to_string(i)).c_str(),
                              ImVec2(0, fit_h(.45f, 110)), true);
            for (auto& r : t.log) {
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
                ImGui::PopID();
                MULK();
                ImGui::EndChild();
                return;
            }
        }
        ImGui::PopID();
    }
    static const char* hmodes[] = {"Java(JS)", "Smali", "Nop (auto)"};
    for (int i = 0; i < (int)s_hooks.size(); i++) {
        ImGui::PushID(8500 + i);
        HookRow& h = s_hooks[i];
        long hits = 0;
        std::string key = h.cls + "." + h.method;
        auto hi = s_hits.find(key);
        if (hi != s_hits.end()) hits = hi->second;
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
        if (ImGui::SmallButton(("open##ho" + std::to_string(i)).c_str()))
            menu_open_class(h.cls);
        ImGui::SameLine();
        ImGui::Text("[%s] %s (%ldx)", hmodes[h.mode], key.c_str(), hits);
        if (!h.note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "[%s]",
                               h.note.c_str());
        }
        ImGui::PopID();
    }
    MULK();
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    if (ImGui::CollapsingHeader("Manual add")) {
        static const char* modes[] = {"Java(JS)", "Smali", "Nop (auto)"};
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
                ImGui::BeginChild("hsrcbx", ImVec2(0, fit_h(.40f, 120)), true);
                ImGui::TextUnformatted(s_hsrc);
                ImGui::EndChild();
                mark_scroll_item(s_lst_scroll);
                if (s_hmode == 1 && ImGui::SmallButton("Copy body -> editor")) {
                    std::string src = s_hsrc;
                    auto b = src.find('\n');
                    if (b != std::string::npos) src = src.substr(b + 1);
                    auto e = src.rfind(".end method");
                    if (e != std::string::npos) src = src.substr(0, e);
                    snprintf(s_hcode, sizeof(s_hcode), "%s", src.c_str());
                }
            }
        }
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
        ime_textfield("Class##tc",  s_tcls, sizeof(s_tcls));
        ime_textfield("Method##tm", s_tmth, sizeof(s_tmth));
        if (ImGui::Button("Track") && s_tcls[0] && s_tmth[0]) {
            std::string a = "{\"class\":\"" + jesc(s_tcls)
                          + "\",\"method\":\"" + jesc(s_tmth) + "\"}";
            post_rpc("addTracker", a);
            MLK();
            s_trackers.push_back({std::string(s_tcls) + "." +
                                  std::string(s_tmth), false, {}});
            MULK();
            s_tcls[0] = s_tmth[0] = 0;
        }
    }
}
static void tab_dumper() {
    if (s_dump_wait) ImGui::Text("Dumping...");
    if (ImGui::Button("DUMP")) {
        if (!s_dump_wait) {
            s_dump_wait = true;
            s_dump_done = false;
            snprintf(s_dump_note, sizeof(s_dump_note), "writing...");
            post_rpc("dumpLoadedClasses", "{}");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s_dump_note);
    if (s_dump_done) {
        ImGui::TextColored(ImVec4(0, 1, .4f, 1), "Done - %ld classes",
                           s_dump_count);
        ImGui::TextDisabled("saved in the app files dir:");
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(s_dump_path);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Copy path"))
            copy_text("path", s_dump_path);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Per-object dumps: Tools > [0x..] object tab > "
                        "Dump to file.");
}
static void tab_recorder() {
    MLK();
    if (!s_rec_on) {
        MULK();
        if (ImGui::Button("Start")) {
            s_rec_on = true;
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
    ImGui::BeginChild("recl", ImVec2(0, fit_h(.55f, 140)), true);
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
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}
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
    ImGui::BeginChild("monl", ImVec2(0, fit_h(.55f, 140)), true);
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
            if (ImGui::Selectable(lbl)) s_mon_sel = i;
            if (!m.note.empty())
                ImGui::TextDisabled("    %s", m.note.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
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
    ImGui::BeginChild("consl", ImVec2(0, fit_h(.40f, 110)), true);
    for (auto& l : s_cons) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}
static void open_obj_tab(const std::string& cls, long handle,
                         const std::string& str);
static void explorer_body(ExplorerWin& e);
static void class_tab_body(ClassTab& c);
static void of_deliver(ClassTab* dst, long handle,
                       const std::string& str);
static std::string s_dup_req;
static void run_search(const char* text, int mode, bool cs) {
    MLK();
    snprintf(s_search, sizeof(s_search), "%s", text);
    s_search_mode = mode;
    s_search_cs   = cs;
    s_search_wait = true;
    int sqid = ++s_search_seq;
    s_search_qid   = sqid;     /* r23: generation armed at SEND time */
    s_search_page0 = false;
    MULK();
    std::string a = "{\"filter\":\"" + jesc(s_search)
                   + "\",\"qid\":" + std::to_string(sqid)
                   + ",\"mode\":" + std::to_string(mode)
                   + ",\"sys\":" + (s_search_sys ? "true" : "false")
                   + ",\"cs\":" + (cs ? "true" : "false") + "}";
    post_rpc("listLoadedClasses", a);
}
static void inst_view(ClassTab& c) {
    /* r29: the single place to find / create / pick instances (the
       Object Finder popup and the Inspector button are merged in) */
    MLK();
    bool picking = (s_arg_uid >= 0 && s_arg_cls[0] &&
                    c.cname == s_arg_cls);
    if (picking)
        ImGui::TextColored(ImVec4(.4f, 1.f, .5f, 1),
                           "picking an instance for arg%d (%s)",
                           s_arg_idx, s_arg_typ);
    if (ImGui::Button("Find instances##fi")) {
        std::string a = "{\"class\":\"" + jesc(c.cname) + "\"}";
        post_rpc("listInstances", a);
        c.inst_wait = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Create instance##ci")) {
        post_rpc("createInstance",
                 "{\"class\":\"" + jesc(c.cname) + "\"}");
        c.inst_creat = true;
    }
    ImGui::SameLine();
    if (picking && ImGui::SmallButton("Cancel pick##cp")) {
        s_arg_uid = -1; s_arg_idx = -1; s_arg_cls[0] = 0;
    }
    static char s_man[128] = {};      /* manual handle/value entry */
    ImGui::SetNextItemWidth(200);
    ime_textfield("##ivman", s_man, sizeof(s_man));
    ImGui::SameLine();
    if (ImGui::Button("Use##ivman") && s_man[0]) {
        bool used = false;
        for (auto& cc : s_classes)
            if (picking && cc.uid == s_arg_uid && s_arg_idx >= 0 &&
                s_arg_idx < 8) {
                snprintf(cc.call_args[s_arg_idx],
                         sizeof(cc.call_args[0]), "%s", s_man);
                used = true;
                break;
            }
        if (!used) c.of_note = std::string("manual: ") + s_man;
        if (picking) { s_arg_uid = -1; s_arg_idx = -1; s_arg_cls[0] = 0; }
        s_man[0] = 0;
    }
    ImGui::TextDisabled("tap [0x..] for an object tab; click a row to"
                        " select it");
    if (c.inst_wait)  ImGui::TextDisabled("(scanning the heap...)");
    if (c.inst_creat) ImGui::TextDisabled("(constructing...)");
    for (int i = 0; i < (int)c.instances.size(); i++) {
        ImGui::PushID(i);
        InstInfo& in = c.instances[i];
        bool sel = (c.sel_inst == i);
        char lbl[256];
        snprintf(lbl, sizeof(lbl), "#%d %s", i, in.str.c_str());
        if (ImGui::Selectable(lbl, sel))
            of_deliver(picking ? nullptr : &c, in.handle, in.str);
        ImGui::SameLine();
        char ob[48];
        snprintf(ob, sizeof(ob), "[0x%lx]##ot", in.handle);
        if (ImGui::SmallButton(ob))
            open_obj_tab(c.cname, in.handle, in.str);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save##ivsv")) {
            bool have = false;
            for (auto& sv : s_saved)
                if (sv.handle == in.handle) { have = true; break; }
            if (!have)
                s_saved.push_back({c.cname, in.str, in.handle});
        }
        ImGui::PopID();
    }
    if (c.instances.empty() && !c.inst_wait && !c.inst_creat)
        ImGui::TextDisabled("no instances yet - tap Find instances");
    if (c.sel_inst >= 0 && c.sel_inst < (int)c.instances.size()) {
        InstInfo& in = c.instances[c.sel_inst];
        ImGui::Separator();
        ImGui::Text("fields of #%d:", c.sel_inst);
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
                s_trackers.push_back({c.cname + "." + fi.name, true, {}});
            }
            ImGui::Text("  value:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            ime_textfield("##fv", nv[f], sizeof(nv[f]));
            ImGui::SameLine();
            if (ImGui::Button("Set##fs")) {
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
    /* r29: Saved Objects moved here from the Object Finder */
    if (ImGui::CollapsingHeader("Saved Objects")) {
        for (int k = 0; k < (int)s_saved.size(); ) {
            ImGui::PushID(9500 + k);
            SavedObj& sv = s_saved[k];
            char lbl2[280];
            snprintf(lbl2, sizeof(lbl2), "%s [0x%lx]",
                     short_name(sv.cls).c_str(), sv.handle);
            if (ImGui::Button(lbl2, ImVec2(
                    ImGui::GetContentRegionAvail().x - 70.f, 0)))
                of_deliver(picking ? nullptr : &c,
                           sv.handle, sv.str);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                s_saved.erase(s_saved.begin() + k);
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            k++;
        }
        if (s_saved.empty())
            ImGui::TextDisabled("Nothing...");
    }
    mark_scroll_item(s_lst_scroll);
    MULK();
}

static void static_fields_view(ClassTab& c) {
    MLK();
    if (ImGui::SmallButton("Read static values"))
        post_rpc("readStaticFields",
                 "{\"class\":\"" + jesc(c.cname) + "\"}");
    ImGui::SameLine();
    int nst = 0;
    for (auto& f : c.fields)
        if (f.value != "(instance)") nst++;
    ImGui::TextDisabled("%d static fields", nst);
    ImGui::Separator();
    for (int i = 0; i < (int)c.fields.size(); i++) {
        FieldInfo& f = c.fields[i];
        if (f.value == "(instance)") continue;
        ImGui::PushID(5000 + i);
        bool hitf = c.hit(c.hits_f, f.name);
        if (hitf) ImGui::PushStyleColor(ImGuiCol_Text,
                   ImVec4(0.25f, 0.95f, 0.35f, 1.f));
        ImGui::BulletText("%s : %s = %s", f.name.c_str(),
                          f.type.c_str(), f.value.c_str());
        if (hitf) ImGui::PopStyleColor();
        ImGui::SameLine();
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
static void fields_view(ClassTab& c) {
    MLK();
    ImGui::TextDisabled("%lu fields (static values: Static Fields tab)",
                        (unsigned long)c.fields.size());
    if (!ImGui::TreeNode("Fields##ftn")) { MULK(); return; }
    for (int i = 0; i < (int)c.fields.size(); i++) {
        FieldInfo& f = c.fields[i];
        ImGui::PushID(6000 + i);
        bool hitf = c.hit(c.hits_f, f.name);
        if (hitf) ImGui::PushStyleColor(ImGuiCol_Text,
                   ImVec4(0.25f, 0.95f, 0.35f, 1.f));
        ImGui::BulletText("%s : %s", f.name.c_str(), f.type.c_str());
        if (hitf) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Patch##fp2")) {
            snprintf(s_pcls, sizeof(s_pcls), "%s", c.cname.c_str());
            snprintf(s_pfld, sizeof(s_pfld), "%s", f.name.c_str());
            snprintf(s_pval, sizeof(s_pval), "%s", f.value.c_str());
            s_next_tab = "Patches";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Track##ftf2")) {
            std::string a = "{\"class\":\"" + jesc(c.cname)
                          + "\",\"field\":\"" + jesc(f.name) + "\"}";
            post_rpc("trackField", a);
            s_trackers.push_back({c.cname + "." + f.name, true, {}});
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
    MULK();
}
static void caller_tab(ClassTab& c, const MethodSig& m) {
    std::vector<std::string> ps = split_params(m.params);
    int na = (int)ps.size();
    if (na > 8) na = 8;
    for (int k = 0; k < na; k++) {
        ImGui::PushID(2000 + k);
        bool prim = is_prim_type(ps[k]);
        char lb[96];
        snprintf(lb, sizeof(lb), "arg%d (%s)", k, ps[k].c_str());
        if (prim) {
            ImGui::SetNextItemWidth(210);
            ime_textfield(lb, c.call_args[k], sizeof(c.call_args[k]));
        } else {
            ImGui::Text("%s", lb);
            ImGui::SetNextItemWidth(-52.f);
            std::string prev = c.call_args[k][0] ?
                std::string(c.call_args[k]) : "(pick instance)";
            if (ImGui::BeginCombo("##co", prev.c_str())) {
                if (ImGui::Selectable(
                        "Find / Create Object (Inspector)...")) {
                    /* r29: pick in the Inspector's Instances sub-tab */
                    snprintf(s_arg_typ, sizeof(s_arg_typ), "%s",
                             ps[k].c_str());
                    snprintf(s_arg_cls, sizeof(s_arg_cls), "%s",
                             ps[k].c_str());
                    s_arg_uid = c.uid;
                    s_arg_idx = k;
                    ClassTab* ct = open_inspector(ps[k], true);
                    if (ct) ct->sub = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (s_pscan.uid != c.uid || s_pscan.arg != k ||
                    s_pscan.cls != ps[k]) {
                    s_pscan.uid = c.uid;
                    s_pscan.arg = k;
                    s_pscan.cls = ps[k];
                    s_pscan.list.clear();
                    s_pscan.waiting = true;
                    post_rpc("listInstances",
                             "{\"class\":\"" + jesc(ps[k]) + "\"}");
                }
                if (s_pscan.waiting)
                    ImGui::TextDisabled("(scanning instances...)");
                int pn = (int)s_pscan.list.size();
                for (int q = 0; q < pn; q++) {
                    ImGui::PushID(q);
                    char pl[220];
                    snprintf(pl, sizeof(pl), "%s (%d/%d)",
                             short_name(ps[k]).c_str(), q, pn);
                    if (ImGui::Selectable(pl)) {
                        char tok[40];
                        snprintf(tok, sizeof(tok), "@handle:%ld",
                                 s_pscan.list[q].handle);
                        snprintf(c.call_args[k], sizeof(c.call_args[0]),
                                 "%s", tok);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::TextDisabled("    %s",
                                        s_pscan.list[q].str.c_str());
                    ImGui::PopID();
                }
                if (pn == 0 && !s_pscan.waiting)
                    ImGui::TextDisabled("no instances - use Find / Create");
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-52.f);
            ime_textfield("##cov", c.call_args[k], sizeof(c.call_args[k]));
        }
        ImGui::PopID();
    }
    if (c.sel_inst >= 0 && c.sel_inst < (int)c.instances.size())
        ImGui::TextDisabled("receiver: instance #%d", c.sel_inst);
    else
        ImGui::TextDisabled("instance methods need a receiver - pick "
                            "one on the Instances tab above");
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Call Count", &c.call_count)) {
        if (c.call_count < 1)    c.call_count = 1;
        if (c.call_count > 50)   c.call_count = 50;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("how many times to call with these args; "
                          "every call is logged below");
    if (ImGui::Button("Call")) {
        long h = (c.sel_inst >= 0 && c.sel_inst < (int)c.instances.size())
                     ? c.instances[c.sel_inst].handle : -1;
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"handle\":" + std::to_string(h)
                      + ",\"args\":[";
        for (int k = 0; k < na; k++) {
            if (k) a += ",";
            std::string v = c.call_args[k];
            if (v.rfind("@handle:", 0) == 0) {
                long oh = strtol(v.c_str() + 8, nullptr, 10);
                if (oh > 0) a += "{\"handle\":" + std::to_string(oh) + "}";
                else        a += "\"" + jesc(v) + "\"";
            } else {
                a += "\"" + jesc(v) + "\"";
            }
        }
        a += "],\"count\":" + std::to_string(c.call_count)
           + ",\"main\":false}";
        post_rpc("callMethod", a);
        char note[80];
        snprintf(note, sizeof(note), "sent %d call(s)...", c.call_count);
        c.call_log.push_back(note);
    }
    ImGui::SameLine();
    if (ImGui::Button("Call in Main Thread")) {
        long h = (c.sel_inst >= 0 && c.sel_inst < (int)c.instances.size())
                     ? c.instances[c.sel_inst].handle : -1;
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"handle\":" + std::to_string(h)
                      + ",\"args\":[";
        for (int k = 0; k < na; k++) {
            if (k) a += ",";
            std::string v = c.call_args[k];
            if (v.rfind("@handle:", 0) == 0) {
                long oh = strtol(v.c_str() + 8, nullptr, 10);
                if (oh > 0) a += "{\"handle\":" + std::to_string(oh) + "}";
                else        a += "\"" + jesc(v) + "\"";
            } else {
                a += "\"" + jesc(v) + "\"";
            }
        }
        a += "],\"count\":" + std::to_string(c.call_count)
           + ",\"main\":true}";
        post_rpc("callMethod", a);
        char note[96];
        snprintf(note, sizeof(note), "sent %d call(s) on the main thread...",
                 c.call_count);
        c.call_log.push_back(note);
    }
    if (!c.call_log.empty()) {
        ImGui::TextDisabled("Call Results:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##clg")) c.call_log.clear();
        ImGui::BeginChild("calllog", ImVec2(0, fit_h(.30f, 90)), true);
        for (auto& l : c.call_log)
            ImGui::TextWrapped("%s", l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
            ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
    }
}
static void patcher_tab(ClassTab& c, const MethodSig& m) {
    ImGui::TextDisabled("Patch NOP: skip the body (void methods return "
                        "immediately; others return the default for "
                        "their type). Redirect: force the return value "
                        "below on every subsequent call.");
    ImGui::SetNextItemWidth(220);
    ime_textfield("Manual Input##pv26", c.patch_val, sizeof(c.patch_val));
    if (ImGui::Button("Patch NOP")) {
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"mode\":\"noop\",\"code\":\"\"}";
        post_rpc("addHook", a);
        s_hooks.push_back({c.cname, m.name, 2, "", true, "..."});
        c.patch_note = "NOP armed on " + m.name;
    }
    ImGui::SameLine();
    if (ImGui::Button("Redirect") && c.patch_val[0]) {
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"args\":[],\"ret\":{\"on\":true,\"v\":\""
                      + jesc(c.patch_val) + "\"}}";
        post_rpc("setMethodIO", a);
        c.patch_note = "redirect armed -> " + std::string(c.patch_val);
    }
    if (!c.patch_val[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("(enter a value first)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop redirect")) {
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"args\":[],\"ret\":{\"on\":false,\"v\":\"\"}}";
        post_rpc("setMethodIO", a);
        c.patch_note = "redirect removed";
    }
    if (!c.patch_note.empty())
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", c.patch_note.c_str());
    if (ImGui::CollapsingHeader("Live IO patch (probe)##ioh")) {
        if (!c.io_open) {
            if (ImGui::SmallButton("Probe next call values")) {
                c.io_sig    = m;
                c.io_wait   = true;
                c.io_nargs  = 0;
                c.io_result = "waiting for the next call...";
                std::string a = "{\"class\":\"" + jesc(c.cname)
                              + "\",\"method\":\"" + jesc(m.name) + "\"}";
                post_rpc("getMethodIO", a);
            }
        } else {
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
        }
    }
}
static void trace_tab(ClassTab& c, const MethodSig& m) {
    std::string tname = c.cname + "." + m.name;
    long hits = 0;
    auto hi = s_hits.find(tname);
    if (hi != s_hits.end()) hits = hi->second;
    if (hits > 0)
        ImGui::Text("Hit Count %ld", hits);
    int ti = find_tracker(tname);
    if (ti < 0) {
        if (ImGui::Button("Track this method")) {
            std::string a = "{\"class\":\"" + jesc(c.cname)
                          + "\",\"method\":\"" + jesc(m.name) + "\"}";
            post_rpc("addTracker", a);
            s_trackers.push_back({tname, false, {}});
        }
        ImGui::SameLine();
        ImGui::TextDisabled("not tracking yet");
    } else {
        Tracker& t = s_trackers[ti];
        ImGui::TextDisabled("tracking - %lu events (full log: Tracer tab)",
                            (unsigned long)t.log.size());
        ImGui::BeginChild("texp", ImVec2(0, fit_h(.35f, 100)), true);
        for (auto& r : t.log) {
            for (auto& cl : r.callers) {
                if (ImGui::SmallButton(("@ " + cl).c_str()))
                    menu_open_class(cls_of_member(cl));
            }
            ImGui::BulletText("args=%s ret=%s%s%s",
                              r.args.c_str(), r.ret.c_str(),
                              r.err.empty() ? "" : " ERR=",
                              r.err.c_str());
            ImGui::Separator();
        }
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
        if (ImGui::SmallButton("Remove tracker")) {
            std::string a = "{\"name\":\"" + jesc(tname) + "\"}";
            post_rpc("removeTracker", a);
            s_trackers.erase(s_trackers.begin() + ti);
        }
    }
    ImGui::Separator();
    static const char* modes[] = {"Java(JS)", "Smali", "Nop (auto)"};
    ImGui::Combo("Mode##hx28", &c.hook_mode, modes, 3);
    ime_multiline("Code##hx28", c.hook_code, sizeof(c.hook_code),
                  ImVec2(-1, 90));
    ImGui::TextDisabled("Java: fn(Java,send,console,thiz,args,orig). "
                        "Smali: body of static hook(Object thiz, Object[] args); "
                        "p0=thiz p1=args, must return an Object.");
    if (ImGui::Button("Add Hook")) {
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name)
                      + "\",\"mode\":\"" + modes[c.hook_mode]
                      + "\",\"code\":\"" + jesc(c.hook_code) + "\"}";
        post_rpc("addHook", a);
        s_hooks.push_back({c.cname, m.name, c.hook_mode, c.hook_code,
                           true, "..."});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove hook")) {
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name) + "\"}";
        post_rpc("removeHook", a);
        for (int k = (int)s_hooks.size() - 1; k >= 0; k--) {
            if (s_hooks[k].cls == c.cname && s_hooks[k].method == m.name) {
                s_hooks.erase(s_hooks.begin() + k);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Method source")) {
        snprintf(s_hcls, sizeof(s_hcls), "%s", c.cname.c_str());
        snprintf(s_hmth, sizeof(s_hmth), "%s", m.name.c_str());
        std::string a = "{\"class\":\"" + jesc(c.cname)
                      + "\",\"method\":\"" + jesc(m.name) + "\"}";
        post_rpc("getMethodSrc", a);
        snprintf(s_hsrc_note, sizeof(s_hsrc_note), "disassembling...");
        s_hsrc_have = false;
    }
    if (s_hsrc_have && c.cname == s_hcls && m.name == s_hmth) {
        ImGui::BeginChild("txsrc", ImVec2(0, fit_h(.40f, 110)), true);
        ImGui::TextUnformatted(s_hsrc);
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
        if (c.hook_mode == 1 &&
            ImGui::SmallButton("Copy body -> editor##tx")) {
            std::string src = s_hsrc;
            auto b = src.find('\n');
            if (b != std::string::npos) src = src.substr(b + 1);
            auto e = src.rfind(".end method");
            if (e != std::string::npos) src = src.substr(0, e);
            snprintf(c.hook_code, sizeof(c.hook_code), "%s", src.c_str());
        }
    } else if (!s_hsrc_have && c.cname == s_hcls && m.name == s_hmth) {
        ImGui::TextDisabled("%s", s_hsrc_note);
    }
}
static void method_body(ClassTab& c, const MethodSig& m) {
    ImGui::Indent();
    if (ImGui::BeginTabBar("mexp")) {
        static const char* msubs[] = {"Call", "Patch", "Trace"};
        char tlbl[40];
        for (int t = 0; t < 3; t++) {
            snprintf(tlbl, sizeof(tlbl), "%s##mexp%d", msubs[t], t);
            if (ImGui::BeginTabItem(tlbl)) {
                c.msub = t;
                if      (t == 0) caller_tab(c, m);
                else if (t == 1) patcher_tab(c, m);
                else             trace_tab(c, m);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Unindent();
}
static void methods_view(ClassTab& c) {
    MLK();
    ImGui::BeginChild("clsm", ImVec2(0, 0), true);
    ImGuiListClipper clip;
    clip.Begin((int)c.methods.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            MethodSig& m = c.methods[i];
            ImGui::PushID(i);
            bool hitm = c.hit(c.hits_m, m.name);
            std::string key = c.cname + "." + m.name;
            long hits = 0;
            auto hi = s_hits.find(key);
            if (hi != s_hits.end()) hits = hi->second;
            bool hooked = false;
            for (auto& h : s_hooks) {
                if (h.cls == c.cname && h.method == m.name) {
                    hooked = true;
                    break;
                }
            }
            int pushed = 0;
            if (hooked || hits > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      IM_COL32(25, 255, 125, 255));
                pushed++;
            } else if (hitm) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.25f, 0.95f, 0.35f, 1.f));
                pushed++;
            }
            char pre[64] = "";
            if (hits > 0)
                snprintf(pre, sizeof(pre), "Hit Count %ld | ", hits);
            char lbl[600];
            snprintf(lbl, sizeof(lbl), "%s%s(%s) -> %s###m%d", pre,
                     m.name.c_str(), m.params.c_str(), m.ret.c_str(), i);
            ImGui::SetNextItemOpen(c.expanded == i, ImGuiCond_Always);
            bool open = ImGui::TreeNodeEx(lbl,
                        ImGuiTreeNodeFlags_SpanAvailWidth);
            if (open) {
                if (c.expanded != i) {
                    c.expanded = i;
                    c.msub = 0;
                    if (c.call_sig.name != m.name ||
                        c.call_sig.params != m.params) {
                        c.call_sig = m;
                        c.call_nargs = 0;
                        c.call_count = 1;
                        c.call_log.clear();
                        for (int k = 0; k < 8; k++) c.call_args[k][0] = 0;
                    }
                }
                if (pushed) { ImGui::PopStyleColor(pushed); pushed = 0; }
                bool rclk = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                static bool mctx_wheel_press = false;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    mctx_wheel_press = false;
                ImGuiIO& pio = ImGui::GetIO();
                if (pio.MouseWheel != 0.f || pio.MouseWheelH != 0.f)
                    mctx_wheel_press = true;
                bool lng = ImGui::IsItemHovered() &&
                           ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                           !mctx_wheel_press &&
                           !ImGui::IsMouseDragging(ImGuiMouseButton_Left,
                                                   10.f) &&
                           pio.MouseDownDuration[ImGuiMouseButton_Left] >
                               0.55f;
                float bx = ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x - 26.f;
                ImGui::SameLine(bx);
                bool dot = ImGui::SmallButton("...");
                if (rclk || dot || lng) ImGui::OpenPopup("mctx");
                if (ImGui::BeginPopup("mctx")) {
                    if (ImGui::MenuItem("Open in new tab")) {
                        s_dup_req = c.cname;
                    }
                    if (ImGui::MenuItem("Full Method View")) {
                        s_fm_cls  = c.cname;
                        s_fm_mth  = m.name;
                        s_fm_open = true;
                        s_fm_have = false;
                        snprintf(s_fm_note, sizeof(s_fm_note),
                                 "disassembling...");
                        std::string a = "{\"class\":\"" + jesc(c.cname)
                                      + "\",\"method\":\"" + jesc(m.name)
                                      + "\"}";
                        post_rpc("getMethodSrc", a);
                    }
                    if (ImGui::MenuItem("Copy Method Name"))
                        copy_text("method", m.name);
                    if (ImGui::MenuItem("Copy Class Name"))
                        copy_text("class", c.cname);
                    ImGui::EndPopup();
                }
                method_body(c, m);
                ImGui::TreePop();
            } else if (c.expanded == i) {
                c.expanded = -1;
            }
            if (pushed) ImGui::PopStyleColor(pushed);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    MULK();
}
static void obj_tab_body(ObjTab& t) {
    MLK();
    char ttl[80];
    snprintf(ttl, sizeof(ttl), "[0x%lx]", t.handle);
    ImGui::TextDisabled("%s", ttl);
    ImGui::SameLine();
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(t.cls.c_str());
    ImGui::PopTextWrapPos();
    if (!t.str.empty())
        ImGui::TextDisabled("%s", t.str.c_str());
    if (ImGui::SmallButton("Refresh##ot")) {
        std::string a = "{\"class\":\"" + jesc(t.cls) + "\"}";
        post_rpc("listInstances", a);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Class##ot"))
        open_inspector(t.cls, true);
    ImGui::SameLine();
    if (ImGui::SmallButton("Methods##ot"))
        ImGui::OpenPopup("ObjMethodsPopup");
    ImGui::SameLine();
    if (ImGui::SmallButton("Dump to file##ot"))
        ImGui::OpenPopup("ObjDumpPopup");
    if (ImGui::BeginPopup("ObjMethodsPopup",
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const char* nm = t.cls.c_str();
        float ww = ImGui::GetWindowSize().x;
        float tw = ImGui::CalcTextSize(nm).x;
        ImGui::SetCursorPosX((ww - tw) * 0.5f);
        ImGui::Text("%s", nm);
        ImGui::Separator();
        ClassTab* c = find_tab(t.cls);
        if (!c || c->methods.empty()) {
            ImGui::TextDisabled("open the class tab once to load its "
                                "method list");
        } else {
            int j = 0;
            for (auto& m : c->methods) {
                ImGui::PushID(9600 + j++);
                char lbl[512];
                snprintf(lbl, sizeof(lbl), "%s(%s) -> %s",
                         m.name.c_str(), m.params.c_str(),
                         m.ret.c_str());
                if (ImGui::Button(lbl,
                                  ImVec2(ImGui::GetContentRegionAvail().x,
                                         0))) {
                    for (int q = 0; q < (int)c->methods.size(); q++) {
                        if (c->methods[q].name == m.name) {
                            c->expanded = q;
                            c->msub = 0;
                            break;
                        }
                    }
                    open_inspector(t.cls, true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("ObjDumpPopup")) {
        char fn[128];
        snprintf(fn, sizeof(fn), "dextool-object-0x%lx.json", t.handle);
        ImGui::Text("File will be saved as %s", fn);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 50, 255));
        ImGui::Text("Note: This may take a while depending on the size "
                    "of the object");
        ImGui::Text("Do not touch the screen if it's freezing!");
        ImGui::PopStyleColor();
        if (ImGui::Button("Proceed",
                          ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            std::string a = "{\"class\":\"" + jesc(t.cls)
                          + "\",\"handle\":" + std::to_string(t.handle)
                          + "}";
            post_rpc("dumpObject", a);
            snprintf(s_dobj_note, sizeof(s_dobj_note), "writing...");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (s_dobj_note[0])
        ImGui::TextDisabled("%s", s_dobj_note);
    ImGui::Separator();
    if (t.sel >= (int)t.fields.size()) t.sel = -1;
    for (int f = 0; f < (int)t.fields.size(); f++) {
        FieldInfo& fi = t.fields[f];
        ImGui::PushID(9000 + f);
        if (t.sel != f) {
            ImGui::BulletText("%s (%s) = %s", fi.name.c_str(),
                              fi.type.c_str(), fi.value.c_str());
            if (ImGui::IsItemClicked()) {
                t.sel = f;
                snprintf(t.edit, sizeof(t.edit), "%s",
                         fi.value.c_str());
            }
        } else {
            ImGui::BulletText("%s (%s)", fi.name.c_str(),
                              fi.type.c_str());
            ImGui::SetNextItemWidth(200);
            ime_textfield("##ov", t.edit, sizeof(t.edit));
            ImGui::SameLine();
            if (ImGui::Button("Set##os")) {
                std::string a = "{\"class\":\"" + jesc(t.cls)
                              + "\",\"handle\":"
                              + std::to_string(t.handle)
                              + ",\"index\":0"
                              + ",\"field\":\"" + jesc(fi.name)
                              + "\",\"value\":\"" + jesc(t.edit) + "\"}";
                post_rpc("setInstanceField", a);
                t.sel = -1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##oc")) t.sel = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Track##otf")) {
            std::string a = "{\"class\":\"" + jesc(t.cls)
                          + "\",\"field\":\"" + jesc(fi.name) + "\"}";
            post_rpc("trackField", a);
            s_trackers.push_back({t.cls + "." + fi.name, true, {}});
        }
        ImGui::PopID();
    }
    if (t.fields.empty())
        ImGui::TextDisabled("no fields yet - open the class tab once, "
                            "then Refresh");
    MULK();
}
static void open_obj_tab(const std::string& cls, long handle,
                         const std::string& str) {
    MLK();
    for (auto& t : s_objtabs) {
        if (t.handle == handle) {
            s_objfocus = handle;
            if (!str.empty()) t.str = str;
            MULK();
            return;
        }
    }
    ObjTab t;
    t.handle = handle;
    t.cls    = cls;
    t.str    = str;
    for (auto& c : s_classes) {
        if (c.cname != cls) continue;
        for (auto& in : c.instances) {
            if (in.handle == handle) {
                t.fields = in.fields;
                if (t.str.empty()) t.str = in.str;
                break;
            }
        }
        break;
    }
    s_objtabs.push_back(t);
    s_objfocus = handle;
    MULK();
}
static void of_deliver(ClassTab* dst, long handle, const std::string& str) {
    /* r29: while a method-arg pick is active the value goes to the
       arg owner (s_arg_uid), otherwise to the listed class tab */
    MLK();
    int  uid  = s_arg_uid;
    int  argn = s_arg_idx;
    s_arg_uid = -1;
    s_arg_idx = -1;
    s_arg_cls[0] = 0;
    if (uid >= 0) {
        for (auto& c : s_classes)
            if (c.uid == uid) {
                char tok[40];
                snprintf(tok, sizeof(tok), "@handle:%ld", handle);
                if (argn >= 0 && argn < 8)
                    snprintf(c.call_args[argn],
                             sizeof(c.call_args[0]), "%s", tok);
                break;
            }
    } else if (dst) {
        dst->of_handle = handle;
        std::string t = str;
        if (t.size() > 120) t = t.substr(0, 117) + "...";
        dst->of_note = "selected: " + t;
        for (int k = 0; k < (int)dst->instances.size(); k++)
            if (dst->instances[k].handle == handle) {
                dst->sel_inst = k;
                break;
            }
    }
    MULK();
}

static void families_window() {
    /* r29: the family dropdown became a movable/resizable window */
    if (!s_fam_open) return;
    MLK();
    ExplorerWin* e = nullptr;
    for (auto& x : s_explorers)
        if (x.id == s_fam_uid) { e = &x; break; }
    if (!e) { MULK(); s_fam_open = false; return; }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(240, 180),
        ImVec2(io.DisplaySize.x * .9f, io.DisplaySize.y * .9f));
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * .3f, io.DisplaySize.y * .25f),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Class Families", &s_fam_open)) {
        ImGui::End();
        MULK();
        return;
    }
    std::vector<const char*> famc;
    for (auto& f : s_families) famc.push_back(f.c_str());
    int nf = (int)famc.size();
    if (e->family >= nf || e->family < 0) e->family = 0;
    ime_textfield("Search families...##fsearch",
                  e->fam_search, sizeof(e->fam_search));
    ImGui::SameLine();
    if (ImGui::SmallButton("All##fwall")) e->all_fam = true;
    ImGui::Separator();
    for (int f = 0; f < nf; f++) {
        if (!contains_ci(s_families[f], e->fam_search))
            continue;
        bool sel = (!e->all_fam && e->family == f);
        if (ImGui::Selectable(famc[f], sel)) {
            e->family  = f;
            e->all_fam = false;
        }
    }
    if (nf == 0)
        ImGui::TextDisabled("no families yet - run a class search");
    mark_scroll_item(s_lst_scroll);
    MULK();
    ImGui::End();
}

static void fm_window() {
    if (!s_fm_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x * .62f, io.DisplaySize.y * .7f),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(320, 220),
        ImVec2(io.DisplaySize.x * .95f, io.DisplaySize.y * .95f));
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .4f),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Full Method View", &s_fm_open,
                      ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }
    MLK();   /* s_fm_* are written by the GumJS callback thread too */
    ImGui::Text("%s.%s", s_fm_cls.c_str(), s_fm_mth.c_str());
    if (ImGui::SmallButton("Load source")) {
        std::string a = "{\"class\":\"" + jesc(s_fm_cls)
                      + "\",\"method\":\"" + jesc(s_fm_mth) + "\"}";
        post_rpc("getMethodSrc", a);
        snprintf(s_fm_note, sizeof(s_fm_note), "disassembling...");
        s_fm_have = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy source") && s_fm_have)
        copy_text("source", s_fm_src);
    ImGui::SameLine();
    if (ImGui::SmallButton("Close##fmx")) s_fm_open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s_fm_note);
    if (s_fm_have) {
        ImGui::BeginChild("fmsrc", ImVec2(0, 0), true);
        ImGui::TextUnformatted(s_fm_src);
        ImGui::EndChild();
        mark_scroll_item(s_lst_scroll);
    }
    MULK();
    ImGui::End();
}
static void explorer_body(ExplorerWin& e) {
    bool do_search = false;
    if (s_families.empty() && !s_search_res.empty() && !s_search_wait) {
        for (auto& n : s_search_res) {
            std::string f = family_of(n);
            if (!f.empty() &&
                std::find(s_families.begin(), s_families.end(), f)
                    == s_families.end())
                s_families.push_back(f);
        }
        std::sort(s_families.begin(), s_families.end());
    }
    std::vector<const char*> famc;
    for (auto& f : s_families) famc.push_back(f.c_str());
    int nf = (int)famc.size();
    if (e.family >= nf || e.family < 0) e.family = 0;
    /* r29: the family picker opens the movable Class Families window */
    ImGui::SetNextItemWidth(
        ImGui::GetContentRegionAvail().x - 90.f);
    const char* fprev = nf ? famc[e.family]
                            : "(no classes yet)";
    char famlbl[180];
    snprintf(famlbl, sizeof(famlbl), "%s##fambtn",
             e.all_fam ? "(All families)" : fprev);
    if (ImGui::Button(famlbl)) {
        s_fam_uid = e.id;
        s_fam_open = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("All", &e.all_fam);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 84.f);
    do_search |= ime_textfield("##esearch", e.search,
                               sizeof(e.search),
                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Filter")) do_search = true;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(0.94f, 0.44f, 0.66f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.98f, 0.55f, 0.74f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.88f, 0.34f, 0.58f, 1.f));
    if (ImGui::Button("Options")) ImGui::OpenPopup("explOpts");
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (s_search_wait) ImGui::TextDisabled("(listing...)");
    else ImGui::TextDisabled("%ld of %ld loaded classes",
                             (long)s_search_shown,
                             (long)s_search_total);
    if (ImGui::BeginPopup("explOpts")) {
        ImGui::Checkbox("Case sensitive", &e.case_sensitive);
        ImGui::TextDisabled("Filter by:");
        int fb = e.advanced ? 3 : e.filter_by;
        if (ImGui::RadioButton("Class",  fb == 0)) {
            e.filter_by = 0; e.advanced = false; }
        if (ImGui::RadioButton("Method", fb == 1)) {
            e.filter_by = 1; e.advanced = false; }
        if (ImGui::RadioButton("Field",  fb == 2)) {
            e.filter_by = 2; e.advanced = false; }
        if (ImGui::RadioButton(
                "Code text (advanced: string inside class code)",
                fb == 3))
            e.advanced = true;
        bool sys = s_search_sys;
        if (ImGui::Checkbox("System classes", &sys))
            s_search_sys = sys;
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::BeginChild("explst", ImVec2(0, 0), true);
    ImGuiListClipper clip;
    clip.Begin((int)s_search_res.size());
    while (clip.Step()) {
        for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
            const std::string& nm = s_search_res[k];
            if (!e.all_fam && e.family < (int)s_families.size() &&
                family_of(nm) != s_families[e.family]) continue;
            ImGui::PushID(k);
            if (ImGui::Selectable(nm.c_str()))
                open_inspector(nm, false);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    mark_scroll_item(s_lst_scroll);
    if (do_search)
        run_search(e.search, e.advanced ? 3 : e.filter_by,
                   e.case_sensitive);
}
static void class_tab_body(ClassTab& c) {
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(c.cname.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Refresh##ct"))
        open_inspector(c.cname, false);
    ImGui::SameLine();
    if (c.of_handle >= 0)
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s",
                           c.of_note.c_str());
    if (!c.got_info)
        ImGui::TextDisabled(c.waiting ? "(waiting for agent...)"
                                      : "(no data - tap Refresh)");
    ImGui::Separator();
    static const char* subs[] = {"Instances", "Static Fields",
                                 "Methods", "Fields"};
    char sl[40];
    if (ImGui::BeginTabBar("clssub")) {
        for (int si = 0; si < 4; si++) {
            snprintf(sl, sizeof(sl), "%s##cs%d", subs[si], si);
            if (ImGui::BeginTabItem(sl)) {
                c.sub = si;
                if      (si == 0) inst_view(c);
                else if (si == 1) static_fields_view(c);
                else if (si == 2) methods_view(c);
                else              fields_view(c);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}
static void tab_tools() {
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
    bool want_expl = s_explorers.empty();
    MULK();
    if (want_expl) {
        ExplorerWin e{};
        e.id = ++s_expl_seq;
        MLK();
        s_explorers.push_back(e);
        MULK();
    }
    if (qid)
        post_rpc("listLoadedClasses",
                 "{\"filter\":\"\",\"qid\":" + std::to_string(qid)
                 + ",\"mode\":0"
                 + ",\"sys\":" + (s_search_sys ? "true" : "false") + "}");
    if (ImGui::Checkbox("Fullscreen", &s_fullscreen)) {
        if (s_fullscreen) {
            s_fs_last_pos  = ImGui::GetWindowPos();
            s_fs_last_size = ImGui::GetWindowSize();
        } else {
            s_fs_restore = true;
        }
        s_conf_dirty = true;
    }
    MLK();
    if (ImGui::BeginTabBar("tabber",
                           ImGuiTabBarFlags_AutoSelectNewTabs |
                           ImGuiTabBarFlags_FittingPolicyScroll |
                           ImGuiTabBarFlags_TabListPopupButton)) {
        if (ImGui::TabItemButton("+",
                                 ImGuiTabItemFlags_NoTooltip |
                                 ImGuiTabItemFlags_Leading))
            s_expl_req = true;
        if (s_expl_req) {
            s_expl_req = false;
            if (s_explorers.size() < 6) {
                ExplorerWin e{};
                e.id = ++s_expl_seq;
                s_explorers.push_back(e);
                s_focus_expl = e.id;
            }
        }
        int exi = 0;
        for (int i = 0; i < (int)s_explorers.size(); ) {
            ExplorerWin& e = s_explorers[i];
            bool open = true;
            char lbl[192];
            if (e.search[0])
                snprintf(lbl, sizeof(lbl), "%s", e.search);
            else if (exi == 0)
                snprintf(lbl, sizeof(lbl), "Classes");
            else
                snprintf(lbl, sizeof(lbl), "Classes [%d]", e.id);
            ImGuiTabItemFlags fl = 0;
            if (s_focus_expl == e.id) {
                fl |= ImGuiTabItemFlags_SetSelected;
                s_focus_expl = 0;
            }
            if (ImGui::BeginTabItem(lbl, &open, fl)) {
                explorer_body(e);
                ImGui::EndTabItem();
            }
            if (!open) {
                s_explorers.erase(s_explorers.begin() + i);
                continue;   /* no i++ - the next tab shifted in */
            }
            exi++;
            i++;
        }
        if (s_explorers.empty()) {
            ExplorerWin e{};
            e.id = ++s_expl_seq;
            s_explorers.push_back(e);
        }
        for (int i = 0; i < (int)s_classes.size(); ) {
            ClassTab& c = s_classes[i];
            bool open = true;
            std::string lbl = win_title(c);
            ImGuiTabItemFlags fl = 0;
            if (s_focus_win == lbl) {
                fl |= ImGuiTabItemFlags_SetSelected;
                s_focus_win.clear();
            }
            if (ImGui::BeginTabItem(lbl.c_str(), &open, fl)) {
                class_tab_body(c);
                ImGui::EndTabItem();
            }
            if (!open) {
                s_classes.erase(s_classes.begin() + i);
                continue;
            }
            i++;
        }
        if (!s_dup_req.empty()) {
            ClassTab nt{};
            nt.cname   = s_dup_req;
            nt.uid     = ++s_tab_uid_seq;
            nt.waiting = true;
            s_classes.push_back(nt);
            s_focus_win = win_title(s_classes.back());
            std::string a = "{\"class\":\"" + jesc(s_dup_req) + "\"}";
            post_rpc("openClass", a);
            s_dup_req.clear();
        }
        for (int i = 0; i < (int)s_objtabs.size(); ) {
            ObjTab& t = s_objtabs[i];
            bool open = true;
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "[0x%lx]##ot%ld",
                     t.handle, t.handle);
            ImGuiTabItemFlags fl = 0;
            if (s_objfocus == t.handle) {
                fl |= ImGuiTabItemFlags_SetSelected;
                s_objfocus = -1;
            }
            if (ImGui::BeginTabItem(lbl, &open, fl)) {
                obj_tab_body(t);
                ImGui::EndTabItem();
            }
            if (!open) {
                /* r31 M7: free the retained instance - it used to
                   stay pinned until the 200-entry LRU evicted it */
                std::string a = "{\"handle\":" +
                                std::to_string(t.handle) + "}";
                post_rpc("releaseHandle", a);
                s_objtabs.erase(s_objtabs.begin() + i);
                continue;
            }
            i++;
        }
        ImGui::EndTabBar();
    }
    MULK();
}
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
    /* r29: a fast tap completes within one frame - handle the press
       directly (the empty handler needed a second tap before) */
    bool fab_press = ImGui::Button(s_visible ? "HIDE" : "MENU",
                                   ImVec2(64, 44));
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
        if (!s_fab_moved && !fab_press) {
            /* tap released just off the button: still a tap */
            s_visible = !s_visible;
            s_visible_pub.store(s_visible);
            s_conf_dirty = true;
        }
    }
    if (fab_press && !s_fab_moved) {  /* r29: single tap toggles */
        s_visible = !s_visible;
        s_visible_pub.store(s_visible);
        s_conf_dirty = true;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
static void draw_resize_handles() {
    /* r29: touch-friendly resize - drag the left/right/bottom edges
       (12px strips, highlighted while touched); the top bar stays
       the move handle */
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    ImGuiIO& io = ImGui::GetIO();
    const float T  = 12.f;
    const float th = ImGui::GetFrameHeight();
    static int   s_rz = -1;          /* 0 left 1 right 2 bottom */
    static ImVec2 m0(0, 0), p0(0, 0), s0(0, 0);
    struct RzDef { const char* id; ImVec2 o, sz; };
    RzDef defs[3] = {
        { "##rzl", ImVec2(0, th),        ImVec2(T, ws.y - th - T) },
        { "##rzr", ImVec2(ws.x - T, th), ImVec2(T, ws.y - th - T) },
        { "##rzb", ImVec2(0, ws.y - T),  ImVec2(ws.x, T) },
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any_active = false;
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(
            ImVec2(wp.x + defs[i].o.x, wp.y + defs[i].o.y));
        ImGui::InvisibleButton(defs[i].id, defs[i].sz);
        bool hov = ImGui::IsItemHovered();
        bool act = ImGui::IsItemActive();
        if (hov || act)
            dl->AddRectFilled(
                ImVec2(wp.x + defs[i].o.x, wp.y + defs[i].o.y),
                ImVec2(wp.x + defs[i].o.x + defs[i].sz.x,
                       wp.y + defs[i].o.y + defs[i].sz.y),
                act ? IM_COL32(120, 200, 255, 90)
                    : IM_COL32(160, 160, 160, 45));
        if (act) {
            any_active = true;
            if (s_rz != i) {
                s_rz = i;
                m0 = io.MousePos;
                p0 = wp;
                s0 = ws;
            }
        }
    }
    if (any_active && s_rz >= 0) {
        float dx = io.MousePos.x - m0.x;
        float dy = io.MousePos.y - m0.y;
        ImVec2 np = p0, ns = s0;
        if (s_rz == 0) {
            float right = p0.x + s0.x;
            np.x = p0.x + dx;
            if (np.x < 0) np.x = 0;
            if (np.x > right - 420.f) np.x = right - 420.f;
            ns.x = right - np.x;
        } else if (s_rz == 1) {
            ns.x = s0.x + dx;
        } else {
            ns.y = s0.y + dy;
        }
        if (ns.x > io.DisplaySize.x * .95f)
            ns.x = io.DisplaySize.x * .95f;
        if (ns.y > io.DisplaySize.y * .95f)
            ns.y = io.DisplaySize.y * .95f;
        if (np.y + ns.y > io.DisplaySize.y)
            ns.y = io.DisplaySize.y - np.y;
        ImGui::SetWindowPos(np);
        ImGui::SetWindowSize(ns);
    } else if (!any_active) {
        s_rz = -1;
    }
}
int menu_frame_pump_level() {
    /* r31 M8: no mutex - s_visible is mirrored into an atomic, so
       the JS thread's frame pump can no longer stall behind a
       whole-frame render lock */
    if (s_visible_pub.load(std::memory_order_relaxed)) return 2;
    return overlay_ready() ? 1 : 0;       /* FAB-only slow pump  */
}
void menu_draw() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    apply_theme();
    bool vis = s_visible_pub.load(std::memory_order_relaxed);  /* r31 M8 */
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
        overlay_publish_scroll_regions(s_regions);
        s_regions.clear();
        return;
    }
    draw_fab(ds);
    ime_frame_prelude();
    MLK();   /* r30 C1: s_flash is pushed from the JS thread */
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float dt = io.DeltaTime;
        float lh = ImGui::CalcTextSize("X").y;
        if (lh <= 0.f) lh = 18.f;
        int maxline = (int)((ds.y - 150.f) / lh) - 5;
        if (maxline > 10) maxline = 10;
        int i = 0;
        for (int k = 0; k < (int)s_flash.size(); ) {
            HitFlash& f = s_flash[k];
            f.time -= dt;
            f.gone -= dt;
            if (f.gone <= 0.f || f.name.empty()) {
                s_flash.erase(s_flash.begin() + k);
                continue;
            }
            if (i < maxline) {
                char lbl[300];
                snprintf(lbl, sizeof(lbl), "%s (%ldx)",
                         f.name.c_str(), f.count);
                ImVec2 pos(20.f, 150.f + lh * (float)i);
                ImVec2 ts = ImGui::CalcTextSize(lbl);
                float g = f.time > 0.f
                              ? (f.time > 1.f ? 1.f : f.time) : 0.f;
                ImVec4 col(1.f - g, 1.f, 1.f - g, 1.f);
                if (f.gone < 1.f) col.w = f.gone;
                dl->AddRectFilled(pos,
                                  ImVec2(pos.x + ts.x, pos.y + ts.y),
                                  IM_COL32(0, 0, 0, 100));
                dl->AddText(pos, ImGui::GetColorU32(col), lbl);
                i++;
            }
            k++;
        }
    }
    MULK();
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    if (s_fullscreen) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ds);
        wflags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    } else {
        if (s_fs_restore) {
            s_fs_restore = false;
            ImGui::SetNextWindowPos(s_fs_last_pos);
            ImGui::SetNextWindowSize(s_fs_last_size);
        }
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(420, 300), ImVec2(ds.x * .95f, ds.y * .95f));
        ImGui::SetNextWindowSize(ImVec2(ds.x * .8f, ds.y * .85f),
                                 ImGuiCond_Once);
    }
    ImGui::SetNextWindowBgAlpha(s_opacity);
    bool open = vis;
    ImGui::Begin("Dex-tool v1.3 (r31)", &open, wflags);
    if (!open) {
        MLK();
        s_visible = false;
        s_visible_pub.store(false);
        s_conf_dirty = true;
        if (!s_show_fab) s_show_fab = true;
        MULK();
    }
    if (ImGui::BeginTabBar("mainTabber")) {
        struct TabDef { const char* name; void (*fn)(); };
        TabDef tabs[] = {
            {"Tools",    tab_tools},    {"Tracer",   tab_tracer},
            {"Patches",  tab_patches},  {"Dumper",   tab_dumper},
            {"Recorder", tab_recorder}, {"Monitor",  tab_monitor},
            {"Script",   tab_script},   {"Settings", tab_settings},
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
    if (s_clip_msg[0] && conf_now_ms() - s_clip_ms < 2500)
        ImGui::TextColored(ImVec4(.4f, 1.f, .5f, 1), "%s", s_clip_msg);
    families_window();
    fm_window();
    if (!s_fullscreen) draw_resize_handles();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::End();
    mark_rect(wp.x, wp.y, ws.x, ws.y, s_ovl_scroll);
    overlay_publish_scroll_regions(s_regions);
    s_regions.clear();
    if (s_conf_dirty && s_conf_path[0] &&
        conf_now_ms() - s_conf_last_ms > 700) {
        conf_save();
        MLK();
        overlay_set_touch_bias(s_y_off);
        MULK();
    }
}
