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
#include <dlfcn.h>
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <mutex>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_swap_dmg)(EGLDisplay, EGLSurface, EGLint*, EGLint);

static fn_swap     g_orig_swap     = nullptr;
static fn_swap_dmg g_orig_swap_dmg = nullptr;

static std::atomic<bool> g_gum_inited  {false};
static std::atomic<bool> g_imgui_up    {false};
static bool              g_imgui_failed = false;
static JavaVM*           g_jvm          = nullptr;
static EGLContext        g_egl_ctx      = EGL_NO_CONTEXT;

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

static void imgui_shutdown() {
    if (g_imgui_up.exchange(false)) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_imgui_failed = false;
        LOGI("[overlay] ImGui shutdown (context lost)");
    }
}

static void imgui_init(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;

    EGLint w = 1080, h = 1920;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const GLubyte* ver = glGetString(GL_VERSION);
    if (!ver || !strstr((const char*)ver, "OpenGL ES")) {
        LOGE("[overlay] not OpenGL ES: %s", ver ? (const char*)ver : "null");
        g_imgui_failed = true;
        return;
    }

    LOGI("[overlay] GL: %s (%dx%d)", (const char*)ver, w, h);
    /* ImGui is compiled with IMGUI_IMPL_OPENGL_ES2, so its shader
       body is GLSL ES 100 (attribute/varying). The header must
       stay "#version 100" even on ES3 contexts — they accept
       GLSL ES 100 programs. */
    ImGui::CreateContext();
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplOpenGL3_Init("#version 100");
    menu_init();
    g_imgui_up.store(true);
    LOGI("[overlay] ImGui up OK (%dx%d)", w, h);
}

static void overlay_tick(EGLDisplay dpy, EGLSurface surf) {
    static thread_local bool in_tick = false;
    if (in_tick) return;   /* driver re-entry (swap->swapWithDamage) */
    in_tick = true;
    struct TickReset { bool& b; ~TickReset() { b = false; } }
        tick_reset{in_tick};
    EGLContext ctx = eglGetCurrentContext();

    if (ctx != EGL_NO_CONTEXT && ctx != g_egl_ctx) {
        if (g_egl_ctx != EGL_NO_CONTEXT) {
            LOGI("[overlay] EGL context changed, re-init ImGui");
            imgui_shutdown();
        }
        g_egl_ctx = ctx;
        imgui_init(dpy, surf);
    }

    if (!g_imgui_up.load() || g_imgui_failed) return;

    ImGuiIO& io = ImGui::GetIO();
    {
        EGLint w = 1080, h = 1920;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
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

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    menu_draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
    return ok;
}
