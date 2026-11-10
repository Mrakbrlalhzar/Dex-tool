#include "overlay.h"
#include "logger.h"
#include "menu.h"
#include "frida_bridge.h"
#include "touch.h"
#include "imgui.h"
#include "imgui_internal.h"   /* r23: live window list for the hit test */
#include "imgui_impl_opengl3.h"
#include "dobby.h"
#include "frida-gumjs.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <ctime>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_swap_dmg)(EGLDisplay, EGLSurface, EGLint*, EGLint);
typedef EGLBoolean (*fn_destroy_ctx)(EGLDisplay, EGLContext);
typedef EGLBoolean (*fn_terminate)(EGLDisplay);

static fn_swap        g_orig_swap        = nullptr;
static fn_swap_dmg    g_orig_swap_dmg    = nullptr;
static fn_destroy_ctx g_orig_destroy_ctx = nullptr;
static fn_terminate   g_orig_terminate   = nullptr;

/* r23: appended to the cloned backend by the CI build (see the ImGui
   step in build.yml). Zeroes the stored GL object names WITHOUT any
   GL calls - the guarded deletes in DestroyDeviceObjects become
   no-ops, so a parked world can never delete stale names that a
   recycled name in the NEW context now owns (the r22 residual of
   the 0x501 abort class). */
IMGUI_IMPL_API void ImGui_ImplOpenGL3_ForgetDeviceObjects();

static std::atomic<bool> g_gum_inited {false};
static JavaVM*           g_jvm        = nullptr;

/* ---- single-owner overlay ---------------------------------------- r9
   r7 elected exactly one EGL context as the overlay owner (the
   continuously rendering GL thread of the game/view) and only that
   thread touches ImGui or GL. r9 keeps that, and adds:

   PARK + ADOPT. When the owner context dies (app switch: the
   GLSurfaceView destroys its context in onPause; keyboard with
   adjustResize on some devices; activity relayout) r7 destroyed the
   whole ImGui world and had to re-elect from scratch after 45 swaps,
   which looked like "switch to another app and back -> the app closes /
   the menu is gone". r9 parks the ImGui context (all window layout,
   input and scroll state) in g_orphan_imgui and the NEXT context that
   reaches 8 swaps adopts it: the menu reappears exactly as it was,
   usually within a fraction of a second of coming back to the app.
   The backend's GL handles died with the old context, so adoption
   calls ImGui_ImplOpenGL3_Shutdown (GL deletes are no-ops for names
   that were never created in the new context) and Init again, which
   makes NewFrame recreate the font texture and shader on the new
   context. The old context's objects are freed by the driver; we never
   call GL without a current context.

   r11 also retires every slot in the eglTerminate hook: terminating a
   display invalidates all of its contexts, and if the driver later hands
   out a new context at the same address, the stale slot would happily
   draw with handles that are invalid in it - the classic
   "glUseProgram -> GL_INVALID_VALUE (0x501)" abort class reported from
   the field. */
#define DT_ELECT_MIN_SWAPS 45
#define DT_ADOPT_MIN_SWAPS  8
#define DT_OWNER_STALE_MS  1500

struct GlState {
    GLint program = 0;
    GLint vao = 0, ab = 0, eb = 0;
    GLint fb = 0;                     /* r9: app FBO restored too */
    GLint active_tex = GL_TEXTURE0, tex2d = 0;
    /* r23: vertex-attrib-array ENABLE flags are global context state
       on ES2 (VAO state on ES3, but we run the ES2 backend path) -
       the ImGui draw leaves attribs 0/1 enabled; apps that never
       re-enable their own attribs drew garbage after our frame. */
    GLboolean attrib_en[8] = {GL_FALSE};
    GLint viewport[4]    = {0, 0, 0, 0};
    GLint scissor_box[4] = {0, 0, 0, 0};
    GLboolean scissor = GL_FALSE, depth = GL_FALSE,
              stencil = GL_FALSE, blend = GL_FALSE, cull = GL_FALSE;
    GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO,
          blend_src_a = GL_ONE, blend_dst_a = GL_ZERO,
          blend_eq_rgb = GL_FUNC_ADD, blend_eq_a = GL_FUNC_ADD;
};
struct Slot {
    ImGuiContext* imgui = nullptr;
    bool  failed = false;
    bool  es3 = false;
    bool  dead = false;         /* under g_slots_mtx */
    bool  reject_logged = false;  /* r14: one log per rejection */
    long  swaps = 0;            /* under g_slots_mtx */
    long  last_swap_ms = 0;     /* under g_slots_mtx */
    long  last_frame_ms = 0;    /* owner thread only */
    EGLDisplay dpy = nullptr;   /* r23: which display this context belongs to - eglTerminate(dpy) may only retire THESE slots */
    GlState gl;
};
static std::mutex g_slots_mtx;
static std::unordered_map<EGLContext, Slot*> g_slots;
static std::vector<Slot*> g_graveyard;  /* never freed: no dangling */
static EGLContext g_owner = nullptr;    /* under g_slots_mtx */
static ImGuiContext* g_orphan_imgui = nullptr;  /* g_tick_mtx only */
/* r14: DisplaySize of the parked world; surfaces far smaller than
   this must NOT adopt it (the "small clone of our menu" bug) */
static float g_orphan_dw = 0.f, g_orphan_dh = 0.f;
static std::mutex g_tick_mtx;  /* one ImGui drawer at a time; also
                                  serializes ownership changes, adoption
                                  and slot teardown against frames */
static std::atomic<bool> g_ready {false};

