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
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <unwind.h>

#ifndef MY_PI
#define MY_PI 3.14159265358979323846f
#endif

#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* gVM = nullptr;
static jclass gHelperClass = nullptr;
static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int gWidth = 0, gHeight = 0;
static int g_log_fd = -1;

static std::vector<std::string> gClassList;
static std::mutex gClassListMutex;
static char gFilter[128] = {0};

static float g_winX = 0, g_winY = 0, g_winW = 0, g_winH = 0, g_titleH = 30.0f;
static float g_virtX = 0, g_virtY = 0;
static float g_realPrevX = 0, g_realPrevY = 0;
static bool g_firstFrame = false;

static float g_settingFontScale   = 1.00f;
static float g_settingDragSpeed   = 0.55f;
static float g_settingScrollSpeed = 1.40f;

static bool  g_menuVisible = true;
static float g_fabSize = 58.0f;
static float g_fabX = -1.0f;
static float g_fabY = -1.0f;

// FAB drag state
static bool  g_fabDown = false;
static bool  g_fabMoved = false;
static float g_fabDownAtX = 0, g_fabDownAtY = 0;
static float g_fabDragStartX = 0, g_fabDragStartY = 0;

// View mode: 0 = class list, 1 = class details
static int g_viewMode = 0;
static std::string g_selectedClass;
static std::vector<std::string> g_selectedFields;
static std::vector<std::string> g_selectedMethods;
static bool g_classDetailsLoading = false;
static std::string g_pendingClassRequest;
static std::mutex gClassDetailsMutex;

// ---------------- Logging ----------------
static void get_pkg_name(char* out, size_t outsz) {
    out[0] = 0;
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) if (buf[i] == 0 || buf[i] == ':') { buf[i] = 0; break; }
    strncpy(out, buf, outsz - 1); out[outsz - 1] = 0;
}
static int try_open(const char* path) { return open(path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666); }
static void init_log() {
    int fd = try_open("/sdcard/Download/ImGuiMenu_crash.log");
    if (fd >= 0) { g_log_fd = fd; return; }
    char pkg[256]; get_pkg_name(pkg, sizeof(pkg));
    if (pkg[0]) {
        char p2[512], p3[512];
        snprintf(p2, sizeof(p2), "/sdcard/Android/data/%s", pkg); mkdir(p2, 0770);
        snprintf(p3, sizeof(p3), "/sdcard/Android/data/%s/crash.log", pkg);
        fd = try_open(p3); if (fd >= 0) { g_log_fd = fd; return; }
    }
}
static void log_step(const char* msg) {
    if (g_log_fd >= 0) { write(g_log_fd, "[N] ", 4); write(g_log_fd, msg, strlen(msg)); write(g_log_fd, "\n", 1); fsync(g_log_fd); }
}

// ---------------- Crash handlers ----------------
struct BtState { void** cur; void** end; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg) {
    BtState* s = (BtState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) { if (s->cur == s->end) return _URC_END_OF_STACK; *(s->cur++) = (void*)pc; }
    return _URC_NO_REASON;
}
static size_t capture_bt(void** b, size_t max) { BtState s = {b, b+max}; _Unwind_Backtrace(bt_cb, &s); return s.cur - b; }
static char g_alt[16384];
static void crash_handler(int sig, siginfo_t*, void*) {
    if (g_log_fd >= 0) {
        write(g_log_fd, "\n=== NATIVE CRASH ===\nSignal: ", 32);
        void* bt[32]; size_t n = capture_bt(bt, 32);
        for (size_t i = 0; i < n; i++) {
            uintptr_t a=(uintptr_t)bt[i]; char hx[20]; hx[0]='0'; hx[1]='x';
            const char* H="0123456789abcdef";
            for (int j=0;j<16;j++) hx[2+j]=H[(a>>((15-j)*4))&0xF];
            hx[18]='\n'; write(g_log_fd, hx, 19);
        }
        write(g_log_fd, "=== END ===\n", 12); fsync(g_log_fd);
    }
    signal(sig, SIG_DFL); raise(sig);
}
static void install_crash_handlers() {
    stack_t ss; ss.ss_sp=g_alt; ss.ss_size=sizeof(g_alt); ss.ss_flags=0;
    sigaltstack(&ss, nullptr);
    struct sigaction sa; memset(&sa,0,sizeof(sa));
    sa.sa_sigaction=crash_handler;
    sa.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,nullptr); sigaction(SIGABRT,&sa,nullptr);
    sigaction(SIGFPE,&sa,nullptr);  sigaction(SIGILL,&sa,nullptr);
    sigaction(SIGBUS,&sa,nullptr);  sigaction(SIGTRAP,&sa,nullptr);
}

