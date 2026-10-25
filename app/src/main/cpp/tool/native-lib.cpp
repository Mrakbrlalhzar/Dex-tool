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
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>

#define MY_PI 3.14159265358979323846f
#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* gVM = nullptr;
static jclass gHelperClass = nullptr;
static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int gWidth = 0, gHeight = 0;

static std::vector<std::string> gClassList;
static std::mutex gClassListMutex;

struct MethodInfo {
    std::string raw, name, returnType, display;
    bool isStatic, isConstructor;
    std::vector<std::string> paramTypes;
};
struct MethodCallState {
    char argBufs[8][256] = {};
    int numParams = 0;
    std::string result;
    bool hasResult = false;
    bool calling = false;
    bool runInMain = false;
    int selectedInstance[8] = {0};
    std::vector<std::string> availableInstances[8];
};
struct ClassDetail {
    bool loading = false;
    std::vector<std::string> fields;
    std::vector<MethodInfo> methods;
    std::map<std::string, MethodCallState> methodStates;
    std::set<std::string> expandedMethods;
};
struct Section {
    int id = 0;
    char filter[128] = {0};
    bool detached = false;
    float detX = 100, detY = 100, detW = 550, detH = 550;
    double detachTime = 0;
    std::map<std::string, ClassDetail> details;
    std::set<std::string> expandedClasses;
};
struct InspectorWin {
    std::string className;
    bool isOpen = true;
    float x = 100, y = 100, w = 400, h = 300;
    std::vector<std::string> instances;
    bool searching = false;
};

static std::vector<Section> gSections;
static std::vector<InspectorWin> gInspectors;
static int gNextSectionId = 1;
static int gActiveSection = 0;
static bool gRequestAddSection = false;
static std::vector<int> gPendingCloseSections;

static int gPressedTabId = -1;
static double gPressedTabStart = 0;
static std::string gPressedClassName;
static int gPressedClassSectionId = -1;
static double gPressedClassStart = 0;
static bool gClassLongPressed = false;

static float gTabBarX = 0, gTabBarY = 0;
static float gWinX = 0, gWinY = 0, gWinW = 0, gWinH = 0;

struct PendingDetailReq { int sectionId; std::string className; };
struct PendingCallReq { int sectionId; std::string className; std::string methodSig; std::string methodName; std::vector<std::string> paramTypes; std::vector<std::string> args; bool isStatic; bool isConstructor; bool runInMain; };
struct PendingInspectorReq { std::string className; };
struct DetailResult { int sectionId; std::string className; std::vector<std::string> fields; std::vector<std::string> methodRaws; };
struct MethodResult { int sectionId; std::string className; std::string methodSig; std::string result; };
struct InspectorResult { std::string className; std::vector<std::string> instances; };
struct AvailableInstancesResult { std::string className; std::vector<std::string> instances; };

static std::vector<PendingDetailReq> gPendingDetailReqs;
static std::vector<PendingCallReq> gPendingCallReqs;
static std::vector<PendingInspectorReq> gPendingInspectorReqs;
static std::vector<DetailResult> gDetailResults;
static std::vector<MethodResult> gMethodResults;
static std::vector<InspectorResult> gInspectorResults;
static std::vector<AvailableInstancesResult> gAvailableInstancesResults;
static std::mutex gQueuesMutex;

static float g_settingFontScale = 1.00f;
static float g_settingDragSpeed = 0.55f;
static float g_settingScrollSpeed = 1.40f;

static bool g_menuVisible = true;
static float g_fabSize = 58.0f;
static float g_fabX = -1.0f, g_fabY = -1.0f;
static bool g_fabDown = false, g_fabMoved = false;
static float g_fabDownAtX = 0, g_fabDownAtY = 0;
static float g_fabDragStartX = 0, g_fabDragStartY = 0;
static float g_virtX = 0, g_virtY = 0;
static float g_realPrevX = 0, g_realPrevY = 0;
static float g_titleH = 30.0f;

