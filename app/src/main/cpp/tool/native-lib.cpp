#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include <GLES3/gl3.h>
#include "dex_data.h"

#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <mutex>

#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* gVM = nullptr;
static jclass gHelperClass = nullptr;
static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int gWidth = 0, gHeight = 0;

static std::vector<std::string> gClassList;
static std::mutex gClassListMutex;
static char gFilter[128] = {0};

static bool loadDex(JNIEnv* env) {
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(clCls, getSys);

    jclass imCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!imCls) { LOGE("InMemoryDexClassLoader missing"); return false; }
    jmethodID ctor = env->GetMethodID(imCls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject buf = env->NewDirectByteBuffer((void*)imgui_dex, imgui_dex_len);
    jobject loader = env->NewObject(imCls, ctor, buf, parent);
    if (!loader) { LOGE("Dex loader ctor failed"); return false; }

    jclass lCls = env->GetObjectClass(loader);
    jmethodID loadM = env->GetMethodID(lCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.menu.imgui.Helper");
    jclass hCls = (jclass)env->CallObjectMethod(loader, loadM, name);
    if (!hCls) { LOGE("Helper not in DEX"); return false; }

    gHelperClass = (jclass)env->NewGlobalRef(hCls);
    LOGI("DEX loaded, Helper class OK");
    return true;
}

static jobject getCurrentActivity(JNIEnv* env) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) return nullptr;
    jmethodID cur = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject at = env->CallStaticObjectMethod(atCls, cur);
    if (!at) return nullptr;
    jfieldID f = env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
    if (!f) return nullptr;
    jobject acts = env->GetObjectField(at, f);
    if (!acts) return nullptr;
    jclass amCls = env->FindClass("android/util/ArrayMap");
    jmethodID sizeM = env->GetMethodID(amCls, "size", "()I");
    jmethodID valAt = env->GetMethodID(amCls, "valueAt", "(I)Ljava/lang/Object;");
    jint n = env->CallIntMethod(acts, sizeM);
    for (jint i = 0; i < n; i++) {
        jobject rec = env->CallObjectMethod(acts, valAt, i);
        if (!rec) continue;
        jclass rCls = env->GetObjectClass(rec);
        jfieldID actF = env->GetFieldID(rCls, "activity", "Landroid/app/Activity;");
        if (!actF) continue;
        jobject a = env->GetObjectField(rec, actF);
        if (a) return a;
    }
    return nullptr;
}

static void* pollThread(void*) {
    JNIEnv* env;
    gVM->AttachCurrentThread(&env, nullptr);
    jobject activity = nullptr;
    for (int i = 0; i < 200; i++) {
        activity = getCurrentActivity(env);
        if (activity) break;
        usleep(100 * 1000);
    }
    if (activity && gHelperClass) {
        jmethodID attach = env->GetStaticMethodID(gHelperClass, "attach", "(Landroid/content/Context;)V");
        if (attach) {
            env->CallStaticVoidMethod(gHelperClass, attach, activity);
            LOGI("Helper.attach dispatched");
        }
    } else {
        LOGE("Activity never found - menu not attached");
    }
    gVM->DetachCurrentThread();
    return nullptr;
}

static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding=10; s.FrameRounding=6; s.GrabRounding=6;
    s.WindowBorderSize=0; s.WindowPadding=ImVec2(14,14); s.ItemSpacing=ImVec2(10,8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.06f,0.08f,0.10f,0.96f);
    c[ImGuiCol_Border]        = ImVec4(1.00f,0.78f,0.34f,0.30f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.06f,0.08f,0.10f,1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.35f,0.27f,0.05f,1.00f);
    c[ImGuiCol_Text]          = ImVec4(0.92f,0.93f,0.94f,1.00f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.14f,0.16f,0.19f,1.00f);
    c[ImGuiCol_Button]        = ImVec4(1.00f,0.78f,0.34f,0.55f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.00f,0.78f,0.34f,0.80f);
    c[ImGuiCol_ButtonActive]  = ImVec4(1.00f,0.78f,0.34f,1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.30f,0.82f,0.88f,1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.30f,0.82f,0.88f,1.00f);
    c[ImGuiCol_Header]        = ImVec4(1.00f,0.78f,0.34f,0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.00f,0.78f,0.34f,0.50f);
}