// ---------------- Sigil ----------------
static void drawSigil(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 gold = IM_COL32(255, 200, 87, 255);
    const ImU32 cyan = IM_COL32(77, 208, 225, 220);
    const ImU32 dark = IM_COL32(10, 12, 16, 255);
    const ImU32 red  = IM_COL32(255, 80, 80, 210);
    for (int i = 5; i >= 1; i--) dl->AddCircle(c, r + i * 1.5f, IM_COL32(255, 200, 87, 10), 32, 1.0f);
    ImVec2 top(c.x, c.y - r), right(c.x + r, c.y), bot(c.x, c.y + r), left(c.x - r, c.y);
    dl->AddQuad(top, right, bot, left, gold, 2.0f);
    float r2 = r * 0.75f;
    ImVec2 hex[6];
    for (int i = 0; i < 6; i++) {
        float a = MY_PI * 2.0f * i / 6.0f + MY_PI * 0.5f;
        hex[i] = ImVec2(c.x + cosf(a) * r2, c.y + sinf(a) * r2);
    }
    for (int i = 0; i < 6; i++) dl->AddLine(hex[i], hex[(i + 1) % 6], cyan, 1.3f);
    float eyeW = r * 0.45f, eyeH = r * 0.22f;
    ImVec2 pts[13];
    for (int i = 0; i <= 12; i++) {
        float t = (float)i / 12.0f, a = MY_PI * t;
        pts[i] = ImVec2(c.x - eyeW * cosf(a), c.y - eyeH * sinf(a));
    }
    dl->AddPolyline(pts, 13, gold, 0, 1.8f);
    for (int i = 0; i <= 12; i++) {
        float t = (float)i / 12.0f, a = MY_PI * t;
        pts[i] = ImVec2(c.x - eyeW * cosf(a), c.y + eyeH * sinf(a));
    }
    dl->AddPolyline(pts, 13, gold, 0, 1.8f);
    dl->AddCircleFilled(c, eyeH * 0.85f, cyan, 24);
    dl->AddCircleFilled(c, eyeH * 0.42f, dark, 16);
    dl->AddCircleFilled(ImVec2(c.x - eyeH * 0.25f, c.y - eyeH * 0.25f), eyeH * 0.13f, IM_COL32(255, 255, 255, 210), 8);
    float rr = r * 0.085f;
    ImVec2 rune[4] = {
        ImVec2(c.x, c.y - r * 0.85f), ImVec2(c.x + r * 0.85f, c.y),
        ImVec2(c.x, c.y + r * 0.85f), ImVec2(c.x - r * 0.85f, c.y)
    };
    for (auto& p : rune) {
        ImVec2 a(p.x, p.y - rr), b(p.x + rr, p.y), d(p.x, p.y + rr), e(p.x - rr, p.y);
        dl->AddQuadFilled(a, b, d, e, red);
    }
}

// ---------------- DEX load ----------------
static bool loadDex(JNIEnv* env) {
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls) return false;
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(clCls, getSys);
    jclass imCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!imCls) return false;
    jmethodID ctor = env->GetMethodID(imCls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject buf = env->NewDirectByteBuffer((void*)imgui_dex, imgui_dex_len);
    jobject loader = env->NewObject(imCls, ctor, buf, parent);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!loader) return false;
    jclass lCls = env->GetObjectClass(loader);
    jmethodID loadM = env->GetMethodID(lCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.menu.imgui.Helper");
    jclass hCls = (jclass)env->CallObjectMethod(loader, loadM, name);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!hCls) return false;
    gHelperClass = (jclass)env->NewGlobalRef(hCls);
    return true;
}