/* ---- touch mapping ------------------------------------------------
   Events arrive in raw SCREEN pixels from the Java path (r15: the hook
   forwards getRawX()/getRawY()) and in window-local pixels from the
   AInputQueue fallback (no Java side - approximate; the Settings Y
   bias exists for such hosts). Two sources feed the map, best wins:
   (a) the agent measured where the overlay's GL view actually sits
       ON SCREEN (getLocationOnScreen of the largest SurfaceView /
       GLSurfaceView / TextureView, content view fallback) ->
       dextool_set_touch_map; measured on Activity.onResume and on
       every touch (throttled), so a relayout at any point in the
       app's life still updates the map.
   (b) native estimate: the largest surface seen on ANY context is
       ~= the full window (a typical app's HWUI surface spans the whole
       540x960 window while the game surface is smaller). r11 tracks
       every swap for this, not just the owner's. Transient only -
       the agent's real measurement replaces it within a few frames.
   On top of the map, overlay_set_touch_bias() adds a manual Y nudge
   (Settings, default 0 since r15) for hosts where neither source
   can be measured. */
struct TouchMap { float x=0, y=0, w=0, h=0; bool ok=false; bool est=false; };
static std::mutex g_map_mtx;
static TouchMap   g_map;
static float      g_bias_y = 0.f;      /* manual nudge (Settings), under g_map_mtx; r15: default 0 - the screen-space map is exact, the slider is only for exotic hosts */

/* Touch stream with dedup: a NativeActivity host can deliver the same
   physical tap through BOTH the AInputQueue and the Java bridge; an
   event identical to the previous one within 40 ms is dropped. */
struct TouchEvent { float x, y; int phase; };
static std::mutex              g_touch_mtx;
static std::vector<TouchEvent> g_touch_q;
static TouchEvent              g_touch_last;
static long                    g_touch_last_ms = 0;
static std::atomic<bool>       g_want_mouse {false};

static std::mutex            g_char_mtx;
/* r22/audit: ONE ordered queue. The agent sends deletions first,
   then insertions (autocorrect "teh" -> "the"); two separate queues
   replayed every insertion before every backspace and produced the
   wrong text. Surrogate halves (0xD800-0xDFFF) are routed through
   io.AddInputCharacterUTF16() below, so astral chars survive. */
struct InEv { unsigned cp; bool bs; };
static std::vector<InEv>      g_pending_in;

/* scroll regions published by menu_draw (owner thread) */
static std::vector<float> g_scroll_regions;

/* r23: rects of every LIVE ImGui window of the last frame (owner
   thread collects them right after menu_draw; the Java touch hook
   reads them from dextool_hit_test() on a binder/UI thread - the
   only cross-thread contact, guarded here). */
static std::mutex           g_win_mtx;
static std::vector<float>   g_window_rects;   /* x,y,w,h quads */

/* r23: the rects must not outlive the world that drew them -
   after a park (owner death / display terminate / migration)
   NOTHING is drawn until a new context adopts, so a frozen
   rect would make dextool_hit_test() eat taps aimed at
   whatever is on screen now. The next owner frame rebuilds
   the snapshot. */
static void clear_window_rects() {
    std::lock_guard<std::mutex> lk(g_win_mtx);
    g_window_rects.clear();
}

/* font atlas: rebuilt on demand from a system font, so the Font Size
   slider actually changes the glyphs instead of just scaling them */
static float g_font_size  = 18.0f;
static bool  g_font_dirty = false;
static char  g_font_path[128] = "";
static std::mutex g_disp_mtx;
static float g_disp_w = 0.f, g_disp_h = 0.f;
static float g_max_surf_w = 0.f, g_max_surf_h = 0.f;   /* seen on ANY ctx */

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void touch_push(float x, float y, int phase) {
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    long now = now_ms();
    if (g_touch_last.phase == phase &&
        fabsf(x - g_touch_last.x) < 0.01f &&
        fabsf(y - g_touch_last.y) < 0.01f &&
        (now - g_touch_last_ms) < 40)
        return;                          /* duplicate from the 2nd path */
    g_touch_last = {x, y, phase};
    g_touch_last_ms = now;
    if (g_touch_q.size() > 96) g_touch_q.erase(g_touch_q.begin());
    g_touch_q.push_back({x, y, phase});
}

extern "C" JNIEXPORT void dextool_on_touch(float x, float y, int action) {
    int a = action & 0xff;
    if      (a == 0)            touch_push(x, y, 0);  /* DOWN */
    else if (a == 1 || a == 3)  touch_push(x, y, 1);  /* UP/CANCEL */
    else if (a == 2)            touch_push(x, y, 2);  /* MOVE */
    else if (a == 5 || a == 6)  touch_push(x, y, 2);  /* PTR up/dn */
}

extern "C" JNIEXPORT void dextool_set_touch_map(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(g_map_mtx);
    bool was = g_map.ok;
    g_map = { (float)x, (float)y, (float)w, (float)h, true, false };
    if (!was)
        LOGI("[overlay] touch map: view rect on screen = (%d,%d %dx%d)",
             x, y, w, h);
}

void overlay_set_touch_bias(float dy) {
    if (dy < -100.f) dy = -100.f;
    if (dy >  100.f) dy =  100.f;
    std::lock_guard<std::mutex> lk(g_map_mtx);
    g_bias_y = dy;
}

