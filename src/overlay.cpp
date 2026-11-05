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
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <cstring>
#include <pthread.h>
#include <atomic>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
static fn_swap              g_orig_swap    = nullptr;
static std::atomic<bool>    g_gum_inited  {false};
static std::atomic<bool>    g_imgui_up    {false};
static bool                 g_imgui_failed = false;
static JavaVM*              g_jvm          = nullptr;

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

static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

static void imgui_init(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 1080, h = 1920;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    const GLubyte* ver = glGetString(GL_VERSION);
    if (!ver || !strstr((const char*)ver, "OpenGL ES 3")) {
        LOGE("ImGui disabled: not ES3 (ver=%s)", ver ? (const char*)ver : "null");
        g_imgui_failed = true;
        return;
    }
    ImGui::CreateContext();
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    menu_init();
    g_imgui_up = true;
    LOGI("ImGui up (%dx%d ES3)", w, h);
}

static EGLBoolean my_swap(EGLDisplay dpy, EGLSurface surf) {
    if (!g_gum_inited.exchange(true)) {
        gum_init_embedded();
        log_info("[overlay] gum_init_embedded done");
        pthread_t ft;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&ft, &a, frida_thread, nullptr);
        pthread_attr_destroy(&a);
        touch_install_hooks();
        imgui_init(dpy, surf);
    }
    if (g_imgui_up && !g_imgui_failed) {
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
    if (r != 0) { LOGE("DobbyHook failed: %d", r); return false; }
    LOGI("eglSwapBuffers hooked OK");
    return true;
}