static jobject getCurrentActivity(JNIEnv* env) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) { env->ExceptionClear(); return nullptr; }
    jmethodID cur = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (env->ExceptionCheck() || !cur) { env->ExceptionClear(); return nullptr; }
    jobject at = env->CallStaticObjectMethod(atCls, cur);
    if (env->ExceptionCheck() || !at) { env->ExceptionClear(); return nullptr; }
    jfieldID f = env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
    if (env->ExceptionCheck() || !f) { env->ExceptionClear(); return nullptr; }
    jobject acts = env->GetObjectField(at, f);
    jclass amCls = env->FindClass("android/util/ArrayMap");
    jmethodID sizeM = env->GetMethodID(amCls, "size", "()I");
    jmethodID valAt = env->GetMethodID(amCls, "valueAt", "(I)Ljava/lang/Object;");
    jint n = env->CallIntMethod(acts, sizeM);
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
    JNIEnv* env = nullptr;
    if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    jobject activity = nullptr;
    for (int i = 0; i < 200; i++) {
        activity = getCurrentActivity(env);
        if (activity) break;
        usleep(100000);
    }
    if (!activity || !gHelperClass) { gVM->DetachCurrentThread(); return nullptr; }
    jmethodID attach = env->GetStaticMethodID(gHelperClass, "attach", "(Landroid/content/Context;)V");
    if (attach) env->CallStaticVoidMethod(gHelperClass, attach, activity);
    if (env->ExceptionCheck()) env->ExceptionClear();
    gVM->DetachCurrentThread();
    return nullptr;
}

// ---------------- Style ----------------
static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 16; s.ChildRounding = 12; s.FrameRounding = 10;
    s.GrabRounding = 10; s.PopupRounding = 14; s.ScrollbarRounding = 10;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.WindowPadding = ImVec2(18, 18); s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10); s.ItemInnerSpacing = ImVec2(8, 8);
    s.ScrollbarSize = 16; s.GrabMinSize = 14;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.05f, 0.06f, 0.08f, 0.97f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.09f, 0.13f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(1.00f, 0.78f, 0.34f, 0.35f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.17f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.22f, 0.25f, 0.31f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.09f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.30f, 0.23f, 0.04f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.05f, 0.06f, 0.08f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1.00f, 0.78f, 0.34f, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 0.78f, 0.34f, 0.55f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.00f, 0.78f, 0.34f, 0.75f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.40f, 0.92f, 0.98f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(1.00f, 0.78f, 0.34f, 0.50f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(1.00f, 0.78f, 0.34f, 0.75f);
    c[ImGuiCol_ButtonActive]         = ImVec4(1.00f, 0.78f, 0.34f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(1.00f, 0.78f, 0.34f, 0.22f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(1.00f, 0.78f, 0.34f, 0.42f);
    c[ImGuiCol_HeaderActive]         = ImVec4(1.00f, 0.78f, 0.34f, 0.60f);
    c[ImGuiCol_Separator]            = ImVec4(1.00f, 0.78f, 0.34f, 0.25f);
}

// ---------------- Lifecycle ----------------
static void doInit(JNIEnv* env, jclass, jobject surface) {
    if (gInitialized || !surface) return;
    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow) return;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    setupStyle();
    if (!ImGui_ImplAndroid_Init(gWindow)) return;
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) return;
    gInitialized = true;
}
static void doResize(JNIEnv*, jclass, jint w, jint h) { gWidth = w; gHeight = h; glViewport(0, 0, w, h); }

// ---------------- FAB ----------------
static void renderFAB() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_fabX < 0) g_fabX = io.DisplaySize.x - g_fabSize - 18;
    if (g_fabY < 0) g_fabY = io.DisplaySize.y - g_fabSize - 36;

    ImGui::SetNextWindowPos(ImVec2(g_fabX, g_fabY));
    ImGui::SetNextWindowSize(ImVec2(g_fabSize, g_fabSize));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##fab", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 center(g_fabX + g_fabSize * 0.5f, g_fabY + g_fabSize * 0.5f);
    float r = g_fabSize * 0.5f;

    int alpha = g_fabMoved ? 30 : 15;
    for (int i = 4; i >= 1; i--) dl->AddCircle(center, r - 1 + i * 1.5f, IM_COL32(255, 200, 87, alpha), 40, 1.2f);
    dl->AddCircleFilled(center, r - 3, IM_COL32(10, 12, 16, 230), 40);
    dl->AddCircle(center, r - 3, IM_COL32(255, 200, 87, g_fabMoved ? 255 : 220), 40, 2.5f);
    drawSigil(dl, center, (r - 6) * 0.85f);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ---------------- Tab: Classes (list) ----------------
