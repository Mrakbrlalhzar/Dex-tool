#include "overlay.h"
#include "logger.h"
#include "menu.h"
#include "frida_bridge.h"
#include "touch.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "dobby.h"
#include "frida-gumjs.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_swap_dmg)(EGLDisplay, EGLSurface, EGLint*, EGLint);
typedef EGLBoolean (*fn_destroy_ctx)(EGLDisplay, EGLContext);

static fn_swap        g_orig_swap        = nullptr;
static fn_swap_dmg    g_orig_swap_dmg    = nullptr;
static fn_destroy_ctx g_orig_destroy_ctx = nullptr;

static std::atomic<bool> g_gum_inited {false};
static JavaVM*           g_jvm        = nullptr;

/* ---- single-owner overlay ---------------------------------------- r7
   The app has TWO GL threads (HWUI's RenderThread and the
   GLSurfaceView GLThread), each with its own EGL context, and BOTH
   call eglSwapBuffers. r6 gave every context its own ImGui world and
   drew the SAME menu from both threads:
     - two overlapping menus, only one of which received the touch
       events (they were consumed by whichever GL thread ticked
       first), so the other looked frozen and could not be resized;
     - taps landed next to the buttons because the winning thread's
       coordinate space did not match the surface the menu was on;
     - hiding the window (X / "Visible") mutated shared state under
       the other thread's feet.
   r7: every context still gets a Slot, but exactly ONE "owner" is
   allowed to run ImGui and draw (elected by swap activity, migrated
   if it goes quiet). Only the owner thread touches ImGui or GL
   state. Slots are never freed — eglDestroyContext just marks them
   dead — so an in-flight tick can never hold a dangling pointer. */
#define DT_ELECT_MIN_SWAPS 45
#define DT_OWNER_STALE_MS   1500

struct GlState {
    GLint program = 0;
    GLint vao = 0, ab = 0, eb = 0;
    GLint active_tex = GL_TEXTURE0, tex2d = 0;
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
    long  swaps = 0;            /* under g_slots_mtx */
    long  last_swap_ms = 0;     /* under g_slots_mtx */
    long  last_frame_ms = 0;    /* owner thread only */
    GlState gl;
};
static std::mutex g_slots_mtx;
static std::unordered_map<EGLContext, Slot*> g_slots;
static std::vector<Slot*> g_graveyard;  /* never freed: no dangling */
static EGLContext g_owner = nullptr;    /* under g_slots_mtx */
static std::mutex g_tick_mtx;  /* one ImGui drawer at a time; also
                                  serializes ownership changes and
                                  slot teardown against frames */

/* Touch events are queued by the Java hook (and by the
   NativeActivity hook) and replayed as REAL ImGui events by the
   owner thread, so a tap can no longer be eaten by the "wrong" GL
   thread. phase: 0=down 1=up 2=move (cancel folded into up). */
struct TouchEvent { float x, y; int phase; };
static std::mutex              g_touch_mtx;
static std::vector<TouchEvent> g_touch_q;
static std::atomic<bool>       g_want_mouse {false};

static std::mutex            g_char_mtx;
static std::vector<unsigned>  g_pending_chars;
static bool                   g_pending_bs    = false;
static bool                   g_want_kbd_prev = false;

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void touch_push(float x, float y, int phase) {
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    if (g_touch_q.size() > 64) g_touch_q.erase(g_touch_q.begin());
    g_touch_q.push_back({x, y, phase});
}

/* legacy boolean interface used by the AInputQueue (NativeActivity)
   hook; translated into phase events here */
static bool g_legacy_pressed = false;
void overlay_on_touch(float x, float y, bool p) {
    if (p) {
        touch_push(x, y, g_legacy_pressed ? 2 : 0);
    } else if (g_legacy_pressed) {
        touch_push(x, y, 1);
    }
    g_legacy_pressed = p;
}

extern "C" JNIEXPORT void dextool_on_touch(float x, float y, int action) {
    int a = action & 0xff;
    if      (a == 0)           touch_push(x, y, 0);  /* DOWN      */
    else if (a == 1 || a == 3) touch_push(x, y, 1);  /* UP/CANCEL */
    else if (a == 2)           touch_push(x, y, 2);  /* MOVE      */
    else if (a == 5 || a == 6) touch_push(x, y, 2);  /* PTR up/dn */
}

/* the Java side asks whether the overlay wants the current touch
   (menu hovered); if true the agent swallows the event so the game
   does not also react to menu taps */