static void splitStr(const std::string& s, char d, std::vector<std::string>& out) {
    out.clear(); size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) { if (i == s.size() || s[i] == d) { out.push_back(s.substr(start, i - start)); start = i + 1; } }
}
static std::string joinStrs(const std::vector<std::string>& v, const char* sep) {
    std::string out; for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; } return out;
}
static MethodInfo parseMethodRaw(const std::string& raw) {
    MethodInfo m; m.raw = raw; std::vector<std::string> p; splitStr(raw, '|', p);
    if (p.size() >= 5) {
        m.name = p[0]; m.returnType = p[1]; m.isStatic = (p[2] == "1"); m.isConstructor = (p[3] == "1");
        if (!p[4].empty()) splitStr(p[4], ';', m.paramTypes);
    }
    m.display = m.isConstructor ? "<init>(" + joinStrs(m.paramTypes, ", ") + ")" : m.returnType + " " + m.name + "(" + joinStrs(m.paramTypes, ", ") + ")";
    return m;
}
static Section* findSection(int id) { for (auto& s : gSections) if (s.id == id) return &s; return nullptr; }
static void addSectionImpl() { Section s; s.id = gNextSectionId++; gSections.push_back(std::move(s)); gActiveSection = gSections.back().id; }
static void closeSectionImpl(int id) {
    gSections.erase(std::remove_if(gSections.begin(), gSections.end(), [id](const Section& s) { return s.id == id; }), gSections.end());
    if (gActiveSection == id) { gActiveSection = 0; for (auto& s : gSections) if (!s.detached) { gActiveSection = s.id; break; } }
}
static void requestClassDetails(int sectionId, const std::string& className) {
    std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingDetailReqs.push_back({sectionId, className});
}

static void drawSigil(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 gold = IM_COL32(255, 200, 87, 255); const ImU32 cyan = IM_COL32(77, 208, 225, 220);
    const ImU32 dark = IM_COL32(10, 12, 16, 255); const ImU32 red  = IM_COL32(255, 80, 80, 210);
    for (int i = 5; i >= 1; i--) dl->AddCircle(c, r + i * 1.5f, IM_COL32(255, 200, 87, 10), 32, 1.0f);
    ImVec2 top(c.x, c.y - r), right(c.x + r, c.y), bot(c.x, c.y + r), left(c.x - r, c.y);
    dl->AddQuad(top, right, bot, left, gold, 2.0f);
    float r2 = r * 0.75f; ImVec2 hex[6];
    for (int i = 0; i < 6; i++) { float a = MY_PI * 2.0f * i / 6.0f + MY_PI * 0.5f; hex[i] = ImVec2(c.x + cosf(a) * r2, c.y + sinf(a) * r2); }
    for (int i = 0; i < 6; i++) dl->AddLine(hex[i], hex[(i+1)%6], cyan, 1.3f);
    float eyeW = r * 0.45f, eyeH = r * 0.22f; ImVec2 pts[13];
    for (int i = 0; i <= 12; i++) { float t = i/12.0f, a = MY_PI * t; pts[i] = ImVec2(c.x - eyeW * cosf(a), c.y - eyeH * sinf(a)); }
    dl->AddPolyline(pts, 13, gold, 0, 1.8f);
    for (int i = 0; i <= 12; i++) { float t = i/12.0f, a = MY_PI * t; pts[i] = ImVec2(c.x - eyeW * cosf(a), c.y + eyeH * sinf(a)); }
    dl->AddPolyline(pts, 13, gold, 0, 1.8f);
    dl->AddCircleFilled(c, eyeH * 0.85f, cyan, 24); dl->AddCircleFilled(c, eyeH * 0.42f, dark, 16);
    float rr = r * 0.085f; ImVec2 rune[4] = { ImVec2(c.x, c.y - r*0.85f), ImVec2(c.x + r*0.85f, c.y), ImVec2(c.x, c.y + r*0.85f), ImVec2(c.x - r*0.85f, c.y) };
    for (auto& p : rune) { ImVec2 a(p.x, p.y-rr), b(p.x+rr, p.y), d(p.x, p.y+rr), e(p.x-rr, p.y); dl->AddQuadFilled(a, b, d, e, red); }
}

static bool loadDex(JNIEnv* env) {
    jclass clCls = env->FindClass("java/lang/ClassLoader"); if (!clCls) return false;
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(clCls, getSys);
    jclass imCls = env->FindClass("dalvik/system/InMemoryDexClassLoader"); if (!imCls) return false;
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
    if (!hCls) return false; gHelperClass = (jclass)env->NewGlobalRef(hCls); return true;
}

static jobject getCurrentActivity(JNIEnv* env) {
    jclass atCls = env->FindClass("android/app/ActivityThread"); if (!atCls) { env->ExceptionClear(); return nullptr; }
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
        jobject rec = env->CallObjectMethod(acts, valAt, i); if (!rec) continue;
        jclass rCls = env->GetObjectClass(rec);
        jfieldID actF = env->GetFieldID(rCls, "activity", "Landroid/app/Activity;");
        if (!actF) { env->ExceptionClear(); continue; }
        jobject a = env->GetObjectField(rec, actF); if (a) return a;
    }
    return nullptr;
}