static void renderClassesTab() {
    size_t total;
    { std::lock_guard<std::mutex> lk(gClassListMutex); total = gClassList.size(); }

    float lineH = ImGui::GetTextLineHeight();
    float iconS = lineH * 1.3f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(iconS, iconS));
    drawSigil(ImGui::GetWindowDrawList(),
              ImVec2(cursor.x + iconS * 0.5f, cursor.y + iconS * 0.5f),
              iconS * 0.42f);
    ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.34f, 1.00f));
    ImGui::TextUnformatted("Classes");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.82f, 0.88f, 1.00f));
    ImGui::Text("%zu", total);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("tap any to view members");
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search classes...", gFilter, sizeof(gFilter));
    ImGui::Spacing();

    int shown = 0;
    std::vector<int> visible;
    {
        std::lock_guard<std::mutex> lk(gClassListMutex);
        visible.reserve(gClassList.size());
        if (gFilter[0]) {
            for (int i = 0; i < (int)gClassList.size(); i++)
                if (gClassList[i].find(gFilter) != std::string::npos) visible.push_back(i);
        } else {
            for (int i = 0; i < (int)gClassList.size(); i++) visible.push_back(i);
        }
        shown = (int)visible.size();

        ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        if (shown == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("  No matching classes");
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(shown);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const std::string& n = gClassList[visible[i]];
                    ImGui::PushID(i);
                    if (ImGui::Selectable(n.c_str(), false)) {
                        {
                            std::lock_guard<std::mutex> lk2(gClassDetailsMutex);
                            g_selectedClass = n;
                            g_selectedFields.clear();
                            g_selectedMethods.clear();
                            g_classDetailsLoading = true;
                        }
                        g_pendingClassRequest = n;
                        g_viewMode = 1;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::TextDisabled("Showing %d of %zu", shown, total);
}

// ---------------- Tab: Settings ----------------
static void renderSettingsTab() {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.34f, 1.00f));
    ImGui::TextUnformatted("Appearance");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("Font scale", &g_settingFontScale, 0.60f, 2.50f, "%.2fx");
    ImGui::TextDisabled("  0.6x - 2.5x (default 1.0x)");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.34f, 1.00f));
    ImGui::TextUnformatted("Touch");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("Drag speed", &g_settingDragSpeed, 0.20f, 1.50f, "%.2fx");
    ImGui::TextDisabled("  Lower = slower window drag");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("Scroll speed", &g_settingScrollSpeed, 0.50f, 3.00f, "%.2fx");
    ImGui::TextDisabled("  Higher = faster list scroll");
    ImGui::Spacing(); ImGui::Spacing();
    if (ImGui::Button("Reset to defaults", ImVec2(-1, 0))) {
        g_settingFontScale = 1.00f;
        g_settingDragSpeed = 0.55f;
        g_settingScrollSpeed = 1.40f;
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextDisabled("Made with love in Iran");
    ImGui::TextDisabled("github.com/Mrakbrlalhzar");
}

// ---------------- Class details view ----------------
static void renderClassDetails() {
    std::string clsName;
    std::vector<std::string> fields, methods;
    bool loading;
    {
        std::lock_guard<std::mutex> lk(gClassDetailsMutex);
        clsName = g_selectedClass;
        fields = g_selectedFields;
        methods = g_selectedMethods;
        loading = g_classDetailsLoading;
    }

    if (ImGui::Button("< Back", ImVec2(90, 0))) {
        g_viewMode = 0;
        return;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.34f, 1.00f));
    ImGui::TextUnformatted(clsName.empty() ? "(no class)" : clsName.c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (loading) {
        ImGui::TextDisabled("Loading members...");
        return;
    }

    ImGui::BeginChild("##details", ImVec2(0, 0), true);

    if (!fields.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.82f, 0.88f, 1.00f));
        ImGui::Text("Fields (%zu)", fields.size());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (const auto& f : fields)
            ImGui::BulletText("%s", f.c_str());
        ImGui::Spacing();
    }

    if (!methods.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.82f, 0.88f, 1.00f));
        ImGui::Text("Methods (%zu)", methods.size());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (const auto& m : methods)
            ImGui::BulletText("%s", m.c_str());
    }

    if (fields.empty() && methods.empty())
        ImGui::TextDisabled("No declared members");

    ImGui::EndChild();
}