extern "C" JNIEXPORT bool dextool_want_mouse() {
    return g_want_mouse.load();
}

extern "C" JNIEXPORT void dextool_add_char(int cp) {
    std::lock_guard<std::mutex> lk(g_char_mtx);
    g_pending_chars.push_back((unsigned)cp);
}

extern "C" JNIEXPORT void dextool_backspace() {
    std::lock_guard<std::mutex> lk(g_char_mtx);
    g_pending_bs = true;
}

static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

/* called from JNI_OnLoad before overlay_start; the
   frida thread and the bridge read g_jvm afterwards */
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
       swapping, ownership migrates to us (the old owner's ImGui is
       destroyed CPU-side only; its GL objects die with its context);
     - with no owner, the first slot to reach DT_ELECT_MIN_SWAPS
       swaps wins — that is the continuously rendering thread (the
       game), not the bursty HWUI RenderThread that only swaps on UI
       changes. */
static bool ensure_ownership(Slot* s, EGLContext ctx) {
    std::lock_guard<std::mutex> lk(g_slots_mtx);
    if (s->dead) return false;
    s->swaps++;
    long now = now_ms();
    s->last_swap_ms = now;   /* migration test needs "active" */
    Slot* ow = nullptr;
    if (g_owner) {
        auto it = g_slots.find(g_owner);
        if (it != g_slots.end() && !it->second->dead) ow = it->second;
    }
    if (ow == s) { s->last_swap_ms = now; return true; }
    if (ow) {
        bool stale = (now - ow->last_swap_ms) > DT_OWNER_STALE_MS &&
                     (now - s->last_swap_ms)  < 600;
        if (!stale) return false;     /* healthy owner elsewhere */
        if (ow->imgui) {              /* retire the old world */
            ImGui::DestroyContext(ow->imgui);
            ow->imgui = nullptr;
        }
        g_owner = nullptr;
        LOGI("[overlay] owner migrated (previous went quiet)");
    }
    if (g_owner == nullptr && s->swaps >= DT_ELECT_MIN_SWAPS) {
        g_owner = ctx;
        s->last_swap_ms = now;
        LOGI("[overlay] owner elected after %ld swaps", s->swaps);
        return true;
    }
    return false;
}

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

    LOGI("[overlay] GL: %s (%dx%d)", (const char*)ver, w, h);
    /* ImGui is compiled with IMGUI_IMPL_OPENGL_ES2, so its shader
       body is GLSL ES 100 (attribute/varying). The header must
       stay "#version 100" even on ES3 contexts — they accept
       GLSL ES 100 programs. */
    s->es3 = strstr((const char*)ver, "OpenGL ES 3") != nullptr;
    s->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(s->imgui);
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigWindowsMoveFromTitleBarOnly = true;  /* touch friendly */
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplOpenGL3_Init("#version 100");
    s->last_frame_ms = now_ms();
    LOGI("[overlay] ImGui up OK (%dx%d)", w, h);
}

/* eglDestroyContext hook: retire the slot WITH its context. The
   Slot object is never freed (graveyard), so an in-flight tick still
   holds a valid pointer and just sees dead=true on its next tick. */
static void slot_destroy(EGLContext ctx) {
    std::lock_guard<std::mutex> lk(g_tick_mtx);
    std::lock_guard<std::mutex> lk2(g_slots_mtx);
    auto it = g_slots.find(ctx);
    if (it == g_slots.end()) return;
    Slot* s = it->second;
    s->dead = true;
    g_slots.erase(it);
    g_graveyard.push_back(s);
    if (g_owner == ctx) g_owner = nullptr;
    if (s->imgui) {
        ImGui::SetCurrentContext(s->imgui);
        if (eglGetCurrentContext() == ctx)
            ImGui_ImplOpenGL3_Shutdown();  /* GL deletes need the ctx */
        ImGui::DestroyContext(s->imgui);
        ImGui::SetCurrentContext(nullptr);
        LOGI("[overlay] ImGui slot destroyed");
    }
}

/* HWUI aborts the whole process on GL_INVALID_VALUE, so every
   binding the ES2 ImGui backend touches (program, VAO, buffers,
   texture, viewport, scissor, blend, depth/stencil/cull) goes back
   exactly the way the app had it before the swap continues. */