void overlay_touch_map_desc(char* buf, int n) {
    if (!buf || n <= 0) return;
    std::lock_guard<std::mutex> lk(g_map_mtx);
    if (g_map.ok)
        snprintf(buf, n, "%s view (%.0f,%.0f) %.0fx%.0f + Y%+.0f",
                 g_map.est ? "est" : "measured",
                 g_map.x, g_map.y, g_map.w, g_map.h, g_bias_y);
    else
        snprintf(buf, n, "none yet (raw passthrough) + Y%+.0f", g_bias_y);
}

/* native estimate: while the agent has not measured the view rect yet,
   assume the window is as tall as the largest surface we have seen (a
   typical app's HWUI surface spans the full window) and that the GL
   view starts at the top-left. This already corrects the systematic
   vertical offset; the agent's real measurement replaces it. */
static void maybe_estimate_map() {
    std::lock_guard<std::mutex> lk(g_map_mtx);
    if (g_map.ok) return;                      /* real map present */
    float w = 0.f, h = 0.f;
    {
        std::lock_guard<std::mutex> lk2(g_disp_mtx);
        w = g_max_surf_w; h = g_max_surf_h;
    }
    if (w <= 0.f || h <= 0.f) return;
    g_map = { 0.f, 0.f, w, h, true, true };
    LOGI("[overlay] touch map: estimate 0,0 %.0fx%.0f (max surface)", w, h);
}

/* defined further down */
static ImVec2 map_touch(float x, float y, const ImVec2& ds);

extern "C" JNIEXPORT bool dextool_want_mouse() {
    /* r12: the drawn keyboard is gone (text input is the system IME
       now, which is its own window - its taps never reach the
       activity). Only the menu itself claims events. */
    return g_want_mouse.load();
}

extern "C" JNIEXPORT int dextool_display_size() {
    std::lock_guard<std::mutex> lk(g_disp_mtx);
    if (g_disp_w <= 0 || g_disp_h <= 0) return 0;
    return ((int)g_disp_w << 16) | (int)g_disp_h;
}

extern "C" JNIEXPORT void dextool_add_char(int cp) {
    std::lock_guard<std::mutex> lk(g_char_mtx);
    if (g_pending_in.size() < 8192) g_pending_in.push_back(InEv{(unsigned)cp, false});
}

extern "C" JNIEXPORT void dextool_backspace() {
    std::lock_guard<std::mutex> lk(g_char_mtx);
    if (g_pending_in.size() < 8192) g_pending_in.push_back(InEv{0, true});
}

/* r14: the GumJS agent posts phase markers here (NativeFunction on
   this export) before and around every risky init step. The markers
   land in dextool-crash.log AND dextool-debug.log, so a field abort
   pins the exact phase that killed the process. */
extern "C" JNIEXPORT void dextool_agent_log(const char* msg) {
    if (!msg) return;
    /* r22/audit: log_debug() adds its own newline - feeding it the
       \n-terminated string wrote a blank line after every marker */
    char d[560];
    snprintf(d, sizeof(d), "[agent] %.500s", msg);
    log_debug(d);
    char m[570];
    snprintf(m, sizeof(m), "%s\n", d);
    log_crash_raw(m);
}

bool overlay_ready() { return g_ready.load(); }

void overlay_get_display_size(float* w, float* h) {
    std::lock_guard<std::mutex> lk(g_disp_mtx);
    if (w) *w = g_disp_w;
    if (h) *h = g_disp_h;
}

void overlay_request_font_rebuild(float size) {
    if (size < 10.f) size = 10.f;
    if (size > 40.f) size = 40.f;
    g_font_size  = size;
    g_font_dirty = true;
}

void overlay_publish_scroll_regions(const std::vector<float>& xywh_speed) {
    /* publisher (menu_draw) and consumer (replay) are both the owner
       thread, one frame apart - no lock needed */
    g_scroll_regions = xywh_speed;
}

static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

/* called from JNI_OnLoad before overlay_start; the frida thread and
   the bridge read g_jvm afterwards */
void overlay_set_java_vm(JavaVM* vm) { g_jvm = vm; }

void overlay_start(JavaVM*) {
    if (g_gum_inited.exchange(true)) return;
    gum_init_embedded();
    log_info("[overlay] gum_init_embedded done");
    pthread_t ft;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&ft, &a, frida_thread, nullptr);
    pthread_attr_destroy(&a);
    touch_install_hooks();
    log_info("[overlay] touch hooks installed, frida thread started");
}

static void menu_init_once() {
    static bool done = false;
    if (!done) { done = true; menu_init(); }
}

static Slot* slot_for(EGLContext ctx) {
    std::lock_guard<std::mutex> lk(g_slots_mtx);
    Slot*& s = g_slots[ctx];
    if (!s) s = new Slot();
    return s;
}

/* Ownership rules (map work under g_slots_mtx, whole frame under
   g_tick_mtx, so this can never race a draw):
     - fast path: we are already the owner;
     - if the owner went quiet for DT_OWNER_STALE_MS while we keep
       swapping, ownership migrates to us;
     - with no owner: the first slot to reach DT_ELECT_MIN_SWAPS swaps
       wins (the continuously rendering thread, not the bursty HWUI
       RenderThread). If an ImGui world is parked from a dead context,
       DT_ADOPT_MIN_SWAPS is enough - coming back from the background
       the new context adopts it almost immediately. */