// ---------------- Main window ----------------
static void renderMainWindow() {
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x * 0.92f;
    float h = io.DisplaySize.y * 0.85f;
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, io.DisplaySize.y * 0.075f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);

    ImGui::Begin("DexTool", nullptr, ImGuiWindowFlags_None);

    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    g_winX = wp.x; g_winY = wp.y;
    g_winW = ws.x; g_winH = ws.y;
    g_titleH = ImGui::GetFrameHeight();

    if (g_viewMode == 1) {
        renderClassDetails();
    } else {
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Classes")) {
                renderClassesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                renderSettingsTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    ImGui::End();
}

// ---------------- Pending request processor ----------------
static void processPendingClassRequest() {
    if (g_pendingClassRequest.empty()) return;
    std::string className = g_pendingClassRequest;
    g_pendingClassRequest.clear();

    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jmethodID reqM = env->GetStaticMethodID(gHelperClass, "requestClassDetails", "(Ljava/lang/String;)V");
    if (reqM) {
        jstring js = env->NewStringUTF(className.c_str());
        env->CallStaticVoidMethod(gHelperClass, reqM, js);
        env->DeleteLocalRef(js);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) gVM->DetachCurrentThread();
}

static void doRender(JNIEnv*, jclass) {
    if (!gInitialized) return;
    if (!g_firstFrame) { log_step("doRender: first frame"); g_firstFrame = true; }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = g_settingFontScale;
    ImGui::NewFrame();

    if (g_menuVisible) renderMainWindow();
    renderFAB();
    processPendingClassRequest();

    ImGui::Render();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------- Touch ----------------
static jboolean doTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (!gInitialized) return JNI_FALSE;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);

    // ---- FAB drag/tap always handled first ----
    if (action == 0) {
        bool inFAB = (g_fabX >= 0 && x >= g_fabX && x <= g_fabX + g_fabSize &&
                      g_fabY >= 0 && y >= g_fabY && y <= g_fabY + g_fabSize);
        if (inFAB) {
            g_fabDown = true;
            g_fabMoved = false;
            g_fabDownAtX = x; g_fabDownAtY = y;
            g_fabDragStartX = g_fabX; g_fabDragStartY = g_fabY;
            return JNI_TRUE;
        }
    } else if (g_fabDown) {
        if (action == 2) {
            float dx = x - g_fabDownAtX;
            float dy = y - g_fabDownAtY;
            if (fabsf(dx) > 20.0f || fabsf(dy) > 20.0f) g_fabMoved = true;
            if (g_fabMoved) {
                g_fabX = g_fabDragStartX + dx;
                g_fabY = g_fabDragStartY + dy;
                if (g_fabX < 0) g_fabX = 0;
                if (g_fabY < 0) g_fabY = 0;
                if (g_fabX > io.DisplaySize.x - g_fabSize) g_fabX = io.DisplaySize.x - g_fabSize;
                if (g_fabY > io.DisplaySize.y - g_fabSize) g_fabY = io.DisplaySize.y - g_fabSize;
            }
            return JNI_TRUE;
        }
        if (action == 1 || action == 3) {
            g_fabDown = false;
            if (!g_fabMoved) {
                g_menuVisible = !g_menuVisible;
                if (g_menuVisible) g_viewMode = 0;
            }
            return JNI_TRUE;
        }
    }

    // ---- Menu hidden: everything else passes through ----
    if (!g_menuVisible) return JNI_FALSE;

    // ---- Menu visible: damped virtual cursor ----
    if (action == 0) {
        g_virtX = x; g_virtY = y;
        g_realPrevX = x; g_realPrevY = y;
        io.AddMousePosEvent(g_virtX, g_virtY);
        io.AddMouseButtonEvent(0, true);
    } else if (action == 1 || action == 3) {
        io.AddMousePosEvent(g_virtX, g_virtY);
        io.AddMouseButtonEvent(0, false);
    } else if (action == 2) {
        float dx = x - g_realPrevX;
        float dy = y - g_realPrevY;
        g_realPrevX = x; g_realPrevY = y;

        bool inWindow = (g_virtX >= g_winX && g_virtX <= g_winX + g_winW &&
                         g_virtY >= g_winY && g_virtY <= g_winY + g_winH);
        bool inTitleBar = inWindow && (g_virtY <= g_winY + g_titleH);

        float scale = 1.0f;
        if (inTitleBar) scale = g_settingDragSpeed;
        else if (inWindow) scale = g_settingScrollSpeed;

        g_virtX += dx * scale;
        g_virtY += dy * scale;
        if (g_virtX < 0) g_virtX = 0;
        if (g_virtY < 0) g_virtY = 0;
        if (g_virtX > io.DisplaySize.x) g_virtX = io.DisplaySize.x;
        if (g_virtY > io.DisplaySize.y) g_virtY = io.DisplaySize.y;

        io.AddMousePosEvent(g_virtX, g_virtY);
    }

    return io.WantCaptureMouse ? JNI_TRUE : JNI_FALSE;
}

