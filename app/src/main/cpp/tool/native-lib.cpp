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
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <unwind.h>

#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* gVM = nullptr;
static jclass gHelperClass = nullptr;
static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int gWidth = 0, gHeight = 0;
static int g_log_fd = -1;

static std::vector<std::string> gClassList;
static std::mutex gClassListMutex;
static char gFilter[128] = {0};

// ---------- Logging ----------
static void get_pkg_name(char* out, size_t outsz) {
    out[0] = 0;
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0 || buf[i] == ':') { buf[i] = 0; break; }
    }
    strncpy(out, buf, outsz - 1);
    out[outsz - 1] = 0;
}

static int try_open(const char* path) {
    return open(path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666);
}

static void init_log() {
    char pkg[256]; get_pkg_name(pkg, sizeof(pkg));
    LOGI("Package: %s", pkg);

    const char* candidates[] = {
        "/sdcard/Download/ImGuiMenu_crash.log",
        "/storage/emulated/0/Download/ImGuiMenu_crash.log",
    };
    for (auto p : candidates) {
        int fd = try_open(p);
        if (fd >= 0) {
            g_log_fd = fd;
            LOGI("Log file: %s", p);
            return;
        }
    }

    // Fallback: /sdcard/Android/data/<pkg>/crash.log
    if (pkg[0]) {
        char p2[512];
        snprintf(p2, sizeof(p2), "/sdcard/Android/data/%s", pkg);
        mkdir(p2, 0770);
        char p3[512];
        snprintf(p3, sizeof(p3), "/sdcard/Android/data/%s/crash.log", pkg);
        int fd = try_open(p3);
        if (fd >= 0) { g_log_fd = fd; LOGI("Log file: %s", p3); return; }

        // Last resort: /data/data/<pkg>/files/
        char p4[512];
        snprintf(p4, sizeof(p4), "/data/data/%s", pkg);
        mkdir(p4, 0770);
        char p5[512];
        snprintf(p5, sizeof(p5), "/data/data/%s/files", pkg);
        mkdir(p5, 0770);
        char p6[512];
        snprintf(p6, sizeof(p6), "/data/data/%s/files/crash.log", pkg);
        fd = try_open(p6);
        if (fd >= 0) { g_log_fd = fd; LOGI("Log file: %s", p6); return; }
    }
    LOGE("Could not open any log file");
}

static void log_step(const char* msg) {
    if (g_log_fd >= 0) {
        write(g_log_fd, "[N] ", 4);
        write(g_log_fd, msg, strlen(msg));
        write(g_log_fd, "\n", 1);
        fsync(g_log_fd);
    }
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, msg);
}

// ---------- Backtrace (for signal handler) ----------
struct BtState { void** cur; void** end; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg) {
    BtState* s = (BtState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (s->cur == s->end) return _URC_END_OF_STACK;
        *(s->cur++) = (void*)pc;
    }
    return _URC_NO_REASON;
}
static size_t capture_bt(void** buf, size_t max) {
    BtState s = { buf, buf + max };
    _Unwind_Backtrace(bt_cb, &s);
    return s.cur - buf;
}

// ---------- Crash handler ----------
static char g_alt_stack[16384];