static bool ensure_ownership(Slot* s, EGLContext ctx, EGLDisplay dpy,
                             float sw, float sh) {
    std::lock_guard<std::mutex> lk(g_slots_mtx);
    if (s->dead) return false;
    s->dpy = dpy;              /* r23: recorded for my_terminate */
    s->swaps++;
    long now = now_ms();
    /* r22/audit: remember the PREVIOUS swap time - the stale-owner
       test below needs the value this swap replaced (comparing
       against the just-written one made the test always true) */
    long prev_swap = s->last_swap_ms;
    s->last_swap_ms = now;
    Slot* ow = nullptr;
    if (g_owner) {
        auto it = g_slots.find(g_owner);
        if (it != g_slots.end() && !it->second->dead) ow = it->second;
    }
    if (ow == s) return true;

    /* r14: never take over a surface far smaller than the menu
       world. When the owner context died (app menu, dialog,
       keyboard relayout), the parked ImGui world used to be ADOPTED
       by the next context to reach 8 swaps - including small popup
       surfaces, which then rendered the whole menu inside their
       tiny window ("tapping the app's menu spawns a small clone of
       our menu"), and the clone ignores taps because the touch map
       belongs to the main surface. The real surface re-adopts when
       it returns. */
    if (sw > 0.f && sh > 0.f) {
        bool ok;
        if (g_orphan_imgui && g_orphan_dw > 0.f && g_orphan_dh > 0.f) {
            float pa = g_orphan_dw * g_orphan_dh;
            ok = (sw * sh) >= pa * 0.5f &&
                 sw >= g_orphan_dw * 0.45f &&
                 sh >= g_orphan_dh * 0.45f;
        } else {
            ok = sw >= 220.f && sh >= 220.f;
        }
        if (!ok) {
            if (!s->reject_logged) {
                s->reject_logged = true;
                LOGI("[overlay] skip small surface %.0fx%.0f (%s)",
                     sw, sh, g_orphan_imgui ?
                         "menu parked - not adopting" : "election");
            }
            return false;
        }
    }
    if (ow) {
        bool stale = (now - ow->last_swap_ms) > DT_OWNER_STALE_MS &&
                     (now - prev_swap)        < 600;
        if (!stale) return false;     /* healthy owner elsewhere */
        /* r23: the migration PARKED nothing before - the old owner's
           slot kept its whole ImGui world (layouts, lists, fonts)
           alive even though that context is gone: the leak the audit
           flagged. Park it as the orphan (the next context adopts
           it), and Forget its backend GL names - the old context is
           not current here (we are on the NEW owner's thread), so
           Shutdown/delete would hit whatever owns those names now. */
        if (ow->imgui && !g_orphan_imgui) {
            g_orphan_imgui = ow->imgui;
            ow->imgui = nullptr;
            {
                std::lock_guard<std::mutex> lk3(g_disp_mtx);
                g_orphan_dw = g_disp_w;
                g_orphan_dh = g_disp_h;
            }
            ImGui::SetCurrentContext(g_orphan_imgui);
            ImGui_ImplOpenGL3_ForgetDeviceObjects();
            clear_window_rects();   /* nothing draws for this world
                                       until a context adopts it */
            LOGI("[overlay] owner migrated - old ImGui world parked "
                 "(not leaked)");
        }
        g_owner = nullptr;
        LOGI("[overlay] owner migrated (previous went quiet)");
    }
    if (g_owner == nullptr) {
        long need = g_orphan_imgui ? DT_ADOPT_MIN_SWAPS : DT_ELECT_MIN_SWAPS;
        if (s->swaps >= need) {
            g_owner = ctx;
            LOGI("[overlay] owner elected after %ld swaps%s",
                 s->swaps, g_orphan_imgui ? " (adopting parked ImGui)" : "");
            return true;
        }
    }
    return false;
}

static void resolve_font_path() {
    if (g_font_path[0]) return;
    const char* cands[] = {
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
        "/system/fonts/NotoSansCJK-Regular.ttc",
    };
    for (const char* c : cands)
        if (access(c, R_OK) == 0) {
            snprintf(g_font_path, sizeof(g_font_path), "%s", c);
            return;
        }
}

static void build_fonts(ImGuiIO& io) {
    resolve_font_path();
    io.Fonts->Clear();
    bool ok = false;
    if (g_font_path[0]) {
        /* r22/audit: dead ImFontConfig removed (built but never
           passed to AddFontFromFileTTF) */
        ok = io.Fonts->AddFontFromFileTTF(g_font_path, g_font_size,
                                         nullptr, nullptr) != nullptr;
    }
    if (!ok) io.Fonts->AddFontDefault();
    io.Fonts->Build();
    g_font_dirty = false;
}

/* build (or adopt) the ImGui world of a freshly elected slot */
static void slot_init(EGLDisplay dpy, EGLSurface surf, Slot* s) {
    EGLint w = 1080, h = 1920;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const GLubyte* ver = glGetString(GL_VERSION);
    if (!ver || !strstr((const char*)ver, "OpenGL ES")) {
        LOGE("[overlay] not OpenGL ES: %s", ver ? (const char*)ver : "null");
        s->failed = true;
        return;
    }
    s->es3 = strstr((const char*)ver, "OpenGL ES 3") != nullptr;

    if (g_orphan_imgui) {
        /* adopt the parked world; its backend GL handles are stale, so
           ShutDown + Init to force CreateDeviceObjects on this context */
        s->imgui = g_orphan_imgui;
        g_orphan_imgui = nullptr;
        g_orphan_dw = 0.f; g_orphan_dh = 0.f;
        ImGui::SetCurrentContext(s->imgui);
        /* r23: every park path either already ran a real Shutdown
           (dying context was current) or called ForgetDeviceObjects
           (backend patch appended by CI) first - the stored GL
           names are zeroed, so this Shutdown's glDelete* calls are
           no-ops and can never hit a name the new context recycled
           for one of the app's own objects. Shutdown still frees
           the backend data struct, which the raw Forget cannot. */
        if (ImGui::GetIO().BackendRendererUserData)
            ImGui_ImplOpenGL3_Shutdown();
        LOGI("[overlay] ImGui world adopted after context switch");
    } else {
        s->imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(s->imgui);
        LOGI("[overlay] GL: %s (%dx%d)", (const char*)ver, w, h);
    }

    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigWindowsMoveFromTitleBarOnly = true;  /* touch friendly */
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    if (io.Fonts->Fonts.empty() || g_font_dirty) build_fonts(io);
    ImGui_ImplOpenGL3_Init("#version 100");
    s->last_frame_ms = now_ms();
    {
        std::lock_guard<std::mutex> lk(g_disp_mtx);
        g_disp_w = (float)w; g_disp_h = (float)h;
    }
    LOGI("[overlay] ImGui up OK (%dx%d, font %.0fpx)", w, h, g_font_size);
}