static void gl_state_save(Slot* s) {
    GlState& g = s->gl;
    glGetIntegerv(GL_CURRENT_PROGRAM, &g.program);
    /* ab/eb must be captured on ES2 too: r6 saved them only on ES3
       and then RESTORED binding 0 on ES2, clobbering the app's
       buffer bindings every frame */
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &g.ab);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &g.eb);
    if (s->es3)
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &g.vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &g.active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &g.tex2d);
    glGetIntegerv(GL_VIEWPORT,    g.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, g.scissor_box);
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

static void gl_state_restore(Slot* s) {
    GlState& g = s->gl;
    if (s->es3) glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER,         g.ab);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.eb);
    glActiveTexture(g.active_tex);
    glBindTexture(GL_TEXTURE_2D, g.tex2d);
    glBlendEquationSeparate(g.blend_eq_rgb, g.blend_eq_a);
    glBlendFuncSeparate(g.blend_src_rgb, g.blend_dst_rgb,
                        g.blend_src_a,  g.blend_dst_a);
    if (g.blend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (g.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (g.depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (g.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (g.cull)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    glScissor(g.scissor_box[0], g.scissor_box[1],
              g.scissor_box[2], g.scissor_box[3]);
    glViewport(g.viewport[0], g.viewport[1],
               g.viewport[2], g.viewport[3]);
    glUseProgram(g.program);
}

static void overlay_tick(EGLDisplay dpy, EGLSurface surf) {
    static thread_local bool in_tick = false;
    if (in_tick) return;   /* driver re-entry (swap->swapWithDamage) */
    in_tick = true;
    struct TickReset { bool& b; ~TickReset() { b = false; } }
        tick_reset{in_tick};

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;

    /* one drawer at a time; also serializes ownership changes and
       slot teardown against in-flight frames */
    std::lock_guard<std::mutex> lk(g_tick_mtx);

    Slot* s = slot_for(ctx);
    if (!ensure_ownership(s, ctx)) return;  /* not the owner: no
                                               ImGui, no GL touched */
    if (s->dead || s->failed) return;

    if (!s->imgui) slot_init(dpy, surf, s);
    if (!s->imgui) return;
    ImGui::SetCurrentContext(s->imgui);
    ImGuiIO& io = ImGui::GetIO();
    {
        EGLint w = 1080, h = 1920;
        eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        io.DisplaySize = ImVec2((float)w, (float)h);
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

    /* replay queued touch events as real ImGui events */
    {
        std::vector<TouchEvent> q;
        {
            std::lock_guard<std::mutex> lk2(g_touch_mtx);
            q.swap(g_touch_q);
        }
        ImVec2 ds = io.DisplaySize;
        for (const TouchEvent& t : q) {
            float x = t.x < 0.f ? 0.f : (t.x > ds.x ? ds.x : t.x);
            float y = t.y < 0.f ? 0.f : (t.y > ds.y ? ds.y : t.y);
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, t.phase != 1);
        }
    }

    {
        std::lock_guard<std::mutex> lk2(g_char_mtx);
        for (unsigned cp : g_pending_chars) io.AddInputCharacter(cp);
        g_pending_chars.clear();
        if (g_pending_bs) {
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            io.AddKeyEvent(ImGuiKey_Backspace, false);
            g_pending_bs = false;
        }
    }

    menu_init_once();

    gl_state_save(s);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* the ES2 backend relies on GL_SCISSOR_TEST for clipping; the
       app may leave it disabled. Enable it for our draw — restore
       puts the app's state back afterwards. */
    glEnable(GL_SCISSOR_TEST);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    menu_draw();
    g_want_mouse.store(io.WantCaptureMouse);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    gl_state_restore(s);

    bool want_kbd = io.WantCaptureKeyboard;
    if (want_kbd != g_want_kbd_prev) {
        g_want_kbd_prev = want_kbd;
        frida_post(want_kbd ?
            "{\"type\":\"showKeyboard\"}" :
            "{\"type\":\"hideKeyboard\"}");
    }
}

static EGLBoolean my_destroy_ctx(EGLDisplay dpy, EGLContext ctx) {
    slot_destroy(ctx);
    return g_orig_destroy_ctx(dpy, ctx);
}

static EGLBoolean my_swap(EGLDisplay dpy, EGLSurface surf) {
    overlay_tick(dpy, surf);
    return g_orig_swap(dpy, surf);
}

static EGLBoolean my_swap_dmg(EGLDisplay dpy, EGLSurface surf,
                              EGLint* rects, EGLint n) {
    overlay_tick(dpy, surf);
    return g_orig_swap_dmg(dpy, surf, rects, n);
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
        else        LOGE("DobbyHook eglDestroyContext: %d", r);
    }
    return ok;
}
