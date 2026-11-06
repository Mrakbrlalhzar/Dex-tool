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

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
static fn_swap              g_orig_swap     = nullptr;
static std::atomic<bool>    g_gum_inited   {false};
static std::atomic<bool>    g_imgui_up     {false};
static bool                 g_imgui_failed  = false;
static JavaVM*              g_jvm           = nullptr;

// FIX 1: Track EGL context to detect pause/resume context recreation.
static EGLContext           g_egl_ctx       = EGL_NO_CONTEXT;

struct Touch {
    std::atomic<float> x{0}, y{0};
    std::atomic<bool> pressed{false}, dirty{false};
};
static Touch g_touch;

void overlay_set_java_vm(JavaVM* vm) { g_jvm = vm; }

void overlay_on_touch(float x, float y, bool p) {
    g_touch.x = x; g_touch.y = y;
    g_touch.pressed = p; g_touch.dirty = true;
}

// FIX 3: Exported C symbol for GumJS agent to call via NativeFunction.
// The agent hooks Activity.dispatchTouchEvent in Java and calls this.
// action: 0=DOWN, 1=UP, 2=MOVE (matches MotionEvent constants).
extern "C" JNIEXPORT void dextool_on_touch(float x, float y, int action) {
    overlay_on_touch(x, y, action == 0 || action == 2);
}

static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

// FIX 1: Tear down ImGui GL resources before context switch.
static void imgui_shutdown() {
    if (g_imgui_up.load()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_imgui_up.store(false);
        g_imgui_failed = false;
        LOGI("[overlay] ImGui shut down (context loss)");
    }
}

// FIX 2: Accept any OpenGL ES (ES2 or ES3). Use #version 100 GLSL.
// The binary is compiled with IMGUI_IMPL_OPENGL_ES2=1 so ImGui always
// uses ES2 function set, which is a subset of ES3.
static void imgui_init(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 1080, h = 1920;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const GLubyte* ver = glGetString(GL_VERSION);
    bool is_es = ver && strstr((const char*)ver, "OpenGL ES");
    if (!is_es) {
        LOGE("[overlay] GL is not OpenGL ES (ver=%s), ImGui disabled",
             ver ? (const char*)ver : "null");
        g_imgui_failed = true;
        return;
    }
    LOGI("[overlay] GL: %s (%dx%d)", (const char*)ver, w, h);

    ImGui::CreateContext();
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    // "#version 100" = GLSL ES 1.00, works on ES2 and ES3.
    ImGui_ImplOpenGL3_Init("#version 100");
    menu_init();
    g_imgui_up.store(true);
    LOGI("[overlay] ImGui up (%dx%d)", w, h);
}

// CRITICAL: calls g_orig_swap — NEVER recurses into eglSwapBuffers symbol.
// FIX 1: eglGetCurrentContext() detects context change after pause/resume.
static EGLBoolean my_swap(EGLDisplay dpy, EGLSurface surf) {
    EGLContext cur_ctx = eglGetCurrentContext();

    if (!g_gum_inited.exchange(true)) {
        // ── First call: full initialization ──────────────────────
        g_egl_ctx = cur_ctx;
        gum_init_embedded();   // FIRST gum_* call, GL context valid
        log_info("[overlay] gum_init_embedded done");

        pthread_t ft;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&ft, &a, frida_thread, nullptr);
        pthread_attr_destroy(&a);

        touch_install_hooks();   // NativeActivity fallback
        imgui_init(dpy, surf);

    } else if (cur_ctx != EGL_NO_CONTEXT && cur_ctx != g_egl_ctx) {
        // ── Context changed after pause/resume: re-init ImGui ─────
        LOGI("[overlay] EGL context changed (%p -> %p), re-initing ImGui",
             g_egl_ctx, cur_ctx);
        g_egl_ctx = cur_ctx;
        imgui_shutdown();
        imgui_init(dpy, surf);
    }

    if (g_imgui_up.load() && !g_imgui_failed) {
        ImGuiIO& io = ImGui::GetIO();
        if (g_touch.dirty.exchange(false)) {
            io.MousePos     = ImVec2(g_touch.x.load(), g_touch.y.load());
            io.MouseDown[0] = g_touch.pressed.load();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        menu_draw();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    return g_orig_swap(dpy, surf);
}

bool install_swap_hook() {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { LOGE("dlopen libEGL.so: %s", dlerror()); return false; }
    void* sym = dlsym(egl, "eglSwapBuffers");
    if (!sym) { LOGE("dlsym eglSwapBuffers failed"); return false; }
    int r = DobbyHook(sym,
                      reinterpret_cast<dobby_dummy_func_t>(my_swap),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap));
    if (r != 0) { LOGE("DobbyHook eglSwapBuffers failed: %d", r); return false; }
    LOGI("eglSwapBuffers hooked OK");
    return true;
}