/* eglDestroyContext hook: retire the slot WITH its context. r7 tore the
   whole ImGui world down here, so every activity relayout that recycled
   the GL context (keyboard with adjustResize, app switch, rotation) was
   fatal - the user saw it as "the app closes". r9 parks the owner's
   ImGui world instead and never frees the Slot (graveyard), so an
   in-flight tick just sees dead=true on its next lock. */
static void slot_destroy(EGLContext ctx) {
    std::lock_guard<std::mutex> lk(g_tick_mtx);
    std::lock_guard<std::mutex> lk2(g_slots_mtx);
    auto it = g_slots.find(ctx);
    if (it == g_slots.end()) return;
    Slot* s = it->second;
    s->dead = true;
    g_slots.erase(it);
    g_graveyard.push_back(s);
    if (g_owner == ctx) {
        g_owner = nullptr;
        g_ready.store(false);
        if (s->imgui) {
            if (g_orphan_imgui)
                ImGui::DestroyContext(g_orphan_imgui);  /* keep newest */
            g_orphan_imgui = s->imgui;
            s->imgui = nullptr;
            {
                std::lock_guard<std::mutex> lk3(g_disp_mtx);
                g_orphan_dw = g_disp_w;
                g_orphan_dh = g_disp_h;
            }
            if (eglGetCurrentContext() == ctx) {
                /* only valid while the dying context is still current;
                   otherwise the driver frees everything with the ctx */
                ImGui::SetCurrentContext(g_orphan_imgui);
                ImGui_ImplOpenGL3_Shutdown();
            } else {
                /* r23: the names belong to the dying context, but the
                   stored slots must not survive into a NEW context:
                   zero them with no GL calls (Shutdown here would
                   delete in whatever context is current - the r22
                   residual 0x501 class). */
                ImGui::SetCurrentContext(g_orphan_imgui);
                ImGui_ImplOpenGL3_ForgetDeviceObjects();
            }
            clear_window_rects();   /* r23: this world draws nothing
                                       until a new context adopts it */
            LOGI("[overlay] owner context destroyed - ImGui parked, "
                 "waiting for the next GL context");
        }
    }
}

/* HWUI and several driver wrappers abort the whole process on stray GL
   errors (the "glUseProgram -> GL_INVALID_VALUE (0x501)" SIGABRT seen in
   the field), so every binding the ES2 ImGui backend touches goes back
   exactly the way the app had it before the swap continues. */
static void gl_state_save(Slot* s) {
    GlState& g = s->gl;
    glGetIntegerv(GL_CURRENT_PROGRAM, &g.program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &g.ab);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &g.eb);
    if (s->es3)
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &g.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g.fb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &g.active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &g.tex2d);
    glGetIntegerv(GL_VIEWPORT,    g.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, g.scissor_box);
    /* r23: vertex-attrib enable flags (ES2 global state; 8 is the
       ES2 guaranteed minimum) */
    for (int i = 0; i < 8; i++) {
        GLint en = 0;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        g.attrib_en[i] = en ? GL_TRUE : GL_FALSE;
    }
    g.scissor = glIsEnabled(GL_SCISSOR_TEST);
    g.depth   = glIsEnabled(GL_DEPTH_TEST);
    g.stencil = glIsEnabled(GL_STENCIL_TEST);
    g.blend   = glIsEnabled(GL_BLEND);
    g.cull    = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_BLEND_SRC_RGB,   &g.blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,   &g.blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &g.blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &g.blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &g.blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &g.blend_eq_a);
}

/* r11: eat whatever the draw left in the error queue and make it
   visible in the debug log - some wrappers abort on a stray sticky
   error at their next check. */
static void gl_drain_errors() {
    GLenum first = GL_NO_ERROR;
    for (int i = 0; i < 12; i++) {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        if (first == GL_NO_ERROR) first = e;
    }
    if (first != GL_NO_ERROR)
        LOGD("[overlay] drained GL error 0x%04x after draw", first);
}