static void crash_handler(int sig, siginfo_t* si, void*) {
    if (g_log_fd >= 0) {
        write(g_log_fd, "\n=== NATIVE CRASH ===\nSignal: ", 32);
        const char* name = "OTHER";
        switch (sig) {
            case SIGSEGV: name = "SIGSEGV"; break;
            case SIGABRT: name = "SIGABRT"; break;
            case SIGFPE:  name = "SIGFPE";  break;
            case SIGILL:  name = "SIGILL";  break;
            case SIGBUS:  name = "SIGBUS";  break;
            case SIGTRAP: name = "SIGTRAP"; break;
        }
        write(g_log_fd, name, strlen(name));
        write(g_log_fd, "\n", 1);
        void* bt[32];
        size_t n = capture_bt(bt, 32);
        write(g_log_fd, "Backtrace:\n", 11);
        for (size_t i = 0; i < n; i++) {
            uintptr_t addr = (uintptr_t)bt[i];
            char hx[20];
            hx[0] = '0'; hx[1] = 'x';
            const char* H = "0123456789abcdef";
            for (int j = 0; j < 16; j++) hx[2+j] = H[(addr >> ((15-j)*4)) & 0xF];
            hx[18] = '\n';
            write(g_log_fd, hx, 19);
        }
        write(g_log_fd, "=== END ===\n", 12);
        fsync(g_log_fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers() {
    stack_t ss;
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof(g_alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}

// ---------- DEX loading ----------
static bool loadDex(JNIEnv* env) {
    log_step("loadDex: start");
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls) { log_step("loadDex: ClassLoader not found"); return false; }
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (!getSys) { log_step("loadDex: getSystemClassLoader missing"); return false; }
    jobject parent = env->CallStaticObjectMethod(clCls, getSys);
    if (!parent) { log_step("loadDex: parent null"); return false; }

    jclass imCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!imCls) { log_step("loadDex: InMemoryDexClassLoader missing"); return false; }
    jmethodID ctor = env->GetMethodID(imCls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!ctor) { log_step("loadDex: ctor missing"); return false; }

    jobject buf = env->NewDirectByteBuffer((void*)imgui_dex, imgui_dex_len);
    if (!buf) { log_step("loadDex: NewDirectByteBuffer failed"); return false; }

    jobject loader = env->NewObject(imCls, ctor, buf, parent);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); env->ExceptionClear();
        log_step("loadDex: NewObject threw exception");
        return false;
    }
    if (!loader) { log_step("loadDex: loader null"); return false; }

    jclass lCls = env->GetObjectClass(loader);
    jmethodID loadM = env->GetMethodID(lCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadM) { log_step("loadDex: loadClass missing"); return false; }

    jstring name = env->NewStringUTF("com.menu.imgui.Helper");
    jclass hCls = (jclass)env->CallObjectMethod(loader, loadM, name);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); env->ExceptionClear();
        log_step("loadDex: Helper class load threw exception");
        return false;
    }
    if (!hCls) { log_step("loadDex: Helper class null"); return false; }

    gHelperClass = (jclass)env->NewGlobalRef(hCls);
    log_step("loadDex: success, Helper class acquired");
    return true;
}

// ---------- Activity discovery ----------
static jobject getCurrentActivity(JNIEnv* env) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) { env->ExceptionClear(); return nullptr; }
    jmethodID cur = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!cur) return nullptr;
    jobject at = env->CallStaticObjectMethod(atCls, cur);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!at) return nullptr;
    jfieldID f = env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!f) return nullptr;
    jobject acts = env->GetObjectField(at, f);
    if (!acts) return nullptr;
    jclass amCls = env->FindClass("android/util/ArrayMap");
    if (!amCls) { env->ExceptionClear(); return nullptr; }
    jmethodID sizeM = env->GetMethodID(amCls, "size", "()I");
    jmethodID valAt = env->GetMethodID(amCls, "valueAt", "(I)Ljava/lang/Object;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!sizeM || !valAt) return nullptr;
    jint n = env->CallIntMethod(acts, sizeM);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    for (jint i = 0; i < n; i++) {
        jobject rec = env->CallObjectMethod(acts, valAt, i);
        if (!rec) continue;
        jclass rCls = env->GetObjectClass(rec);
        jfieldID actF = env->GetFieldID(rCls, "activity", "Landroid/app/Activity;");
        if (!actF) { env->ExceptionClear(); continue; }
        jobject a = env->GetObjectField(rec, actF);
        if (a) return a;
    }
    return nullptr;
}

