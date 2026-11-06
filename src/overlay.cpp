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

// ── Thread-safe input queue ─────────────────────────────────
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
    e.pressed = (action == 0 /*DOWN*/ || action == 2 /*MOVE*/);
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

// ── Worker thread ───────────────────────────────────────────
static void* frida_thread(void*) {
    frida_bridge_start(g_jvm);
    return nullptr;
}

// ── ImGui lifecycle ─────────────────────────────────────────
static void imgui_shutdown() {
    if (g_imgui_up.load()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_imgui_up.store(false);
        g_imgui_failed = false;
        LOGI("[overlay] ImGui shut down (context loss)");
    }
}

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

    ImGui_ImplOpenGL3_Init("#version 100");
    menu_init();
    g_imgui_up.store(true);
    LOGI("[overlay] ImGui up (%dx%d)", w, h);
}

// ── Key mapping ─────────────────────────────────────────────
static ImGuiKey map_key(int kc) {
    switch (kc) {
        case 67:  return ImGuiKey_Backspace;   // KEYCODE_DEL
        case 66:  return ImGuiKey_Enter;       // KEYCODE_ENTER
        case 4:   return ImGuiKey_Escape;      // KEYCODE_BACK
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

// ── Drain queued input into ImGui (called on GL thread) ─────
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
                    bool down = (e.action == 0 /*DOWN*/);
                    io.AddKeyEvent(k, down);
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

// ── Shared frame work ────────────────────────────────────────
static void render_frame(EGLDisplay dpy, EGLSurface surf) {
    EGLContext cur_ctx = eglGetCurrentContext();
    if (!g_gum_inited.exchange(true)) {
        g_egl_ctx = cur_ctx;
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
    } else if (cur_ctx != EGL_NO_CONTEXT && cur_ctx != g_egl_ctx) {
        LOGI("[overlay] EGL context changed, re-initing ImGui");
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

    // Notify agent to show/hide soft keyboard if WantTextInput changed.
    bool want = ImGui::GetIO().WantTextInput;
    if (want != g_want_text_last.load()) {
        g_want_text_last.store(want);
        frida_post(want
            ? "{\"type\":\"keyboard\",\"show\":true}"
            : "{\"type\":\"keyboard\",\"show\":false}");
    }
}

// ── Hooked entry points ────────────────────────────────────
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
    if (s1) {
        if (DobbyHook(s1,
                reinterpret_cast<dobby_dummy_func_t>(my_swap),
                reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap)) == 0) {
            LOGI("eglSwapBuffers hooked OK");
            hooked++;
        }
    }

    void* s2 = dlsym(egl, "eglSwapBuffersWithDamageKHR");
    if (s2) {
        if (DobbyHook(s2,
                reinterpret_cast<dobby_dummy_func_t>(my_swap_khr),
                reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap_khr)) == 0) {
            LOGI("eglSwapBuffersWithDamageKHR hooked OK");
            hooked++;
        }
    }

    void* s3 = dlsym(egl, "eglSwapBuffersWithDamageEXT");
    if (s3) {
        if (DobbyHook(s3,
                reinterpret_cast<dobby_dummy_func_t>(my_swap_ext),
                reinterpret_cast<dobby_dummy_func_t*>(&g_orig_swap_ext)) == 0) {
            LOGI("eglSwapBuffersWithDamageEXT hooked OK");
            hooked++;
        }
    }

    return hooked > 0;
}