static void gl_state_restore(Slot* s) {
    GlState& g = s->gl;
    if (s->es3) glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER,         g.ab);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.eb);
    /* r23: put the app's vertex-attrib enable flags back - right
       after the buffer bindings, before anything else may bind */
    for (int i = 0; i < 8; i++) {
        if (g.attrib_en[i]) glEnableVertexAttribArray(i);
        else                glDisableVertexAttribArray(i);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,     g.fb);
    /* r22/audit: tex2d was captured with unit 0 active - restore it
       on unit 0, then put the app's active unit back */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.tex2d);
    glActiveTexture(g.active_tex);
    glBlendEquationSeparate(g.blend_eq_rgb, g.blend_eq_a);
    glBlendFuncSeparate(g.blend_src_rgb, g.blend_dst_rgb,
                        g.blend_src_a, g.blend_dst_a);
    if (g.blend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (g.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (g.depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (g.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (g.cull)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    glScissor(g.scissor_box[0], g.scissor_box[1],
              g.scissor_box[2], g.scissor_box[3]);
    glViewport(g.viewport[0], g.viewport[1],
               g.viewport[2], g.viewport[3]);
    /* r11: only restore a real program. Apps that never bind one (the
       sample host just clears) have 0 current; some ES wrappers raise
       GL_INVALID_VALUE for glUseProgram(0) and the app's next error
       check aborts. If the app had none bound it will bind its own
       before drawing, so leaving ours is safe. */
    if (g.program != 0)
        glUseProgram(g.program);
    gl_drain_errors();
}

/* is a display-space point inside one of the published scroll rects? */
static float region_speed_at(float x, float y) {
    const std::vector<float>& r = g_scroll_regions;
    for (size_t i = 0; i + 4 < r.size(); i += 5) {
        if (x >= r[i] && x <= r[i] + r[i+2] &&
            y >= r[i+1] && y <= r[i+1] + r[i+3])
            return r[i+4];
    }
    return 0.f;
}

/* map a raw SCREEN position into the overlay surface's coordinate
   space. r15 rebuilds the transform on ONE coordinate space: the
   hook forwards getRawX()/getRawY() (screen) and the agent measures
   the GL view with getLocationOnScreen() (screen), so the origin is
   shared and BOTH axes are scaled proportionally:

       imgui = (raw - viewOrigin) * (io.DisplaySize / viewSize)

   The r12/r13 scheme mixed view-local getX()/getY() with a
   window-relative content-view rect, hardcoded sy = 1.0 and patched
   the residue with a device-specific -80 px Y bias - a workaround,
   not a fix (the logged surface delta was 128 px, not 80). The bias
   default is 0 now; the Settings slider remains for hosts where the
   view rect cannot be measured. */
static ImVec2 map_touch(float x, float y, const ImVec2& ds) {
    float mx = x, my = y;
    {
        std::lock_guard<std::mutex> lk(g_map_mtx);
        if (g_map.ok && g_map.w > 0 && g_map.h > 0) {
            float sx = ds.x / g_map.w;
            float sy = ds.y / g_map.h;
            mx = (x - g_map.x) * sx;
            my = (y - g_map.y) * sy;
        }
        my += g_bias_y;
    }
    /* r23: NO clamping. Clamping to the display rect pushed taps that
       landed OUTSIDE the measured view (letterboxed games, split
       screen, free-form windows) onto the edge widgets - a tap below
       the view hit the bottom row of the menu. Off-view taps now map
       off-canvas and simply hit nothing. */
    return ImVec2(mx, my);
}

/* r23: owner thread, right after menu_draw()/Render() - snapshot the
   rects of every live ImGui window for dextool_hit_test(). */
static void collect_window_rects() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    std::vector<float> wr;
    if (g) {
        for (int i = 0; i < g->Windows.Size; i++) {
            ImGuiWindow* w = g->Windows[i];
            if (!w || !w->Active || w->Hidden) continue;
            if (w->Flags & ImGuiWindowFlags_NoInputs) continue;
            ImRect r = w->Rect();
            wr.push_back(r.Min.x);
            wr.push_back(r.Min.y);
            wr.push_back(r.Max.x - r.Min.x);
            wr.push_back(r.Max.y - r.Min.y);
        }
    }
    std::lock_guard<std::mutex> lk(g_win_mtx);
    g_window_rects.swap(wr);
}

/* r23: click-through filter, called by the Java touch hook BEFORE the
   gesture reaches the host app: true = the tap maps onto a live ImGui
   window and must be consumed. The window-rect snapshot IS the source
   of truth: io.WantCaptureMouse (g_want_mouse) was tried as a gate
   here, but it is computed from the PREVIOUS event's position - the
   FIRST tap on the menu after any passthrough tap (e.g. the FAB tap
   that opens the menu) still leaked to the app below it. Rects are
   refreshed every frame by collect_window_rects() and cleared on
   every park, so a hit here always means "a window was really drawn
   at this point last frame". Runs on the app's UI thread: only
   touches mutex-guarded snapshots. */
extern "C" JNIEXPORT bool dextool_hit_test(float x, float y) {
    float dw = 0.f, dh = 0.f;
    {
        std::lock_guard<std::mutex> lk(g_disp_mtx);
        dw = g_disp_w;
        dh = g_disp_h;
    }
    if (dw <= 0.f || dh <= 0.f) return false;
    ImVec2 p = map_touch(x, y, ImVec2(dw, dh));
    std::lock_guard<std::mutex> lk(g_win_mtx);
    const std::vector<float>& r = g_window_rects;
    for (size_t i = 0; i + 3 < r.size(); i += 4) {
        if (p.x >= r[i] && p.x <= r[i] + r[i + 2] &&
            p.y >= r[i + 1] && p.y <= r[i + 1] + r[i + 3])
            return true;
    }
    return false;
}

static void overlay_tick(EGLDisplay dpy, EGLSurface surf) {
    static thread_local bool in_tick = false;
    if (in_tick) return;   /* driver re-entry (swap->swapWithDamage) */
    in_tick = true;
    struct TickReset { bool& b; ~TickReset() { b = false; } }
        tick_reset{in_tick};

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;

    /* r11: surface bookkeeping for EVERY context, before ownership
       gating. The largest surface on any thread (the HWUI window
       surface spans the whole window) is the best native estimate of
       the full window, so the touch-map fallback no longer depends on
       this thread being the owner. */
    EGLint sw = 0, sh = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &sw);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
    if (sw > 0 && sh > 0) {
        std::lock_guard<std::mutex> lk(g_disp_mtx);
        if ((float)sw > g_max_surf_w) g_max_surf_w = (float)sw;
        if ((float)sh > g_max_surf_h) g_max_surf_h = (float)sh;
        if (g_disp_w <= 0.f) { g_disp_w = (float)sw; g_disp_h = (float)sh; }
    }

    /* one drawer at a time; also serializes ownership changes, adoption
       and slot teardown against in-flight frames */
    std::lock_guard<std::mutex> lk(g_tick_mtx);

    Slot* s = slot_for(ctx);
    if (!ensure_ownership(s, ctx, dpy, (float)sw, (float)sh)) return;
    if (s->dead || s->failed) return;

    if (!s->imgui) slot_init(dpy, surf, s);
    if (!s->imgui) return;
    ImGui::SetCurrentContext(s->imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)sw, (float)sh);
    {
        std::lock_guard<std::mutex> lk2(g_disp_mtx);
        g_disp_w = (float)sw; g_disp_h = (float)sh;
    }
    {
        long nowt = now_ms();
        if (s->last_frame_ms > 0) {
            float dt = (float)(nowt - s->last_frame_ms) / 1000.0f;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.05f)  dt = 0.05f;
            io.DeltaTime = dt;
        }
        s->last_frame_ms = nowt;
    }

    /* replay queued touch events as real ImGui events. Drags that
       start inside a published scroll rect additionally become wheel
       events, scaled by that rect's speed - this is the touch
       scrolling of the recorder/monitor/class lists and of the menu
       itself. (r12: the drawn keyboard is gone; IME text arrives as
       character events further down.) */
    {
        std::vector<TouchEvent> q;
        {
            std::lock_guard<std::mutex> lk2(g_touch_mtx);
            q.swap(g_touch_q);
        }
        ImVec2 ds = io.DisplaySize;
        ImVec2 prev(0, 0);
        bool   have_prev = false;
        for (const TouchEvent& t : q) {
            ImVec2 p = map_touch(t.x, t.y, ds);
            io.AddMousePosEvent(p.x, p.y);
            if (t.phase == 0) io.AddMouseButtonEvent(0, true);
            if (t.phase == 1) io.AddMouseButtonEvent(0, false);
            if (t.phase == 2) {
                io.AddMouseButtonEvent(0, true);
                if (have_prev) {
                    float sp = region_speed_at(prev.x, prev.y);
                    if (sp > 0.f) {
                        float wy = (p.y - prev.y) * sp;
                        if (wy >  400.f) wy =  400.f;
                        if (wy < -400.f) wy = -400.f;
                        if (fabsf(wy) > 2.f) io.AddMouseWheelEvent(0.f, -wy);
                    }
                }
            }
            prev = p;
            have_prev = true;
        }
    }

    {
        std::lock_guard<std::mutex> lk2(g_char_mtx);
        for (const InEv& ev : g_pending_in) {
            if (ev.bs) {
                io.AddKeyEvent(ImGuiKey_Backspace, true);
                io.AddKeyEvent(ImGuiKey_Backspace, false);
            } else if (ev.cp >= 0xD800 && ev.cp <= 0xDFFF) {
                /* UTF-16 surrogate half: let ImGui recombine the pair */
                io.AddInputCharacterUTF16((ImWchar16)ev.cp);
            } else {
                io.AddInputCharacter(ev.cp);
            }
        }
        g_pending_in.clear();
    }

    menu_init_once();
    maybe_estimate_map();     /* native fallback until the agent measures */
    if (g_font_dirty) {
        build_fonts(io);
        /* drop the GL font texture so NewFrame re-uploads it at the
           new size; the current context is ours and live right here */
        ImGui_ImplOpenGL3_DestroyDeviceObjects();
    }

    gl_state_save(s);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* the ES2 backend relies on GL_SCISSOR_TEST for clipping; the app
       may leave it disabled. Enable it for our draw - restore puts the
       app's state back afterwards. */
    glEnable(GL_SCISSOR_TEST);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    menu_draw();
    collect_window_rects();   /* r23: snapshot for the hit test */
    g_want_mouse.store(io.WantCaptureMouse);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    gl_state_restore(s);
    g_ready.store(true);
}

