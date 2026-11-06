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
#include <mutex>
#include <vector>

typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_swap_dmg)(EGLDisplay, EGLSurface, const EGLint*, EGLint);

static fn_swap        g_orig_swap     = nullptr;
static fn_swap_dmg    g_orig_swap_khr = nullptr;
static fn_swap_dmg    g_orig_swap_ext = nullptr;
static std::atomic<bool> g_gum_inited   {false};
static std::atomic<bool> g_imgui_up     {false};
static bool              g_imgui_failed  = false;
static JavaVM*           g_jvm           = nullptr;
static EGLContext        g_egl_ctx       = EGL_NO_CONTEXT;
static std::atomic<bool> g_want_text_last{false};

struct Evt {
    enum Kind { TOUCH, KEY, TEXT, BACKSPACE } kind;
    float x = 0, y = 0;
    bool  pressed = false;
    int   keycode = 0, action = 0;
    char  text[128] = {};
};
static std::mutex       g_in_mtx;
static std::vector<Evt> g_in_q;

static void push_evt(const Evt& e) {
    std::lock_guard<std::mutex> lk(g_in_mtx);
    if (g_in_q.size() > 256) g_in_q.erase(g_in_q.begin());
    g_in_q.push_back(e);
}

void overlay_set_java_vm(JavaVM* vm) { g_jvm = vm; }

void overlay_on_touch(float x, float y, bool p) {
    Evt e; e.kind = Evt::TOUCH; e.x = x; e.y = y; e.pressed = p;
    push_evt(e);
}

extern "C" JNIEXPORT void dextool_on_touch(float x, float y, int action) {
    Evt e; e.kind = Evt::TOUCH; e.x = x; e.y = y;
    e.pressed = (action == 0 || action == 2);
    push_evt(e);
}
extern "C" JNIEXPORT void dextool_on_key(int keycode, int action) {
    Evt e; e.kind = Evt::KEY; e.keycode = keycode; e.action = action;
    push_evt(e);
}
extern "C" JNIEXPORT void dextool_on_text(const char* utf8) {
    if (!utf8) return;
    Evt e; e.kind = Evt::TEXT;
    strncpy(e.text, utf8, sizeof(e.text) - 1);
    push_evt(e);
}
extern "C" JNIEXPORT void dextool_on_backspace(void) {
    Evt e; e.kind = Evt::BACKSPACE;
    push_evt(e);
}

static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

static void spawn_frida_thread() {
    log_info("[overlay] spawning frida thread (16MB stack)");
    pthread_t ft;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 16 * 1024 * 1024);
    int rc = pthread_create(&ft, &a, frida_thread, nullptr);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        char m[64]; snprintf(m, sizeof(m), "[overlay] pthread_create rc=%d", rc);
        log_crash_raw(m);
    }
}

static void imgui_shutdown() {
    if (g_imgui_up.load()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_imgui_up.store(false);
        g_imgui_failed = false;
        log_info("[overlay] ImGui shut down (context loss)");
    }
}

static void imgui_init(EGLDisplay dpy, EGLSurface surf) {
    log_info("[overlay] imgui_init entry");
    EGLint w = 1080, h = 1920;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const GLubyte* ver = glGetString(GL_VERSION);
    bool is_es = ver && strstr((const char*)ver, "OpenGL ES");
    if (!is_es) {
        log_info("[overlay] GL not OpenGL ES, ImGui disabled");
        g_imgui_failed = true;
        return;
    }
    log_info("[overlay] imgui_init creating ImGui context");
    ImGui::CreateContext();
    log_info("[overlay] imgui_init context created");
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    ImGui_ImplOpenGL3_Init("#version 100");
    log_info("[overlay] imgui_init GL backend OK, menu_init");
    menu_init();
    g_imgui_up.store(true);
    log_info("[overlay] imgui_init done");
}

static ImGuiKey map_key(int kc) {
    switch (kc) {
        case 67:  return ImGuiKey_Backspace;
        case 66:  return ImGuiKey_Enter;
        case 4:   return ImGuiKey_Escape;
        case 19:  return ImGuiKey_UpArrow;
        case 20:  return ImGuiKey_DownArrow;
        case 21:  return ImGuiKey_LeftArrow;
        case 22:  return ImGuiKey_RightArrow;
        case 61:  return ImGuiKey_Tab;
        case 112: return ImGuiKey_Delete;
        case 122: return ImGuiKey_Home;
        case 123: return ImGuiKey_End;
        default:  return ImGuiKey_None;
    }
}