static void doInit(JNIEnv* env, jclass, jobject surface) {
    if (gInitialized || !surface) return;
    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow) return;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    setupStyle();
    if (!ImGui_ImplAndroid_Init(gWindow)) { LOGE("Android backend fail"); return; }
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) { LOGE("GL3 backend fail"); return; }
    gInitialized = true;
    LOGI("ImGui ready");
}
static void doResize(JNIEnv*, jclass, jint w, jint h) { gWidth=w; gHeight=h; glViewport(0,0,w,h); }
static void doRender(JNIEnv*, jclass) {
    if (!gInitialized) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(40,80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720,560), ImGuiCond_FirstUseEver);
    ImGui::Begin("ImGui Tool");

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Main")) {
            ImGui::TextColored(ImVec4(1,0.78f,0.34f,1), "Self-contained overlay");
            ImGui::Separator();
            static float s = 42.0f;
            ImGui::SliderFloat("Value", &s, 0.0f, 100.0f, "%.1f");
            static bool a=false,b=false;
            ImGui::Checkbox("Option A", &a);
            ImGui::Checkbox("Option B", &b);
            if (ImGui::Button("Log")) LOGI("s=%.1f a=%d b=%d", s, (int)a, (int)b);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Classes")) {
            size_t total;
            { std::lock_guard<std::mutex> lk(gClassListMutex); total = gClassList.size(); }
            ImGui::Text("Classes in APK: %zu", total);
            ImGui::InputTextWithHint("##filter","Filter...",gFilter,sizeof(gFilter));
            ImGui::Separator();
            int shown = 0;
            ImGui::BeginChild("##list", ImVec2(0,0), true);
            {
                std::lock_guard<std::mutex> lk(gClassListMutex);
                for (const auto& n : gClassList) {
                    if (gFilter[0] && n.find(gFilter) == std::string::npos) continue;
                    ImGui::Selectable(n.c_str());
                    shown++;
                }
            }
            ImGui::EndChild();
            ImGui::Text("Showing %d / %zu", shown, total);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    ImGui::Render();
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
static jboolean doTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (!gInitialized) return JNI_FALSE;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(x, y);
    if (action == 0) io.AddMouseButtonEvent(0, true);
    else if (action == 1 || action == 3) io.AddMouseButtonEvent(0, false);
    return io.WantCaptureMouse ? JNI_TRUE : JNI_FALSE;
}
static void doSetClassList(JNIEnv* env, jclass, jobjectArray arr) {
    std::lock_guard<std::mutex> lk(gClassListMutex);
    gClassList.clear();
    if (!arr) return;
    jsize n = env->GetArrayLength(arr);
    gClassList.reserve((size_t)n);
    for (jsize i = 0; i < n; i++) {
        jstring js = (jstring)env->GetObjectArrayElement(arr, i);
        const char* cs = env->GetStringUTFChars(js, nullptr);
        if (cs) { gClassList.emplace_back(cs); env->ReleaseStringUTFChars(js, cs); }
        env->DeleteLocalRef(js);
    }
    LOGI("Class list stored: %zu", gClassList.size());
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env; vm->GetEnv((void**)&env, JNI_VERSION_1_6); gVM = vm;
    if (!loadDex(env)) return JNI_VERSION_1_6;
    JNINativeMethod methods[] = {
        {(char*)"nativeInit",         (char*)"(Landroid/view/Surface;)V", (void*)doInit},
        {(char*)"nativeResize",       (char*)"(II)V",                    (void*)doResize},
        {(char*)"nativeRender",       (char*)"()V",                      (void*)doRender},
        {(char*)"nativeTouch",        (char*)"(IFF)Z",                    (void*)doTouch},
        {(char*)"nativeSetClassList", (char*)"([Ljava/lang/String;)V",    (void*)doSetClassList},
    };
    env->RegisterNatives(gHelperClass, methods, 5);
    LOGI("Natives registered");

    pthread_t t;
    if (pthread_create(&t, nullptr, pollThread, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
