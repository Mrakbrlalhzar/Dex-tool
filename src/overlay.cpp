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
#include <pthread.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_swap_dmg)(EGLDisplay, EGLSurface, EGLint*, EGLint);
typedef EGLBoolean (*fn_destroy_ctx)(EGLDisplay, EGLContext);

static fn_swap     g_orig_swap     = nullptr;
static fn_swap_dmg g_orig_swap_dmg = nullptr;
static fn_destroy_ctx g_orig_destroy_ctx = nullptr;

static std::atomic<bool> g_gum_inited {false};
static JavaVM*           g_jvm        = nullptr;
          
/* One ImGui world per EGL context. The app has TWO GL threads
   (HWUI's RenderThread + GLSurfaceView's GLThread), each with its
   own EGL context, and BOTH call eglSwapBuffers. A single global
   ImGuiContext torn down on "context changed" crashed the other
   thread mid-frame. GL objects belong to their context, so no
   shutdown on switch — objects die with the context. */
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
    bool failed = false;
    bool es3 = false;
    GlState gl;
};
static std::mutex g_slots_mtx;
static std::unordered_map<EGLContext, Slot*> g_slots;
static std::mutex g_tick_mtx;   /* GImGui is not thread-local */
          
struct Touch {
    std::atomic<float> x{0}, y{0};
    std::atomic<bool> pressed{false}, dirty{false};
};
static Touch g_touch;

static std::mutex            g_char_mtx;
static std::vector<unsigned> g_pending_chars;
static bool                  g_pending_bs    = false;
static bool                  g_want_kbd_prev = false;

void overlay_set_java_vm(JavaVM* vm) { g_jvm = vm; }

void overlay_on_touch(float x, float y, bool p) {
    g_touch.x = x; g_touch.y = y;
    g_touch.pressed = p; g_touch.dirty = true;
}

extern "C" JNIEXPORT void dextool_on_touch(float x, float y, int action) {
    overlay_on_touch(x, y, action == 0 || action == 2);
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
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplOpenGL3_Init("#version 100");
    LOGI("[overlay] ImGui up OK (%dx%d)", w, h);
}
          
/* called from the eglDestroyContext hook: retire the slot WITH
   its context, never on a mere context switch */
static void slot_destroy(EGLContext ctx) {
    std::lock_guard<std::mutex> lk(g_tick_mtx);
    std::lock_guard<std::mutex> lk2(g_slots_mtx);
    auto it = g_slots.find(ctx);
    if (it == g_slots.end()) return;
    Slot* s = it->second;
    g_slots.erase(it);
    if (s->imgui) {
        ImGui::SetCurrentContext(s->imgui);
        if (eglGetCurrentContext() == ctx)
            ImGui_ImplOpenGL3_Shutdown();  /* GL deletes need the ctx */
        ImGui::DestroyContext(s->imgui);
        ImGui::SetCurrentContext(nullptr);
        LOGI("[overlay] ImGui slot destroyed");
    }
    delete s;
}
          
/* HWUI aborts the whole process on GL_INVALID_VALUE, so every
   binding the ES2 ImGui backend touches (program, VAO, buffers,
   texture, viewport, scissor, blend, depth/stencil/cull) goes back
   exactly the way the app had it before the swap continues. */
static void gl_state_save(Slot* s) {
    GlState& g = s->gl;
    glGetIntegerv(GL_CURRENT_PROGRAM, &g.program);
    if (s->es3) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &g.vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &g.ab);
        if (g.vao == 0)
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &g.eb);
    }
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
    glBindBuffer(GL_ARRAY_BUFFER, g.ab);
    if (!s->es3 || g.vao == 0)
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
              
    /* GImGui is process-global: the two GL threads must not draw
       at once. Frames are vsync-paced, contention is negligible. */
    std::lock_guard<std::mutex> lk(g_tick_mtx);
              
    Slot* s = slot_for(ctx);
    if (s->failed) return;
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
              
    if (g_touch.dirty.exchange(false)) {
        io.MousePos     = ImVec2(g_touch.x.load(), g_touch.y.load());
        io.MouseDown[0] = g_touch.pressed.load();
    }
              
    {
        std::lock_guard<std::mutex> lk(g_char_mtx);
        for (unsigned cp : g_pending_chars) io.AddInputCharacter(cp);
        g_pending_chars.clear();
        if (g_pending_bs) {
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            io.AddKeyEvent(ImGuiKey_Backspace, false);
            g_pending_bs = false;
        }
    }
              
    bool want_kbd = io.WantCaptureKeyboard;
    if (want_kbd != g_want_kbd_prev) {
        g_want_kbd_prev = want_kbd;
        frida_post(want_kbd ?
            "{\"type\":\"showKeyboard\"}" :
            "{\"type\":\"hideKeyboard\"}");
    }
              
    menu_init_once();
              
    gl_state_save(s);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    menu_draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    gl_state_restore(s);
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