static void* pollThread(void*) {
    log_step("pollThread: started");
    JNIEnv* env = nullptr;
    jint r = gVM->AttachCurrentThread(&env, nullptr);
    if (r != JNI_OK || !env) {
        log_step("pollThread: AttachCurrentThread failed");
        return nullptr;
    }
    log_step("pollThread: attached");
    jobject activity = nullptr;
    for (int i = 0; i < 200; i++) {
        activity = getCurrentActivity(env);
        if (activity) {
            log_step("pollThread: activity found");
            break;
        }
        usleep(100000);
    }
    if (!activity) {
        log_step("pollThread: activity NOT found after 20s");
        gVM->DetachCurrentThread();
        return nullptr;
    }
    if (!gHelperClass) {
        log_step("pollThread: gHelperClass null");
        gVM->DetachCurrentThread();
        return nullptr;
    }
    jmethodID attach = env->GetStaticMethodID(gHelperClass, "attach", "(Landroid/content/Context;)V");
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (!attach) {
        log_step("pollThread: attach method missing");
        gVM->DetachCurrentThread();
        return nullptr;
    }
    log_step("pollThread: calling Helper.attach");
    env->CallStaticVoidMethod(gHelperClass, attach, activity);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); env->ExceptionClear();
        log_step("pollThread: Helper.attach threw");
    } else {
        log_step("pollThread: Helper.attach returned OK");
    }
    gVM->DetachCurrentThread();
    return nullptr;
}

// ---------- ImGui ----------
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

static bool g_firstFrame = false;

static void doInit(JNIEnv* env, jclass, jobject surface) {
    log_step("doInit: entered");
    if (gInitialized) { log_step("doInit: already initialized"); return; }
    if (!surface) { log_step("doInit: surface null"); return; }
    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow) { log_step("doInit: ANativeWindow_fromSurface failed"); return; }
    log_step("doInit: window acquired");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    setupStyle();
    log_step("doInit: context + style ready");
    if (!ImGui_ImplAndroid_Init(gWindow)) { log_step("doInit: ImGui_ImplAndroid_Init failed"); return; }
    log_step("doInit: Android backend OK");
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) { log_step("doInit: OpenGL3 backend failed"); return; }
    log_step("doInit: OpenGL3 backend OK");
    gInitialized = true;
    log_step("doInit: complete");
}

static void doResize(JNIEnv*, jclass, jint w, jint h) {
    gWidth=w; gHeight=h;
    glViewport(0,0,w,h);
}

static void doRender(JNIEnv*, jclass) {
    if (!gInitialized) return;
    if (!g_firstFrame) { log_step("doRender: first frame"); g_firstFrame = true; }
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
    init_log();
    install_crash_handlers();
    log_step("JNI_OnLoad: started");

    JNIEnv* env = nullptr;
    jint r = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (r != JNI_OK || !env) {
        log_step("JNI_OnLoad: GetEnv failed");
        return JNI_VERSION_1_6;
    }
    log_step("JNI_OnLoad: env acquired");
    gVM = vm;

    if (!loadDex(env)) {
        log_step("JNI_OnLoad: loadDex failed, exiting");
        return JNI_VERSION_1_6;
    }
    log_step("JNI_OnLoad: register natives");
    JNINativeMethod methods[] = {
        {(char*)"nativeInit",         (char*)"(Landroid/view/Surface;)V", (void*)doInit},
        {(char*)"nativeResize",       (char*)"(II)V",                    (void*)doResize},
        {(char*)"nativeRender",       (char*)"()V",                      (void*)doRender},
        {(char*)"nativeTouch",        (char*)"(IFF)Z",                    (void*)doTouch},
        {(char*)"nativeSetClassList", (char*)"([Ljava/lang/String;)V",    (void*)doSetClassList},
    };
    if (env->RegisterNatives(gHelperClass, methods, 5) != 0) {
        env->ExceptionDescribe(); env->ExceptionClear();
        log_step("JNI_OnLoad: RegisterNatives failed");
        return JNI_VERSION_1_6;
    }
    log_step("JNI_OnLoad: natives registered");

    pthread_t t;
    if (pthread_create(&t, nullptr, pollThread, nullptr) == 0) {
        pthread_detach(t);
        log_step("JNI_OnLoad: poll thread started");
    } else {
        log_step("JNI_OnLoad: pthread_create failed");
    }
    log_step("JNI_OnLoad: complete");
    return JNI_VERSION_1_6;
}