// ---------------- Native callbacks ----------------
static jboolean doWantKeyboard(JNIEnv*, jclass) {
    if (!gInitialized) return JNI_FALSE;
    return ImGui::GetIO().WantTextInput ? JNI_TRUE : JNI_FALSE;
}
static void doAddInputChar(JNIEnv*, jclass, jint cp) {
    if (!gInitialized || cp <= 0) return;
    ImGui::GetIO().AddInputCharacter((unsigned int)cp);
}
static void doAddKeyEvent(JNIEnv*, jclass, jint keycode, jboolean down) {
    if (!gInitialized) return;
    ImGuiKey k = ImGuiKey_None;
    switch (keycode) {
        case 67: k = ImGuiKey_Backspace; break;
        case 66: k = ImGuiKey_Enter; break;
        case 65: k = ImGuiKey_Tab; break;
        default: return;
    }
    ImGui::GetIO().AddKeyEvent(k, down ? true : false);
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
}

static std::string jstrToStd(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
static std::vector<std::string> jarrToStrs(JNIEnv* env, jobjectArray arr) {
    std::vector<std::string> out;
    if (!arr) return out;
    jsize n = env->GetArrayLength(arr);
    out.reserve(n);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(arr, i);
        if (s) {
            const char* c = env->GetStringUTFChars(s, nullptr);
            if (c) out.emplace_back(c);
            if (c) env->ReleaseStringUTFChars(s, c);
            env->DeleteLocalRef(s);
        }
    }
    return out;
}

static void doSetClassDetails(JNIEnv* env, jclass, jstring className, jobjectArray fields, jobjectArray methods) {
    std::string name = jstrToStd(env, className);
    std::vector<std::string> fs = jarrToStrs(env, fields);
    std::vector<std::string> ms = jarrToStrs(env, methods);
    std::lock_guard<std::mutex> lk(gClassDetailsMutex);
    if (name != g_selectedClass) return;
    g_selectedFields = std::move(fs);
    g_selectedMethods = std::move(ms);
    g_classDetailsLoading = false;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    init_log();
    install_crash_handlers();
    log_step("JNI_OnLoad: started");
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    gVM = vm;
    if (!loadDex(env)) { log_step("loadDex failed"); return JNI_VERSION_1_6; }
    JNINativeMethod methods[] = {
        {(char*)"nativeInit",            (char*)"(Landroid/view/Surface;)V",   (void*)doInit},
        {(char*)"nativeResize",          (char*)"(II)V",                        (void*)doResize},
        {(char*)"nativeRender",          (char*)"()V",                          (void*)doRender},
        {(char*)"nativeTouch",           (char*)"(IFF)Z",                       (void*)doTouch},
        {(char*)"nativeSetClassList",    (char*)"([Ljava/lang/String;)V",       (void*)doSetClassList},
        {(char*)"nativeWantKeyboard",    (char*)"()Z",                          (void*)doWantKeyboard},
        {(char*)"nativeAddInputChar",    (char*)"(I)V",                         (void*)doAddInputChar},
        {(char*)"nativeAddKeyEvent",     (char*)"(IZ)V",                        (void*)doAddKeyEvent},
        {(char*)"nativeSetClassDetails", (char*)"(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V", (void*)doSetClassDetails},
    };
    if (env->RegisterNatives(gHelperClass, methods, 9) != 0) {
        env->ExceptionClear();
        return JNI_VERSION_1_6;
    }
    log_step("JNI_OnLoad: natives registered");
    pthread_t t;
    if (pthread_create(&t, nullptr, pollThread, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