static EGLBoolean my_destroy_ctx(EGLDisplay dpy, EGLContext ctx) {
    slot_destroy(ctx);
    return g_orig_destroy_ctx ? g_orig_destroy_ctx(dpy, ctx) : EGL_TRUE;
}

static EGLBoolean my_terminate(EGLDisplay dpy) {
    /* r11: terminating a display invalidates every context of that
       display. r23: retire ONLY the slots that belong to THIS display
       (recorded at swap time in Slot.dpy) - a second display's
       contexts stay valid, and slots never drawn on have no recorded
       display and are retired conservatively; otherwise the next
       eglCreateContext can land at a recycled address whose stale
       slot would draw with GL handles that are invalid in the new
       context (the glUseProgram GL_INVALID_VALUE 0x501 abort class). */
    {
        std::lock_guard<std::mutex> lk(g_tick_mtx);
        std::lock_guard<std::mutex> lk2(g_slots_mtx);
        ImGuiContext* park = nullptr;
        bool had_owner = false;
        bool retired_any = false;
        for (auto& kv : g_slots) {
            Slot* s = kv.second;
            if (s->dpy != nullptr && s->dpy != dpy)
                continue;               /* another display - still live */
            s->dead = true;
            retired_any = true;
            g_graveyard.push_back(s);
            if (g_owner == kv.first) {
                had_owner = true;
                if (s->imgui) {
                    if (park) ImGui::DestroyContext(park);
                    park = s->imgui;
                    s->imgui = nullptr;
                }
            } else if (s->imgui) {
                ImGui::DestroyContext(s->imgui);
                s->imgui = nullptr;
            }
        }
        if (retired_any) {
            for (auto it = g_slots.begin(); it != g_slots.end(); ) {
                if (it->second->dead) it = g_slots.erase(it);
                else ++it;
            }
        }
        if (had_owner) {
            g_owner = nullptr;
            g_ready.store(false);
            if (park) {
                if (g_orphan_imgui)
                    ImGui::DestroyContext(g_orphan_imgui);  /* keep newest */
                g_orphan_imgui = park;
                {
                    std::lock_guard<std::mutex> lk3(g_disp_mtx);
                    g_orphan_dw = g_disp_w;
                    g_orphan_dh = g_disp_h;
                }
                /* r23: the terminated display's contexts are gone -
                   the parked backend names are stale; zero them with
                   NO GL calls so adoption can never delete a recycled
                   name in the new context (audit item: stale GL
                   names deleted in the new context) */
                ImGui::SetCurrentContext(g_orphan_imgui);
                ImGui_ImplOpenGL3_ForgetDeviceObjects();
                clear_window_rects();
                LOGI("[overlay] display terminated - ImGui parked, "
                     "waiting for the next GL context");
            }
        }
    }
    return g_orig_terminate ? g_orig_terminate(dpy) : EGL_TRUE;
}