static void* pollThread(void*) {
    JNIEnv* env = nullptr;
    if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    jobject activity = nullptr;
    for (int i = 0; i < 200; i++) { activity = getCurrentActivity(env); if (activity) break; usleep(100000); }
    if (!activity || !gHelperClass) { gVM->DetachCurrentThread(); return nullptr; }
    jmethodID attach = env->GetStaticMethodID(gHelperClass, "attach", "(Landroid/content/Context;)V");
    if (attach) env->CallStaticVoidMethod(gHelperClass, attach, activity);
    if (env->ExceptionCheck()) env->ExceptionClear();
    gVM->DetachCurrentThread(); return nullptr;
}

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

static void doInit(JNIEnv* env, jclass, jobject surface) {
    if (gInitialized || !surface) return;
    gWindow = ANativeWindow_fromSurface(env, surface); if (!gWindow) return;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    setupStyle();
    if (!ImGui_ImplAndroid_Init(gWindow)) return;
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) return;
    gInitialized = true;
    if (gSections.empty()) addSectionImpl();
}
static void doResize(JNIEnv*, jclass, jint w, jint h) { gWidth = w; gHeight = h; glViewport(0, 0, w, h); }

static bool isComplexType(const std::string& t) {
    if (t == "int" || t == "long" || t == "short" || t == "byte" || t == "char" || 
        t == "boolean" || t == "float" || t == "double" || t == "java.lang.String") return false;
    return true;
}

