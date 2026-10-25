#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include <GLES3/gl3.h>

#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int  gWidth = 0;
static int  gHeight = 0;

static void setupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 10.0f;
    style.FrameRounding     = 6.0f;
    style.GrabRounding      = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize  = 0.0f;
    style.WindowPadding     = ImVec2(14, 14);
    style.ItemSpacing       = ImVec2(10, 8);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]           = ImVec4(0.06f, 0.08f, 0.10f, 0.96f);
    c[ImGuiCol_Border]             = ImVec4(1.00f, 0.78f, 0.34f, 0.30f);
    c[ImGuiCol_TitleBg]            = ImVec4(0.06f, 0.08f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.35f, 0.27f, 0.05f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.06f, 0.08f, 0.10f, 1.00f);
    c[ImGuiCol_Text]               = ImVec4(0.92f, 0.93f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.52f, 0.55f, 1.00f);
    c[ImGuiCol_FrameBg]            = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.25f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_Button]             = ImVec4(1.00f, 0.78f, 0.34f, 0.55f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(1.00f, 0.78f, 0.34f, 0.80f);
    c[ImGuiCol_ButtonActive]       = ImVec4(1.00f, 0.78f, 0.34f, 1.00f);
    c[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrab]         = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive]   = ImVec4(0.40f, 0.92f, 0.98f, 1.00f);
    c[ImGuiCol_Separator]          = ImVec4(1.00f, 0.78f, 0.34f, 0.20f);
    c[ImGuiCol_Header]             = ImVec4(1.00f, 0.78f, 0.34f, 0.30f);
    c[ImGuiCol_HeaderHovered]      = ImVec4(1.00f, 0.78f, 0.34f, 0.50f);
    c[ImGuiCol_HeaderActive]       = ImVec4(1.00f, 0.78f, 0.34f, 0.70f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_tool_OverlayView_nativeInit(JNIEnv* env, jobject, jobject surface) {
    if (gInitialized) { LOGI("Already initialized"); return; }
    if (!surface)     { LOGE("Surface null");     return; }

    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow)     { LOGE("ANativeWindow_fromSurface failed"); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;    // don't write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    setupStyle();

    if (!ImGui_ImplAndroid_Init(gWindow)) {
        LOGE("ImGui_ImplAndroid_Init failed");
        return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        LOGE("ImGui_ImplOpenGL3_Init failed");
        return;
    }

    gInitialized = true;
    LOGI("ImGui initialized OK");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_tool_OverlayView_nativeResize(JNIEnv*, jobject, jint w, jint h) {
    gWidth = w; gHeight = h;
    glViewport(0, 0, w, h);
    LOGI("Resize %dx%d", w, h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_tool_OverlayView_nativeRender(JNIEnv*, jobject) {
    if (!gInitialized) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos (ImVec2(40, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(650, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("ImGui Tool Menu");

    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.34f, 1.0f), "Hello from ImGui on Android");
    ImGui::Separator();
    ImGui::Text("Drag me, click buttons, slide the slider.");
    ImGui::Text("Touch the app text behind me -> it should respond.");
    ImGui::Spacing();

    static float slider = 42.0f;
    ImGui::SliderFloat("Value", &slider, 0.0f, 100.0f, "%.1f");

    static bool a = false, b = false, c = false;
    ImGui::Checkbox("Feature A", &a);
    ImGui::Checkbox("Feature B", &b);
    ImGui::Checkbox("Feature C", &c);

    ImGui::Spacing();
    if (ImGui::Button("Do something", ImVec2(180, 0))) {
        LOGI("Button clicked! slider=%.1f a=%d b=%d c=%d",
             slider, (int)a, (int)b, (int)c);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) { slider = 0.0f; a = b = c = false; }

    ImGui::End();

    ImGui::Render();

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // transparent
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_tool_OverlayView_nativeTouch(JNIEnv*, jobject, jint action, jfloat x, jfloat y) {
    if (!gInitialized) return JNI_FALSE;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(x, y);

    if      (action == 0) io.AddMouseButtonEvent(0, true);   // DOWN
    else if (action == 1) io.AddMouseButtonEvent(0, false);  // UP
    else if (action == 3) io.AddMouseButtonEvent(0, false);  // CANCEL

    // Tell Android whether ImGui wants this touch.
    // WantCaptureMouse is from the LAST frame (1-frame latency). Good enough for now.
    return io.WantCaptureMouse ? JNI_TRUE : JNI_FALSE;
}