/* r22/audit: ONE flag for BOTH hooks, held across the call into the
   real function. libEGL's eglSwapBuffers forwards to
   eglSwapBuffersWithDamageKHR (also hooked); the in_tick guard
   inside overlay_tick was already released by then, so the overlay
   was drawn TWICE per frame. */
static thread_local bool t_in_swap = false;

static EGLBoolean my_swap(EGLDisplay dpy, EGLSurface surf) {
    if (t_in_swap) return g_orig_swap ? g_orig_swap(dpy, surf) : EGL_FALSE;
    t_in_swap = true;
    overlay_tick(dpy, surf);
    EGLBoolean r = g_orig_swap ? g_orig_swap(dpy, surf) : EGL_FALSE;
    t_in_swap = false;
    return r;
}

static EGLBoolean my_swap_dmg(EGLDisplay dpy, EGLSurface surf,
                              EGLint* rects, EGLint n) {
    if (t_in_swap)
        return g_orig_swap_dmg ? g_orig_swap_dmg(dpy, surf, rects, n)
                               : EGL_FALSE;
    t_in_swap = true;
    overlay_tick(dpy, surf);
    EGLBoolean r = g_orig_swap_dmg ? g_orig_swap_dmg(dpy, surf, rects, n)
                                   : EGL_FALSE;
    t_in_swap = false;
    return r;
}

bool install_swap_hook() {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { LOGE("dlopen libEGL.so: %s", dlerror()); return false; }
    bool ok = false;
    void* sym = dlsym(egl, "eglSwapBuffers");
    if (sym) {
        int r = DobbyHook(sym,
            reinterpret_cast<dobby_dummy_func_t>(my_swap),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap));
        if (r == 0) { LOGI("eglSwapBuffers hooked OK"); ok = true; }
        else LOGE("DobbyHook eglSwapBuffers: %d", r);
    }
    void* sym_dmg = dlsym(egl, "eglSwapBuffersWithDamageKHR");
    if (!sym_dmg) {
        typedef void* (*PFN)(const char*);
        PFN gpa = (PFN)dlsym(egl, "eglGetProcAddress");
        if (gpa) {
            sym_dmg = gpa("eglSwapBuffersWithDamageKHR");
            if (!sym_dmg) sym_dmg = gpa("eglSwapBuffersWithDamageEXT");
        }
    }
    if (sym_dmg) {
        int r = DobbyHook(sym_dmg,
            reinterpret_cast<dobby_dummy_func_t>(my_swap_dmg),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap_dmg));
        if (r == 0) LOGI("eglSwapBuffersWithDamageKHR hooked OK");
        else        LOGE("DobbyHook eglSwapBuffersWithDamageKHR: %d", r);
    }
    void* sym_dc = dlsym(egl, "eglDestroyContext");
    if (sym_dc) {
        int r = DobbyHook(sym_dc,
            reinterpret_cast<dobby_dummy_func_t>(my_destroy_ctx),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_destroy_ctx));
        if (r == 0) LOGI("eglDestroyContext hooked OK");
        else LOGE("DobbyHook eglDestroyContext: %d", r);
    }
    void* sym_term = dlsym(egl, "eglTerminate");
    if (!sym_term) {
        typedef void* (*PFN)(const char*);
        PFN gpa = (PFN)dlsym(egl, "eglGetProcAddress");
        if (gpa) sym_term = gpa("eglTerminate");
    }
    if (sym_term) {
        int r = DobbyHook(sym_term,
            reinterpret_cast<dobby_dummy_func_t>(my_terminate),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_terminate));
        if (r == 0) LOGI("eglTerminate hooked OK (r11)");
        else LOGE("DobbyHook eglTerminate: %d", r);
    }
    return ok;
}