static void renderMethodPanel(Section& sec, const std::string& className, MethodInfo& m, MethodCallState& st) {
    ImGui::Indent(20);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Return: %s", m.returnType.c_str());
    ImGui::PopTextWrapPos();

    for (int i = 0; i < st.numParams; i++) {
        ImGui::PushID(i);
        ImGui::TextDisabled("arg%d (%s)", i, m.paramTypes[i].c_str());
        
        if (isComplexType(m.paramTypes[i])) {
            if (st.availableInstances[i].empty()) {
                st.availableInstances[i].push_back("null");
                st.availableInstances[i].push_back("[Current Activity]");
                st.availableInstances[i].push_back("[Application Context]");
            }
            if (st.selectedInstance[i] >= (int)st.availableInstances[i].size()) st.selectedInstance[i] = 0;
            
            std::string preview = st.availableInstances[i][st.selectedInstance[i]];
            if (ImGui::BeginCombo("##arg_combo", preview.c_str())) {
                for (int n = 0; n < st.availableInstances[i].size(); n++) {
                    bool is_selected = (st.selectedInstance[i] == n);
                    if (ImGui::Selectable(st.availableInstances[i][n].c_str(), is_selected)) {
                        st.selectedInstance[i] = n;
                        snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%s", st.availableInstances[i][n].c_str());
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else if (m.paramTypes[i] == "boolean") {
            bool val = (st.argBufs[i][0] == '1');
            if (ImGui::Checkbox("##bool", &val)) {
                snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%d", val ? 1 : 0);
            }
        } else {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##arg", st.argBufs[i], sizeof(st.argBufs[i]));
        }
        ImGui::PopID();
    }
    ImGui::Checkbox("Call in Main Thread", &st.runInMain);
    ImGui::TextDisabled("Note: For objects, choose an instance or [Current Activity].");
    
    if (ImGui::Button("Call", ImVec2(100, 0))) {
        PendingCallReq c;
        c.sectionId = sec.id; c.className = className;
        c.methodSig = m.raw; c.methodName = m.name;
        c.paramTypes = m.paramTypes; c.args.resize(st.numParams);
        for (int i = 0; i < st.numParams; i++) c.args[i] = std::string(st.argBufs[i]);
        c.isStatic = m.isStatic; c.isConstructor = m.isConstructor; c.runInMain = st.runInMain;
        { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingCallReqs.push_back(c); }
        st.calling = true; st.hasResult = false;
    }
    if (st.calling) { ImGui::SameLine(); ImGui::TextDisabled("Executing..."); }
    else if (st.hasResult) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", st.result.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Unindent(20);
}

static void renderClassDetail(Section& sec, const std::string& className) {
    auto it = sec.details.find(className); if (it == sec.details.end()) return;
    ClassDetail& det = it->second;
    ImGui::Indent(20);
    
    if (ImGui::Button("Inspector")) {
        gInspectors.push_back({className, true, 100, 100, 400, 300, {}, true});
        { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingInspectorReqs.push_back({className}); }
    }
    ImGui::Separator();

    if (det.loading) { ImGui::TextDisabled("Loading..."); ImGui::Unindent(20); return; }

    if (!det.fields.empty()) {
        ImGui::TextColored(ImVec4(0.30f,0.82f,0.88f,1), "Fields");
        ImGui::PushTextWrapPos(0.0f);
        for (auto& f : det.fields) ImGui::BulletText("%s", f.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    if (!det.methods.empty()) {
        ImGui::TextColored(ImVec4(0.30f,0.82f,0.88f,1), "Methods");
        for (auto& m : det.methods) {
            if (m.name.find("lambda$") == 0) continue; // Hide compiler generated
            bool exp = det.expandedMethods.count(m.raw) > 0;
            ImGui::PushID(m.raw.c_str());
            ImGui::PushTextWrapPos(0.0f);
            if (ImGui::TreeNodeEx(m.display.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                if (!exp) {
                    det.expandedMethods.insert(m.raw);
                    auto& st = det.methodStates[m.raw];
                    st.numParams = (int)m.paramTypes.size(); if (st.numParams > 8) st.numParams = 8;
                    for (int i = 0; i < st.numParams; i++) st.argBufs[i][0] = 0;
                }
                renderMethodPanel(sec, className, m, det.methodStates[m.raw]);
                ImGui::TreePop();
            } else if (exp) {
                det.expandedMethods.erase(m.raw);
            }
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
    }
    ImGui::Unindent(20);
    ImGui::Separator();
}

static void renderSectionContent(Section& sec) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search classes...", sec.filter, sizeof(sec.filter));
    ImGui::Spacing();
    std::vector<std::string> visible;
    {
        std::lock_guard<std::mutex> lk(gClassListMutex);
        if (sec.filter[0]) { for (auto& c : gClassList) if (c.find(sec.filter) != std::string::npos) visible.push_back(c); }
        else visible = gClassList;
    }
    ImGui::TextDisabled("%zu classes", visible.size());
    ImGui::BeginChild("##list", ImVec2(0, 0), true);
    for (auto& name : visible) {
        ImGui::PushID(name.c_str());
        bool node_open = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        if (ImGui::IsItemClicked()) {
            gPressedClassName = name; gPressedClassSectionId = sec.id; gPressedClassStart = ImGui::GetTime(); gClassLongPressed = false;
        }
        if (node_open) {
            if (sec.details.count(name) == 0) { sec.details[name].loading = true; requestClassDetails(sec.id, name); }
            renderClassDetail(sec, name);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void renderFAB() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_fabX < 0) g_fabX = io.DisplaySize.x - g_fabSize - 18;
    if (g_fabY < 0) g_fabY = io.DisplaySize.y - g_fabSize - 36;
    ImGui::SetNextWindowPos(ImVec2(g_fabX, g_fabY)); ImGui::SetNextWindowSize(ImVec2(g_fabSize, g_fabSize));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,0));
    ImGui::Begin("##fab", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 center(g_fabX + g_fabSize * 0.5f, g_fabY + g_fabSize * 0.5f); float r = g_fabSize * 0.5f;
    int alpha = g_fabMoved ? 30 : 15;
    for (int i = 4; i >= 1; i--) dl->AddCircle(center, r - 1 + i * 1.5f, IM_COL32(255,200,87,alpha), 40, 1.2f);
    dl->AddCircleFilled(center, r - 3, IM_COL32(10,12,16,230), 40);
    dl->AddCircle(center, r - 3, IM_COL32(255,200,87,g_fabMoved ? 255 : 220), 40, 2.5f);
    drawSigil(dl, center, (r - 6) * 0.85f);
    ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}

static void renderSettingsTab() {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.78f,0.34f,1)); ImGui::TextUnformatted("Appearance"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("Font scale", &g_settingFontScale, 0.60f, 2.50f, "%.2fx");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.78f,0.34f,1)); ImGui::TextUnformatted("Touch"); ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("Drag speed", &g_settingDragSpeed, 0.20f, 1.50f, "%.2fx");
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("Scroll speed", &g_settingScrollSpeed, 0.50f, 3.00f, "%.2fx");
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults", ImVec2(-1, 0))) {
        g_settingFontScale = 1.00f; g_settingDragSpeed = 0.55f; g_settingScrollSpeed = 1.40f;
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextDisabled("Made with love in Iran");
    ImGui::TextDisabled("github.com/Mrakbrlalhzar");
}

static void renderMainWindow() {
    ImGuiIO& io = ImGui::GetIO(); float w = io.DisplaySize.x * 0.92f; float h = io.DisplaySize.y * 0.85f;
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, io.DisplaySize.y * 0.075f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    ImGui::Begin("DexTool", nullptr, ImGuiWindowFlags_None);
    ImVec2 wp = ImGui::GetWindowPos(); ImVec2 ws = ImGui::GetWindowSize();
    gWinX = wp.x; gWinY = wp.y; gWinW = ws.x; gWinH = ws.y; g_titleH = ImGui::GetFrameHeight();

    if (ImGui::BeginTabBar("##main_tabs")) {
        if (ImGui::BeginTabItem("Classes")) {
            float startX = ImGui::GetCursorScreenPos().x; float startY = ImGui::GetCursorScreenPos().y;
            for (auto& sec : gSections) {
                if (sec.detached) continue;
                ImGui::PushID(sec.id); bool isActive = (sec.id == gActiveSection);
                if (isActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f,0.27f,0.05f,1.0f));
                char label[256]; if (sec.filter[0]) snprintf(label, sizeof(label), "S%d: %s", sec.id, sec.filter); else snprintf(label, sizeof(label), "Section %d", sec.id);
                ImGui::Button(label, ImVec2(0, 32));
                if (ImGui::IsItemClicked()) { gPressedTabId = sec.id; gPressedTabStart = ImGui::GetTime(); }
                ImGui::SameLine();
                if (isActive) ImGui::PopStyleColor(); ImGui::PopID();
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f,0.82f,0.88f,0.6f));
            if (ImGui::Button("+", ImVec2(32, 32))) gRequestAddSection = true;
            ImGui::PopStyleColor();
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            gTabBarX = startX; gTabBarY = startY;

            if (gPressedTabId != -1) {
                if (!ImGui::IsMouseDown(0)) { if (ImGui::GetTime() - gPressedTabStart < 0.6) gActiveSection = gPressedTabId; gPressedTabId = -1; }
                else if (ImGui::GetTime() - gPressedTabStart > 0.6) {
                    auto* s = findSection(gPressedTabId);
                    if (s && !s->detached) { s->detached = true; s->detX = 100; s->detY = startY + 90; s->detachTime = ImGui::GetTime(); }
                    gPressedTabId = -1;
                }
            }

            if (!gPressedClassName.empty()) {
                if (!ImGui::IsMouseDown(0)) {
                    if (!gClassLongPressed) { /* Tap */ }
                    gPressedClassName.clear(); gClassLongPressed = false;
                } else if (!gClassLongPressed && ImGui::GetTime() - gPressedClassStart > 0.5) {
                    gClassLongPressed = true;
                    Section s; s.id = gNextSectionId++; s.detached = true; s.expandedClasses.insert(gPressedClassName);
                    s.detX = io.DisplaySize.x / 2 - 250; s.detY = io.DisplaySize.y / 2 - 250; s.detW = 500; s.detH = 500;
                    s.filter[0] = 0; snprintf(s.filter, sizeof(s.filter), "%s", gPressedClassName.c_str());
                    gSections.push_back(s); requestClassDetails(s.id, gPressedClassName); gPressedClassName.clear();
                }
            }

            Section* active = findSection(gActiveSection);
            if (!active || active->detached) { active = nullptr; for (auto& s : gSections) if (!s.detached) { active = &s; gActiveSection = s.id; break; } }
            if (active) renderSectionContent(*active); else ImGui::TextDisabled("No sections. Press + to add one.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) { renderSettingsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void renderDetachedWindows() {
    for (auto& sec : gSections) {
        if (!sec.detached) continue;
        ImGui::SetNextWindowPos(ImVec2(sec.detX, sec.detY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(sec.detW, sec.detH), ImGuiCond_FirstUseEver);
        char title[256]; if (sec.filter[0]) snprintf(title, sizeof(title), "S%d: %s##det%d", sec.id, sec.filter, sec.id); else snprintf(title, sizeof(title), "Section %d##det%d", sec.id, sec.id);
        bool open = true; ImGui::Begin(title, &open, ImGuiWindowFlags_None);
        ImVec2 p = ImGui::GetWindowPos(); ImVec2 s = ImGui::GetWindowSize();
        sec.detX = p.x; sec.detY = p.y; sec.detW = s.x; sec.detH = s.y;
        renderSectionContent(sec); ImGui::End();
        if (!open) gPendingCloseSections.push_back(sec.id);
        if (ImGui::GetTime() - sec.detachTime > 0.5) {
            bool inZone = (p.y > gTabBarY - 40 && p.y < gTabBarY + 110 && p.x + s.x/2 > gTabBarX && p.x + s.x/2 < gTabBarX + gWinW);
            if (inZone && !ImGui::IsMouseDown(0)) { sec.detached = false; gActiveSection = sec.id; }
            else if (inZone) ImGui::GetForegroundDrawList()->AddRect(ImVec2(gTabBarX, gTabBarY - 5), ImVec2(gTabBarX + gWinW, gTabBarY + 45), IM_COL32(255, 200, 87, 220), 10, 0, 3.0f);
        }
    }
}

static void renderInspectors() {
    for (auto it = gInspectors.begin(); it != gInspectors.end(); ) {
        ImGui::SetNextWindowPos(ImVec2(it->x, it->y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(it->w, it->h), ImGuiCond_FirstUseEver);
        char title[256]; snprintf(title, sizeof(title), "Inspector: %s##%s", it->className.c_str(), it->className.c_str());
        bool open = it->isOpen;
        ImGui::Begin(title, &open, ImGuiWindowFlags_None);
        it->isOpen = open;
        it->x = ImGui::GetWindowPos().x; it->y = ImGui::GetWindowPos().y;
        it->w = ImGui::GetWindowSize().x; it->h = ImGui::GetWindowSize().y;

        ImGui::Text("Class: %s", it->className.c_str());
        if (ImGui::Button("Find Instances")) {
            it->searching = true; it->instances.clear();
            { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingInspectorReqs.push_back({it->className}); }
        }
        ImGui::Spacing();
        if (it->searching) ImGui::TextDisabled("Searching...");
        else if (it->instances.empty()) ImGui::TextDisabled("No instances found.");
        else {
            ImGui::PushTextWrapPos(0.0f);
            for (auto& inst : it->instances) ImGui::BulletText("%s", inst.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
        if (!open) it = gInspectors.erase(it); else ++it;
    }
}

static void processPendingCalls() {
    std::vector<PendingDetailReq> details; std::vector<PendingCallReq> calls; std::vector<PendingInspectorReq> inspectors;
    { std::lock_guard<std::mutex> lk(gQueuesMutex); details.swap(gPendingDetailReqs); calls.swap(gPendingCallReqs); inspectors.swap(gPendingInspectorReqs); }
    if (details.empty() && calls.empty() && inspectors.empty()) return; if (!gHelperClass || !gVM) return;
    JNIEnv* env = nullptr; bool attached = false;
    if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) return; attached = true; }
    
    if (!details.empty()) {
        jmethodID m = env->GetStaticMethodID(gHelperClass, "requestClassDetails", "(ILjava/lang/String;)V");
        if (m) for (auto& d : details) { jstring cn = env->NewStringUTF(d.className.c_str()); env->CallStaticVoidMethod(gHelperClass, m, (jint)d.sectionId, cn); env->DeleteLocalRef(cn); }
    }
    if (!calls.empty()) {
        jmethodID m = env->GetStaticMethodID(gHelperClass, "requestMethodCall", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;ZZZ)V");
        if (m) {
            jclass strCls = env->FindClass("java/lang/String");
            for (auto& c : calls) {
                jstring jc = env->NewStringUTF(c.className.c_str()); jstring js = env->NewStringUTF(c.methodSig.c_str()); jstring jn = env->NewStringUTF(c.methodName.c_str());
                jobjectArray jpt = env->NewObjectArray(c.paramTypes.size(), strCls, nullptr); for (size_t i = 0; i < c.paramTypes.size(); i++) env->SetObjectArrayElement(jpt, i, env->NewStringUTF(c.paramTypes[i].c_str()));
                jobjectArray ja = env->NewObjectArray(c.args.size(), strCls, nullptr); for (size_t i = 0; i < c.args.size(); i++) env->SetObjectArrayElement(ja, i, env->NewStringUTF(c.args[i].c_str()));
                env->CallStaticVoidMethod(gHelperClass, m, (jint)c.sectionId, jc, js, jn, jpt, ja, c.isStatic, c.isConstructor, c.runInMain);
                env->DeleteLocalRef(jc); env->DeleteLocalRef(js); env->DeleteLocalRef(jn); env->DeleteLocalRef(jpt); env->DeleteLocalRef(ja);
            }
        }
    }
    if (!inspectors.empty()) {
        jmethodID m = env->GetStaticMethodID(gHelperClass, "requestFindInstances", "(Ljava/lang/String;)V");
        if (m) for (auto& i : inspectors) { jstring cn = env->NewStringUTF(i.className.c_str()); env->CallStaticVoidMethod(gHelperClass, m, cn); env->DeleteLocalRef(cn); }
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (attached) gVM->DetachCurrentThread();
}

static void doRender(JNIEnv*, jclass) {
    if (!gInitialized) return;
    {
        std::lock_guard<std::mutex> lk(gQueuesMutex);
        while (!gDetailResults.empty()) {
            auto r = gDetailResults.front(); gDetailResults.erase(gDetailResults.begin());
            auto* sec = findSection(r.sectionId); if (!sec) continue;
            auto& det = sec->details[r.className]; det.fields = r.fields; det.methods.clear();
            for (auto& raw : r.methodRaws) det.methods.push_back(parseMethodRaw(raw));
            det.loading = false;
        }
        while (!gMethodResults.empty()) {
            auto r = gMethodResults.front(); gMethodResults.erase(gMethodResults.begin());
            auto* sec = findSection(r.sectionId); if (!sec) continue;
            auto& st = sec->details[r.className].methodStates[r.methodSig];
            st.result = r.result; st.hasResult = true; st.calling = false;
        }
        while (!gInspectorResults.empty()) {
            auto r = gInspectorResults.front(); gInspectorResults.erase(gInspectorResults.begin());
            for (auto& insp : gInspectors) { if (insp.className == r.className) { insp.instances = r.instances; insp.searching = false; } }
        }
    }

    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplAndroid_NewFrame();
    ImGuiIO& io = ImGui::GetIO(); io.FontGlobalScale = g_settingFontScale;
    ImGui::NewFrame();
    if (g_menuVisible) { renderMainWindow(); renderDetachedWindows(); renderInspectors(); }
    renderFAB();
    ImGui::Render();
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (gRequestAddSection) { addSectionImpl(); gRequestAddSection = false; }
    if (!gPendingCloseSections.empty()) { std::vector<int> c = gPendingCloseSections; gPendingCloseSections.clear(); for (int id : c) closeSectionImpl(id); }
    processPendingCalls();
}

static jboolean doTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (!gInitialized) return JNI_FALSE;
    ImGuiIO& io = ImGui::GetIO(); io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (action == 0) {
        bool inFAB = (g_fabX >= 0 && x >= g_fabX && x <= g_fabX + g_fabSize && g_fabY >= 0 && y >= g_fabY && y <= g_fabY + g_fabSize);
        if (inFAB) { g_fabDown = true; g_fabMoved = false; g_fabDownAtX = x; g_fabDownAtY = y; g_fabDragStartX = g_fabX; g_fabDragStartY = g_fabY; return JNI_TRUE; }
    } else if (g_fabDown) {
        if (action == 2) {
            float dx = x - g_fabDownAtX, dy = y - g_fabDownAtY; if (fabsf(dx) > 20.0f || fabsf(dy) > 20.0f) g_fabMoved = true;
            if (g_fabMoved) { g_fabX = g_fabDragStartX + dx; g_fabY = g_fabDragStartY + dy; if (g_fabX < 0) g_fabX = 0; if (g_fabY < 0) g_fabY = 0; if (g_fabX > io.DisplaySize.x - g_fabSize) g_fabX = io.DisplaySize.x - g_fabSize; if (g_fabY > io.DisplaySize.y - g_fabSize) g_fabY = io.DisplaySize.y - g_fabSize; }
            return JNI_TRUE;
        }
        if (action == 1 || action == 3) { g_fabDown = false; if (!g_fabMoved) g_menuVisible = !g_menuVisible; return JNI_TRUE; }
    }
    if (!g_menuVisible) return JNI_FALSE;
    if (action == 0) { g_virtX = x; g_virtY = y; g_realPrevX = x; g_realPrevY = y; io.AddMousePosEvent(g_virtX, g_virtY); io.AddMouseButtonEvent(0, true); }
    else if (action == 1 || action == 3) { io.AddMousePosEvent(g_virtX, g_virtY); io.AddMouseButtonEvent(0, false); }
    else if (action == 2) {
        float dx = x - g_realPrevX, dy = y - g_realPrevY; g_realPrevX = x; g_realPrevY = y;
        bool inWin = (g_virtX >= gWinX && g_virtX <= gWinX + gWinW && g_virtY >= gWinY && g_virtY <= gWinY + gWinH);
        bool inTitle = inWin && (g_virtY <= gWinY + g_titleH); float scale = 1.0f;
        if (inTitle) scale = g_settingDragSpeed; else if (inWin) scale = g_settingScrollSpeed;
        g_virtX += dx * scale; g_virtY += dy * scale; if (g_virtX < 0) g_virtX = 0; if (g_virtY < 0) g_virtY = 0; if (g_virtX > io.DisplaySize.x) g_virtX = io.DisplaySize.x; if (g_virtY > io.DisplaySize.y) g_virtY = io.DisplaySize.y;
        io.AddMousePosEvent(g_virtX, g_virtY);
    }
    return io.WantCaptureMouse ? JNI_TRUE : JNI_FALSE;
}

static jboolean doWantKeyboard(JNIEnv*, jclass) { return gInitialized && ImGui::GetIO().WantTextInput ? JNI_TRUE : JNI_FALSE; }
static void doAddInputChar(JNIEnv*, jclass, jint cp) { if (gInitialized && cp > 0) ImGui::GetIO().AddInputCharacter((unsigned int)cp); }
static void doAddKeyEvent(JNIEnv*, jclass, jint keycode, jboolean down) {
    if (!gInitialized) return; ImGuiKey k = ImGuiKey_None;
    switch (keycode) { case 67: k = ImGuiKey_Backspace; break; case 66: k = ImGuiKey_Enter; break; case 65: k = ImGuiKey_Tab; break; default: return; }
    ImGui::GetIO().AddKeyEvent(k, down ? true : false);
}
static void doSetClassList(JNIEnv* env, jclass, jobjectArray arr) {
    std::lock_guard<std::mutex> lk(gClassListMutex); gClassList.clear(); if (!arr) return;
    jsize n = env->GetArrayLength(arr); gClassList.reserve((size_t)n);
    for (jsize i = 0; i < n; i++) { jstring js = (jstring)env->GetObjectArrayElement(arr, i); const char* cs = env->GetStringUTFChars(js, nullptr); if (cs) { gClassList.emplace_back(cs); env->ReleaseStringUTFChars(js, cs); } env->DeleteLocalRef(js); }
}
static void doSetClassDetails(JNIEnv* env, jclass, jint sectionId, jstring className, jobjectArray fields, jobjectArray methods) {
    DetailResult r; r.sectionId = (int)sectionId;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (fields) { jsize n = env->GetArrayLength(fields); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(fields, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.fields.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    if (methods) { jsize n = env->GetArrayLength(methods); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(methods, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.methodRaws.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gDetailResults.push_back(std::move(r));
}
static void doSetMethodResult(JNIEnv* env, jclass, jint sectionId, jstring className, jstring methodSig, jstring result) {
    MethodResult r; r.sectionId = (int)sectionId;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (methodSig) { const char* c = env->GetStringUTFChars(methodSig, nullptr); r.methodSig = c ? c : ""; if (c) env->ReleaseStringUTFChars(methodSig, c); }
    if (result) { const char* c = env->GetStringUTFChars(result, nullptr); r.result = c ? c : ""; if (c) env->ReleaseStringUTFChars(result, c); }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gMethodResults.push_back(std::move(r));
}
static void doSetInspectorInstances(JNIEnv* env, jclass, jstring className, jobjectArray instances) {
    InspectorResult r;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (instances) { jsize n = env->GetArrayLength(instances); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(instances, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.instances.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gInspectorResults.push_back(std::move(r));
}
static void doSetAvailableInstances(JNIEnv* env, jclass, jstring className, jobjectArray instances) {
    AvailableInstancesResult r;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (instances) { jsize n = env->GetArrayLength(instances); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(instances, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.instances.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gAvailableInstancesResults.push_back(std::move(r));
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    gVM = vm; if (!loadDex(env)) return JNI_VERSION_1_6;
    JNINativeMethod methods[] = {
        {(char*)"nativeInit",            (char*)"(Landroid/view/Surface;)V",   (void*)doInit},
        {(char*)"nativeResize",          (char*)"(II)V",                        (void*)doResize},
        {(char*)"nativeRender",          (char*)"()V",                          (void*)doRender},
        {(char*)"nativeTouch",           (char*)"(IFF)Z",                       (void*)doTouch},
        {(char*)"nativeSetClassList",    (char*)"([Ljava/lang/String;)V",       (void*)doSetClassList},
        {(char*)"nativeWantKeyboard",    (char*)"()Z",                          (void*)doWantKeyboard},
        {(char*)"nativeAddInputChar",    (char*)"(I)V",                         (void*)doAddInputChar},
        {(char*)"nativeAddKeyEvent",     (char*)"(IZ)V",                        (void*)doAddKeyEvent},
        {(char*)"nativeSetClassDetails", (char*)"(ILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V", (void*)doSetClassDetails},
        {(char*)"nativeSetMethodResult", (char*)"(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void*)doSetMethodResult},
        {(char*)"nativeSetInspectorInstances", (char*)"(Ljava/lang/String;[Ljava/lang/String;)V", (void*)doSetInspectorInstances},
        {(char*)"nativeSetAvailableInstances", (char*)"(Ljava/lang/String;[Ljava/lang/String;)V", (void*)doSetAvailableInstances},
    };
    if (env->RegisterNatives(gHelperClass, methods, 12) != 0) { env->ExceptionClear(); return JNI_VERSION_1_6; }
    pthread_t t; if (pthread_create(&t, nullptr, pollThread, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