static void drain_input() {
    std::lock_guard<std::mutex> lk(g_in_mtx);
    if (g_in_q.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& e : g_in_q) {
        switch (e.kind) {
            case Evt::TOUCH:
                io.AddMousePosEvent(e.x, e.y);
                io.AddMouseButtonEvent(0, e.pressed);
                break;
            case Evt::KEY: {
                ImGuiKey k = map_key(e.keycode);
                if (k != ImGuiKey_None) {
                    io.AddKeyEvent(k, e.action == 0);
                }
                break;
            }
            case Evt::TEXT:
                io.AddInputCharactersUTF8(e.text);
                break;
            case Evt::BACKSPACE:
                io.AddKeyEvent(ImGuiKey_Backspace, true);
                io.AddKeyEvent(ImGuiKey_Backspace, false);
                break;
        }
    }
    g_in_q.clear();
}

static void render_frame(EGLDisplay dpy, EGLSurface surf) {
    EGLContext cur_ctx = eglGetCurrentContext();
    if (!g_gum_inited.exchange(true)) {
        g_egl_ctx = cur_ctx;
        log_info("[overlay] first swap: gum_init_embedded");
        gum_init_embedded();
        log_info("[overlay] gum_init_embedded done");
        // Do ImGui init FIRST — it is entirely GL-side, and running it
        // before the frida thread removes the init race on GLib state.
        log_info("[overlay] touch_install_hooks");
        touch_install_hooks();
        log_info("[overlay] imgui_init (before frida spawn)");
        imgui_init(dpy, surf);
        // Only after everything local is set up do we spawn GumJS.
        spawn_frida_thread();
        log_info("[overlay] first-frame setup complete");
    } else if (cur_ctx != EGL_NO_CONTEXT && cur_ctx != g_egl_ctx) {
        log_info("[overlay] EGL context changed, re-initing ImGui");
        g_egl_ctx = cur_ctx;
        imgui_shutdown();
        imgui_init(dpy, surf);
    }

    if (!g_imgui_up.load() || g_imgui_failed) return;

    drain_input();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    menu_draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    bool want = ImGui::GetIO().WantTextInput;
    if (want != g_want_text_last.load()) {
        g_want_text_last.store(want);
        frida_post(want
            ? "{\"type\":\"keyboard\",\"show\":true}"
            : "{\"type\":\"keyboard\",\"show\":false}");
    }
}

static EGLBoolean my_swap(EGLDisplay dpy, EGLSurface surf) {
    render_frame(dpy, surf);
    return g_orig_swap(dpy, surf);
}
static EGLBoolean my_swap_khr(EGLDisplay dpy, EGLSurface surf,
                              const EGLint* rects, EGLint n) {
    render_frame(dpy, surf);
    if (g_orig_swap_khr) return g_orig_swap_khr(dpy, surf, rects, n);
    if (g_orig_swap)     return g_orig_swap(dpy, surf);
    return EGL_FALSE;
}
static EGLBoolean my_swap_ext(EGLDisplay dpy, EGLSurface surf,
                              const EGLint* rects, EGLint n) {
    render_frame(dpy, surf);
    if (g_orig_swap_ext) return g_orig_swap_ext(dpy, surf, rects, n);
    if (g_orig_swap_khr) return g_orig_swap_khr(dpy, surf, rects, n);
    if (g_orig_swap)     return g_orig_swap(dpy, surf);
    return EGL_FALSE;
}

bool install_swap_hook() {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { LOGE("dlopen libEGL.so: %s", dlerror()); return false; }
    int hooked = 0;
    void* s1 = dlsym(egl, "eglSwapBuffers");
    if (s1 && DobbyHook(s1,
            reinterpret_cast<dobby_dummy_func_t>(my_swap),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap)) == 0) {
        LOGI("eglSwapBuffers hooked"); hooked++;
    }
    void* s2 = dlsym(egl, "eglSwapBuffersWithDamageKHR");
    if (s2 && DobbyHook(s2,
            reinterpret_cast<dobby_dummy_func_t>(my_swap_khr),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap_khr)) == 0) {
        LOGI("eglSwapBuffersWithDamageKHR hooked"); hooked++;
    }
    void* s3 = dlsym(egl, "eglSwapBuffersWithDamageEXT");
    if (s3 && DobbyHook(s3,
            reinterpret_cast<dobby_dummy_func_t>(my_swap_ext),
            reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap_ext)) == 0) {
        LOGI("eglSwapBuffersWithDamageEXT hooked"); hooked++;
    }
    return hooked > 0;
}
