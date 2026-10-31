#include <jni.h>
#include <mutex>
static JavaVM* gVM = nullptr;
static JavaVM* g_jvm = nullptr;
static bool g_menuVisible = true;
static bool gMenuOpen = false;

static std::once_flag frida_once;
#include <android/log.h>
#include <android/native_window_jni.h>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include <GLES3/gl3.h>

#include "Log.h"
#include "dex_data.h"
#include "ImGui.h"
#include "Input.h"




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
#include <frida-gumjs.h>

static std::vector<std::string> gCallLog;
static std::mutex gCallLogMutex;

#define MY_PI 3.14159265358979323846f
#define LOG_TAG "ImGuiTool"
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#endif
static jclass gHelperClass = nullptr;
static ANativeWindow* gWindow = nullptr;
static bool gInitialized = false;
static int gWidth = 0, gHeight = 0;
static std::vector<std::string> gClassList;
static std::mutex gClassListMutex;

struct PatchItem { int id; std::string type, className, targetName, originalValue; char currentValue[256] = {0}; bool isActive; };
static std::vector<PatchItem> gPatches;
static std::mutex gPatchesMutex;

struct FieldInfo { std::string display, name, type; bool isStatic; char editBuf[256] = {0}; std::string currentValue; bool hasValue = false; bool updating = false; };
struct MethodInfo { std::string display, name, returnType; std::vector<std::string> paramTypes; bool isStatic, isConstructor; };
struct MethodCallState {
    char argBufs[8][256] = {}; int numParams = 0; std::string result;
    bool hasResult = false; bool calling = false; bool runInMain = false;
    int widgetType[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    std::vector<std::string> suggestions[8];
    int selectedSuggestion[8] = {0,0,0,0,0,0,0,0};
    bool needsInit = true; bool needsFocus = false;
};
struct ClassDetail { bool loading = false; std::vector<FieldInfo> fields; std::vector<MethodInfo> methods; std::map<std::string, MethodCallState> methodStates; std::set<std::string> expandedMethods; };
struct Section { int id = 0; char filter[128] = {0}; bool detached = false; float detX = 100, detY = 100, detW = 550, detH = 550; double detachTime = 0; std::map<std::string, ClassDetail> details; std::set<std::string> expandedClasses; };

static std::vector<Section> gSections;
static int gNextSectionId = 1, gActiveSection = 0;
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
struct PendingFieldReq { int sectionId; std::string className; std::string fieldName; std::string instanceName; std::string value; bool isSet; };
struct PendingPatchAction { int id; std::string action; std::string value; };
struct DetailResult { int sectionId; std::string className; std::vector<std::string> fields; std::vector<std::string> methods; };
struct MethodResult { int sectionId; std::string className; std::string methodSig; std::string result; };
struct FieldResult { int sectionId; std::string className; std::string fieldName; std::string value; bool isSet; };

static std::vector<PendingDetailReq> gPendingDetailReqs;
static std::vector<PendingCallReq> gPendingCallReqs;
static std::vector<PendingFieldReq> gPendingFieldReqs;
static std::vector<PendingPatchAction> gPendingPatchActions;
static std::vector<DetailResult> gDetailResults;
static std::vector<MethodResult> gMethodResults;
static std::vector<FieldResult> gFieldResults;
static std::mutex gQueuesMutex;

static float g_settingFontScale = 1.00f, g_settingDragSpeed = 0.55f, g_settingScrollSpeed = 1.40f;
static float g_fabSize = 58.0f, g_fabX = -1.0f, g_fabY = -1.0f;
static bool g_fabDown = false, g_fabMoved = false;
static float g_fabDownAtX = 0, g_fabDownAtY = 0, g_fabDragStartX = 0, g_fabDragStartY = 0;
static float g_virtX = 0, g_virtY = 0, g_realPrevX = 0, g_realPrevY = 0, g_titleH = 30.0f;

// ====== FRIDA INIT (FIXED: 6 args for create_sync) ======
static GumScript* g_script = nullptr;















static GumScriptBackend* g_backend = nullptr;

const gchar* PATCH_AGENT_JS = R"(Java.perform(function() {
  function log(m){ console.log("[DexTool] "+m); try{ send({type:"log", msg:m}); }catch(e){} }
  log("agent loaded");
  var HOOKED = {};
  recv(function(raw) {
    try {
      var cmd = JSON.parse(raw);
      log("recv: "+raw);
      if (cmd.action !== "patchMethod") return;
      var key = cmd.className+"#"+cmd.methodName;
      if (HOOKED[key]) { log("already patched: "+key); return; }
      var loaders = Java.enumerateClassLoadersSync();
      var loader = null;
      for (var i=0;i<loaders.length;i++){ try{ if(loaders[i].findClass(cmd.className)){ loader=loaders[i]; break; } }catch(e){} }
      if (loader) Java.classFactory.loader = loader;
      var cls = Java.use(cmd.className);
      var ovs = cls[cmd.methodName].overloads;
      var target = null;
      for (var i=0;i<ovs.length;i++){ if(target===null && ovs[i].argumentTypes.length===0) target=ovs[i]; }
      if (!target) target = ovs[0];
      var rt = target.returnType.className;
      var rv = cmd.returnValue;
      target.implementation = function() {
        log("HOOK HIT: "+cmd.methodName);
        if (rt==="int"||rt==="long"||rt==="short"||rt==="byte") return parseInt(rv,10);
        if (rt==="float"||rt==="double") return parseFloat(rv);
        if (rt==="boolean") return rv==="true"||rv==="1";
        if (rt==="void") return undefined;
        return rv;
      };
      HOOKED[key] = true;
      log("PATCHED: "+key);
    } catch(e) { log("ERROR: "+e.message); }
  });
  send({type:"ready"});
});)";

static void on_script_message(const gchar* message, GBytes* data, gpointer user_data) {
    LOGI("Frida JS: %s", message); if (message && strstr(message, "\"type\":\"log\"")) { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[JS] ")+message); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); }
}

static void init_frida_script() {
    GError* error = nullptr;
    g_backend = gum_script_backend_obtain_qjs();
    // FIXED: 6 arguments: self, name, source, snapshot, cancellable, error
    g_script = gum_script_backend_create_sync(g_backend, "patch_agent", PATCH_AGENT_JS, nullptr, nullptr, &error);
    if (error || !g_script) {
        LOGI("Frida create error: %s", error ? error->message : "null script");
        if (error) g_error_free(error);
        return;
    }
    gum_script_set_message_handler(g_script, on_script_message, nullptr, nullptr);
    gum_script_load_sync(g_script, nullptr);
    LOGI("Frida Patch Agent loaded");
}

static void send_to_js(const char* json) {
    if (g_script) gum_script_post(g_script, json, nullptr);
}

// ====== SIGIL ======
















extern "C" JNIEXPORT void JNICALL
Java_com_menu_imgui_Helper_nativePatchMethod(JNIEnv* env, jclass, jstring jc, jstring jm, jstring jv) {
    if (!g_jvm) env->GetJavaVM(&g_jvm);
    if (!g_script) return;
    const char* c = env->GetStringUTFChars(jc, nullptr);
    const char* m = env->GetStringUTFChars(jm, nullptr);
    const char* v = env->GetStringUTFChars(jv, nullptr);
    std::string json = "{\"action\":\"patchMethod\",\"className\":\"";
    json += c; json += "\",\"methodName\":\""; json += m;
    json += "\",\"returnValue\":\""; json += v; json += "\"}";
    gum_script_post(g_script, json.c_str(), nullptr);
    env->ReleaseStringUTFChars(jc, c);
    env->ReleaseStringUTFChars(jm, m);
    env->ReleaseStringUTFChars(jv, v);
}

static void drawSigil(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 gold = IM_COL32(255, 200, 87, 255), cyan = IM_COL32(77, 208, 225, 220);
    const ImU32 dark = IM_COL32(10, 12, 16, 255), red = IM_COL32(255, 80, 80, 210);
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



static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 16; s.ChildRounding = 12; s.FrameRounding = 10;
    s.GrabRounding = 10; s.PopupRounding = 14; s.ScrollbarRounding = 10;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.WindowPadding = ImVec2(18, 18); s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10); s.ItemInnerSpacing = ImVec2(8, 8);
    s.ScrollbarSize = 16; s.GrabMinSize = 14;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.13f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.00f, 0.78f, 0.34f, 0.35f);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.25f, 0.31f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.23f, 0.04f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.00f, 0.78f, 0.34f, 0.35f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 0.78f, 0.34f, 0.55f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.00f, 0.78f, 0.34f, 0.75f);
    c[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.82f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.92f, 0.98f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(1.00f, 0.78f, 0.34f, 0.50f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 0.78f, 0.34f, 0.75f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.78f, 0.34f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(1.00f, 0.78f, 0.34f, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.00f, 0.78f, 0.34f, 0.42f);
    c[ImGuiCol_HeaderActive] = ImVec4(1.00f, 0.78f, 0.34f, 0.60f);
    c[ImGuiCol_Separator] = ImVec4(1.00f, 0.78f, 0.34f, 0.25f);
}




static void splitStr(const std::string& s, char d, std::vector<std::string>& out) {
    out.clear(); size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) { if (i == s.size() || s[i] == d) { out.push_back(s.substr(start, i - start)); start = i + 1; } }
}
static std::string joinStrs(const std::vector<std::string>& v, const char* sep) {
    std::string out; for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; } return out;
}
static MethodInfo parseMethodDisplay(const std::string& display) {
    MethodInfo m; m.display = display;
    size_t p1 = display.find('('), p2 = display.find(')');
    if (p1 != std::string::npos) {
        m.name = display.substr(0, p1);
        size_t lastSpace = m.name.rfind(' ');
        if (lastSpace != std::string::npos) { m.returnType = m.name.substr(0, lastSpace); m.name = m.name.substr(lastSpace + 1); }
        if (p2 != std::string::npos && p2 > p1) { std::string params = display.substr(p1 + 1, p2 - p1 - 1); if (!params.empty()) splitStr(params, ',', m.paramTypes); }
    }
    m.isConstructor = m.name.find("<init>") != std::string::npos;
    m.isStatic = display.find("static ") != std::string::npos;
    return m;
}
static Section* findSection(int id) { for (auto& s : gSections) if (s.id == id) return &s; return nullptr; }

static void initMethodWidgets(MethodCallState& st, const std::vector<std::string>& types) {
    if (!gHelperClass || !gVM) return;
    JNIEnv* env = nullptr; bool attached = false;
    if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) return; attached = true; }
    jmethodID getType = env->GetStaticMethodID(gHelperClass, "getArgWidgetType", "(Ljava/lang/String;)I");
    jmethodID getSugg = env->GetStaticMethodID(gHelperClass, "getArgSuggestions", "(Ljava/lang/String;)[Ljava/lang/String;");
    for (size_t i = 0; i < types.size() && i < 8; i++) {
        jstring js = env->NewStringUTF(types[i].c_str()); if (!js) continue;
        st.widgetType[i] = 0;
        if (getType) { jint wt = env->CallStaticIntMethod(gHelperClass, getType, js); if (!env->ExceptionCheck()) st.widgetType[i] = (int)wt; else env->ExceptionClear(); }
        st.suggestions[i].clear();
        if (getSugg) {
            jobjectArray arr = (jobjectArray)env->CallStaticObjectMethod(gHelperClass, getSugg, js);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (arr) {
                jsize n = env->GetArrayLength(arr);
                for (jsize j = 0; j < n; j++) { jstring s = (jstring)env->GetObjectArrayElement(arr, j); const char* c = env->GetStringUTFChars(s, nullptr); if (c) st.suggestions[i].push_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); }
                env->DeleteLocalRef(arr);
            }
        }
        if (!st.suggestions[i].empty()) { st.selectedSuggestion[i] = 0; snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%s", st.suggestions[i][0].c_str()); }
        env->DeleteLocalRef(js);
    }
    st.needsInit = false;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) gVM->DetachCurrentThread();
}

static void renderField(Section& sec, const std::string& className, FieldInfo& f) {
    ImGui::PushID(f.name.c_str());
    if (ImGui::TreeNodeEx(f.display.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Indent(20);
        if (ImGui::Button("Get")) { PendingFieldReq r; r.sectionId = sec.id; r.className = className; r.fieldName = f.name; r.instanceName = "[Live] Current Activity"; r.isSet = false; { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingFieldReqs.push_back(r); } f.updating = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::InputText("##edit", f.editBuf, sizeof(f.editBuf));
        if (ImGui::Button("Set")) { PendingFieldReq r; r.sectionId = sec.id; r.className = className; r.fieldName = f.name; r.instanceName = "[Live] Current Activity"; r.value = f.editBuf; r.isSet = true; { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingFieldReqs.push_back(r); } f.updating = true; }
        ImGui::SameLine();
        if (ImGui::Button("Add Patch")) {
            JNIEnv* env = nullptr; bool attached = false;
            if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr; else attached = true; }
            if (env && gHelperClass) { jmethodID m = env->GetStaticMethodID(gHelperClass, "requestPatchField", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"); if (m) { jstring jc = env->NewStringUTF(className.c_str()); jstring jf = env->NewStringUTF(f.name.c_str()); jstring jv = env->NewStringUTF(f.currentValue.c_str()); env->CallStaticVoidMethod(gHelperClass, m, jc, jf, jv); env->DeleteLocalRef(jc); env->DeleteLocalRef(jf); env->DeleteLocalRef(jv); } }
            if (attached) gVM->DetachCurrentThread();
        }
        if (f.updating) { ImGui::SameLine(); ImGui::TextDisabled("..."); }
        else if (f.hasValue) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1), "%s", f.currentValue.c_str()); }
        ImGui::Unindent(20); ImGui::TreePop();
    }
    ImGui::PopID();
}

static void renderMethodPanel(Section& sec, const std::string& className, MethodInfo& m, MethodCallState& st) {
    ImGui::Indent(20);
    if (st.needsInit) initMethodWidgets(st, m.paramTypes);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Return: %s", m.returnType.c_str());
    ImGui::PopTextWrapPos();
    for (int i = 0; i < st.numParams; i++) {
        ImGui::PushID(i);
        ImGui::TextDisabled("arg%d (%s)", i, m.paramTypes[i].c_str());
        int wt = st.widgetType[i];
        if (wt == 0) { if (st.needsFocus && i == 0) { ImGui::SetKeyboardFocusHere(); st.needsFocus = false; } ImGui::SetNextItemWidth(-1); ImGui::InputText("##arg", st.argBufs[i], sizeof(st.argBufs[i])); }
        else if (wt == 1) { int bv = (strcmp(st.argBufs[i], "true") == 0) ? 1 : 0; if (ImGui::Combo("##bool", &bv, "false\0true\0")) snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%s", bv ? "true" : "false"); }
        else if (wt == 2 || wt == 4) {
            if (st.suggestions[i].empty()) ImGui::TextDisabled("(no options)");
            else { int sel = st.selectedSuggestion[i]; if (sel >= (int)st.suggestions[i].size()) sel = 0;
                if (ImGui::BeginCombo("##sugg", st.suggestions[i][sel].c_str())) {
                    for (int n = 0; n < (int)st.suggestions[i].size(); n++) { bool is_sel = (sel == n); if (ImGui::Selectable(st.suggestions[i][n].c_str(), is_sel)) { st.selectedSuggestion[i] = n; snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%s", st.suggestions[i][n].c_str()); } if (is_sel) ImGui::SetItemDefaultFocus(); }
                    ImGui::EndCombo(); } }
        }
        else if (wt == 3) { ImGui::TextColored(ImVec4(0.30f,0.82f,0.88f,1), "[Auto-Inject]"); ImGui::SameLine(); if (!st.suggestions[i].empty()) { snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%s", st.suggestions[i][0].c_str()); ImGui::TextDisabled("%s", st.suggestions[i][0].c_str()); } else ImGui::TextDisabled("Pending..."); }
        else if (wt == 5) { std::string hint = "comma-separated"; ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##arr", hint.c_str(), st.argBufs[i], sizeof(st.argBufs[i])); }
        else if (wt == 6) {
    int iv = 0; try { iv = std::stoi(st.argBufs[i]); } catch (...) {}
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##int", &iv)) snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%d", iv); }
else if (wt == 7) {
    float fv = 0.0f; try { fv = std::stof(st.argBufs[i]); } catch (...) {}
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputFloat("##float", &fv)) snprintf(st.argBufs[i], sizeof(st.argBufs[i]), "%.6g", fv); }
else { ImGui::SetNextItemWidth(-1); ImGui::InputText("##arg", st.argBufs[i], sizeof(st.argBufs[i])); }
        ImGui::PopID();
    }
    ImGui::Checkbox("Call in Main Thread", &st.runInMain);
    if (ImGui::Button("Refresh args")) { st.needsInit = true; for (int i = 0; i < 8; i++) { st.widgetType[i] = -1; st.suggestions[i].clear(); st.selectedSuggestion[i] = 0; } }
    ImGui::SameLine();
    if (ImGui::Button("Call", ImVec2(100, 0))) {
        PendingCallReq c; c.sectionId = sec.id; c.className = className; c.methodSig = m.display; c.methodName = m.name; c.paramTypes = m.paramTypes; c.args.resize(st.numParams);
        for (int i = 0; i < st.numParams; i++) c.args[i] = std::string(st.argBufs[i]);
        c.isStatic = m.isStatic; c.isConstructor = m.isConstructor; c.runInMain = st.runInMain;
        { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingCallReqs.push_back(c); { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } { std::lock_guard<std::mutex> lk(gCallLogMutex); gCallLog.push_back(std::string("[CALL] ")+className+"."+m.name); if (gCallLog.size()>200) gCallLog.erase(gCallLog.begin()); } }
        st.calling = true; st.hasResult = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Patch Return")) {
        JNIEnv* env = nullptr; bool attached = false;
        if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr; else attached = true; }
        if (env && gHelperClass) {
            jmethodID m_patch = env->GetStaticMethodID(gHelperClass, "requestPatchMethod", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (m_patch) { jstring jc = env->NewStringUTF(className.c_str()); jstring js = env->NewStringUTF(m.display.c_str()); jstring jt = env->NewStringUTF(m.returnType.c_str()); env->CallStaticVoidMethod(gHelperClass, m_patch, jc, js, jt); env->DeleteLocalRef(jc); env->DeleteLocalRef(js); env->DeleteLocalRef(jt); }
        }
        if (attached) gVM->DetachCurrentThread();
    }
    if (st.calling) { ImGui::SameLine(); ImGui::TextDisabled("Executing..."); }
    else if (st.hasResult) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 0.3f, 1.0f)); ImGui::PushTextWrapPos(0.0f); ImGui::TextWrapped("%s", st.result.c_str()); ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); }
    ImGui::Unindent(20);
}

static void renderClassDetail(Section& sec, const std::string& className) {
    auto it = sec.details.find(className); if (it == sec.details.end()) return;
    ClassDetail& det = it->second;
    ImGui::Indent(20);
    ImGui::Separator();
    if (det.loading) { ImGui::TextDisabled("Loading..."); ImGui::Unindent(20); return; }
    if (!det.fields.empty()) {
        ImGui::TextColored(ImVec4(0.30f,0.82f,0.88f,1), "Fields");
        for (auto& f : det.fields) renderField(sec, className, f);
        ImGui::Spacing();
    }
    if (!det.methods.empty()) {
        ImGui::TextColored(ImVec4(0.30f,0.82f,0.88f,1), "Methods");
        for (auto& m : det.methods) {
            ImGui::PushID(m.display.c_str());
            ImGui::PushTextWrapPos(0.0f);
            if (ImGui::TreeNodeEx(m.display.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                auto& st = det.methodStates[m.display];
                if (st.numParams == 0 && m.paramTypes.size() > 0) { st.numParams = (int)m.paramTypes.size(); if (st.numParams > 8) st.numParams = 8; for (int i = 0; i < st.numParams; i++) st.argBufs[i][0] = 0; st.needsFocus = true; st.needsInit = true; }
                renderMethodPanel(sec, className, m, st);
                ImGui::TreePop();
            }
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
    }
    ImGui::Unindent(20); ImGui::Separator();
}

static void renderSectionContent(Section& sec) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search classes...", sec.filter, sizeof(sec.filter));
    ImGui::Spacing();
    std::vector<std::string> visible;
    { std::lock_guard<std::mutex> lk(gClassListMutex);
      if (sec.filter[0]) { for (auto& c : gClassList) if (c.find(sec.filter) != std::string::npos) visible.push_back(c); }
      else visible = gClassList; }
    ImGui::TextDisabled("%zu classes", visible.size());
    ImGui::BeginChild("##list", ImVec2(0, 0), true);
    for (auto& name : visible) {
        ImGui::PushID(name.c_str());
        bool node_open = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        if (ImGui::IsItemClicked()) { gPressedClassName = name; gPressedClassSectionId = sec.id; gPressedClassStart = ImGui::GetTime(); gClassLongPressed = false; }
        if (node_open) {
            if (sec.details.count(name) == 0) { sec.details[name].loading = true; { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingDetailReqs.push_back({sec.id, name}); } }
            renderClassDetail(sec, name);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void renderPatchesTab() {
    std::lock_guard<std::mutex> lk(gPatchesMutex);
    if (gPatches.empty()) { ImGui::TextDisabled("No active patches."); return; }
    ImGui::Text("Active Patches (%zu)", gPatches.size());
    ImGui::Separator();
    ImGui::BeginChild("##patchlist", ImVec2(0, 0), true);
    for (auto& p : gPatches) {
        ImGui::PushID(p.id);
        ImGui::TextColored(ImVec4(1,0.78f,0.34f,1), "[%s] %s", p.type.c_str(), p.className.c_str());
        ImGui::TextDisabled("Target: %s", p.targetName.c_str());
        ImGui::TextDisabled("Original: %s", p.originalValue.c_str());
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##val", p.currentValue, sizeof(p.currentValue));
        ImGui::SameLine();
        if (ImGui::Button("Update")) { std::lock_guard<std::mutex> qlk(gQueuesMutex); gPendingPatchActions.push_back({p.id, "update", std::string(p.currentValue)}); }
        ImGui::SameLine();
        bool active = p.isActive;
        if (ImGui::Checkbox("Active", &active)) { std::lock_guard<std::mutex> qlk(gQueuesMutex); gPendingPatchActions.push_back({p.id, "toggle", ""}); }
        ImGui::SameLine();
        if (ImGui::Button("Restore")) { std::lock_guard<std::mutex> qlk(gQueuesMutex); gPendingPatchActions.push_back({p.id, "remove", ""}); }
        ImGui::Separator();
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
    if (ImGui::Button("Reset to defaults", ImVec2(-1, 0))) { g_settingFontScale = 1.00f; g_settingDragSpeed = 0.55f; g_settingScrollSpeed = 1.40f; }
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
                if (!ImGui::IsMouseDown(0)) { gPressedClassName.clear(); gClassLongPressed = false; }
                else if (!gClassLongPressed && ImGui::GetTime() - gPressedClassStart > 0.5) {
                    gClassLongPressed = true;
                    Section s; s.id = gNextSectionId++; s.detached = true; s.expandedClasses.insert(gPressedClassName);
                    s.detX = io.DisplaySize.x / 2 - 250; s.detY = io.DisplaySize.y / 2 - 250; s.detW = 500; s.detH = 500;
                    s.filter[0] = 0; snprintf(s.filter, sizeof(s.filter), "%s", gPressedClassName.c_str());
                    gSections.push_back(s);
                    { std::lock_guard<std::mutex> lk(gQueuesMutex); gPendingDetailReqs.push_back({s.id, gPressedClassName}); }
                    gPressedClassName.clear();
                }
            }

            Section* active = findSection(gActiveSection);
            if (!active || active->detached) { active = nullptr; for (auto& s : gSections) if (!s.detached) { active = &s; gActiveSection = s.id; break; } }
            if (active) renderSectionContent(*active); else ImGui::TextDisabled("No sections.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) { std::lock_guard<std::mutex> lk(gCallLogMutex); ImGui::BeginChild("##logchild", ImVec2(0,0), true); for (auto& l : gCallLog) ImGui::TextUnformatted(l.c_str()); ImGui::EndChild(); ImGui::EndTabItem(); } if (ImGui::BeginTabItem("Patches")) { renderPatchesTab(); ImGui::EndTabItem(); }
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

static void processPendingCalls() {
    std::vector<PendingDetailReq> details; std::vector<PendingCallReq> calls; std::vector<PendingFieldReq> flds; std::vector<PendingPatchAction> patchActs;
    { std::lock_guard<std::mutex> lk(gQueuesMutex); details.swap(gPendingDetailReqs); calls.swap(gPendingCallReqs); flds.swap(gPendingFieldReqs); patchActs.swap(gPendingPatchActions); }
    if (details.empty() && calls.empty() && flds.empty() && patchActs.empty()) return; if (!gHelperClass || !gVM) return;
    JNIEnv* env = nullptr; bool attached = false;
    if (gVM->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) return; attached = true; }
    if (!details.empty()) { jmethodID m = env->GetStaticMethodID(gHelperClass, "requestClassDetails", "(ILjava/lang/String;)V"); if (m) for (auto& d : details) { jstring cn = env->NewStringUTF(d.className.c_str()); env->CallStaticVoidMethod(gHelperClass, m, (jint)d.sectionId, cn); env->DeleteLocalRef(cn); } }
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
    if (!flds.empty()) {
        jmethodID mGet = env->GetStaticMethodID(gHelperClass, "requestGetField", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        jmethodID mSet = env->GetStaticMethodID(gHelperClass, "requestSetField", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        for (auto& f : flds) {
            jstring jc = env->NewStringUTF(f.className.c_str()); jstring jf = env->NewStringUTF(f.fieldName.c_str()); jstring ji = env->NewStringUTF(f.instanceName.c_str());
            if (f.isSet && mSet) { jstring jv = env->NewStringUTF(f.value.c_str()); env->CallStaticVoidMethod(gHelperClass, mSet, (jint)f.sectionId, jc, jf, ji, jv); env->DeleteLocalRef(jv); }
            else if (!f.isSet && mGet) { env->CallStaticVoidMethod(gHelperClass, mGet, (jint)f.sectionId, jc, jf, ji); }
            env->DeleteLocalRef(jc); env->DeleteLocalRef(jf); env->DeleteLocalRef(ji);
        }
    }
    if (!patchActs.empty()) {
        jmethodID mToggle = env->GetStaticMethodID(gHelperClass, "togglePatch", "(IZ)V");
        jmethodID mUpdate = env->GetStaticMethodID(gHelperClass, "updatePatchValue", "(ILjava/lang/String;)V");
        jmethodID mRemove = env->GetStaticMethodID(gHelperClass, "removePatch", "(I)V");
        for (auto& a : patchActs) {
            if (a.action == "update" && mUpdate) { jstring js = env->NewStringUTF(a.value.c_str()); env->CallStaticVoidMethod(gHelperClass, mUpdate, (jint)a.id, js); env->DeleteLocalRef(js); }
            else if (a.action == "toggle" && mToggle) { env->CallStaticVoidMethod(gHelperClass, mToggle, (jint)a.id, (jboolean)true); }
            else if (a.action == "remove" && mRemove) { env->CallStaticVoidMethod(gHelperClass, mRemove, (jint)a.id); }
        }
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (attached) gVM->DetachCurrentThread();
}

static void doRender(JNIEnv*, jclass) {
    if (!gInitialized) return;
    { std::lock_guard<std::mutex> lk(gQueuesMutex);
      while (!gDetailResults.empty()) {
          auto r = gDetailResults.front(); gDetailResults.erase(gDetailResults.begin());
          auto* sec = findSection(r.sectionId); if (!sec) continue;
          auto& det = sec->details[r.className]; det.loading = false;
          det.fields.clear(); for (auto& f : r.fields) { FieldInfo fi; fi.display = f; det.fields.push_back(fi); }
          det.methods.clear(); for (auto& m : r.methods) det.methods.push_back(parseMethodDisplay(m));
      }
      while (!gMethodResults.empty()) {
          auto r = gMethodResults.front(); gMethodResults.erase(gMethodResults.begin());
          auto* sec = findSection(r.sectionId); if (!sec) continue;
          auto& st = sec->details[r.className].methodStates[r.methodSig];
          st.result = r.result; st.hasResult = true; st.calling = false;
      }
      while (!gFieldResults.empty()) {
          auto r = gFieldResults.front(); gFieldResults.erase(gFieldResults.begin());
          auto* sec = findSection(r.sectionId); if (!sec) continue;
          auto& det = sec->details[r.className];
          for (auto& f : det.fields) if (f.name == r.fieldName) { f.currentValue = r.value; f.hasValue = true; f.updating = false; snprintf(f.editBuf, sizeof(f.editBuf), "%s", r.value.c_str()); break; }
      }
    }
    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplAndroid_NewFrame();
    ImGuiIO& io = ImGui::GetIO(); io.FontGlobalScale = g_settingFontScale;
    ImGui::NewFrame();
    if (g_menuVisible) { renderMainWindow(); renderDetachedWindows(); }
    renderFAB();
    ImGui::Render();
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (gRequestAddSection) { Section s; s.id = gNextSectionId++; gSections.push_back(std::move(s)); gActiveSection = gSections.back().id; gRequestAddSection = false; }
    if (!gPendingCloseSections.empty()) {
        std::vector<int> c = gPendingCloseSections; gPendingCloseSections.clear();
        for (int id : c) {
            gSections.erase(std::remove_if(gSections.begin(), gSections.end(), [id](const Section& s) { return s.id == id; }), gSections.end());
            if (gActiveSection == id) { gActiveSection = 0; for (auto& s : gSections) if (!s.detached) { gActiveSection = s.id; break; } }
        }
    }
    processPendingCalls();
}

static jboolean doTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (!gInitialized) return JNI_FALSE;
    ImGuiIO& io = ImGui::GetIO(); io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (action == 0) {
        bool inFAB = (g_fabX >= 0 && x >= g_fabX && x <= g_fabX + g_fabSize && g_fabY >= 0 && y >= g_fabY && y <= g_fabY + g_fabSize);
        if (inFAB) { g_fabDown = true; g_fabMoved = false; g_fabDownAtX = x; g_fabDownAtY = y; g_fabDragStartX = g_fabX; g_fabDragStartY = g_fabY; return JNI_TRUE; }
    } else if (g_fabDown) {
        if (action == 2) { float dx = x - g_fabDownAtX, dy = y - g_fabDownAtY; if (fabsf(dx) > 20.0f || fabsf(dy) > 20.0f) g_fabMoved = true; if (g_fabMoved) { g_fabX = g_fabDragStartX + dx; g_fabY = g_fabDragStartY + dy; if (g_fabX < 0) g_fabX = 0; if (g_fabY < 0) g_fabY = 0; if (g_fabX > io.DisplaySize.x - g_fabSize) g_fabX = io.DisplaySize.x - g_fabSize; if (g_fabY > io.DisplaySize.y - g_fabSize) g_fabY = io.DisplaySize.y - g_fabSize; } return JNI_TRUE; }
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
static void doAddKeyEvent(JNIEnv*, jclass, jint keycode, jboolean down) { if (!gInitialized) return; ImGuiKey k = ImGuiKey_None; switch (keycode) { case 67: k = ImGuiKey_Backspace; break; case 66: k = ImGuiKey_Enter; break; case 65: k = ImGuiKey_Tab; break; default: return; } ImGui::GetIO().AddKeyEvent(k, down ? true : false); }

static void doSetClassList(JNIEnv* env, jclass, jobjectArray arr) {
    std::lock_guard<std::mutex> lk(gClassListMutex); gClassList.clear(); if (!arr) return;
    jsize n = env->GetArrayLength(arr); gClassList.reserve((size_t)n);
    for (jsize i = 0; i < n; i++) { jstring js = (jstring)env->GetObjectArrayElement(arr, i); const char* cs = env->GetStringUTFChars(js, nullptr); if (cs) { gClassList.emplace_back(cs); env->ReleaseStringUTFChars(js, cs); } env->DeleteLocalRef(js); }
}
static void doSetClassDetails(JNIEnv* env, jclass, jint sectionId, jstring className, jobjectArray fields, jobjectArray methods) {
    DetailResult r; r.sectionId = (int)sectionId;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (fields) { jsize n = env->GetArrayLength(fields); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(fields, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.fields.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    if (methods) { jsize n = env->GetArrayLength(methods); for (jsize i = 0; i < n; i++) { jstring s = (jstring)env->GetObjectArrayElement(methods, i); const char* c = env->GetStringUTFChars(s, nullptr); if (c) r.methods.emplace_back(c); if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s); } }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gDetailResults.push_back(std::move(r));
}
static void doSetMethodResult(JNIEnv* env, jclass, jint sectionId, jstring className, jstring methodSig, jstring result) {
    MethodResult r; r.sectionId = (int)sectionId;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (methodSig) { const char* c = env->GetStringUTFChars(methodSig, nullptr); r.methodSig = c ? c : ""; if (c) env->ReleaseStringUTFChars(methodSig, c); }
    if (result) { const char* c = env->GetStringUTFChars(result, nullptr); r.result = c ? c : ""; if (c) env->ReleaseStringUTFChars(result, c); }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gMethodResults.push_back(std::move(r));
}
static void doSetFieldValue(JNIEnv* env, jclass, jint sectionId, jstring className, jstring fieldName, jstring value, jboolean isSet) {
    FieldResult r; r.sectionId = (int)sectionId; r.isSet = isSet;
    if (className) { const char* c = env->GetStringUTFChars(className, nullptr); r.className = c ? c : ""; if (c) env->ReleaseStringUTFChars(className, c); }
    if (fieldName) { const char* c = env->GetStringUTFChars(fieldName, nullptr); r.fieldName = c ? c : ""; if (c) env->ReleaseStringUTFChars(fieldName, c); }
    if (value) { const char* c = env->GetStringUTFChars(value, nullptr); r.value = c ? c : ""; if (c) env->ReleaseStringUTFChars(value, c); }
    std::lock_guard<std::mutex> lk(gQueuesMutex); gFieldResults.push_back(std::move(r));
}
static void doSetPatches(JNIEnv* env, jclass, jobjectArray arr) {
    std::lock_guard<std::mutex> lk(gPatchesMutex);
    gPatches.clear(); if (!arr) return;
    jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(arr, i); const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) { std::vector<std::string> parts; splitStr(c, '|', parts);
            if (parts.size() == 7) { PatchItem p; p.id = std::stoi(parts[0]); p.type = parts[1]; p.className = parts[2]; p.targetName = parts[3]; p.originalValue = parts[4]; snprintf(p.currentValue, sizeof(p.currentValue), "%s", parts[5].c_str()); p.isActive = (parts[6] == "true"); gPatches.push_back(p); }
        }
        if (c) env->ReleaseStringUTFChars(s, c); env->DeleteLocalRef(s);
    }
}

static void setup_log_path(JNIEnv* env) {
    std::string ext;
    jclass at = env->FindClass("android/app/ActivityThread");
    if (at) {
        jmethodID m = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
        if (m) {
            jobject app = env->CallStaticObjectMethod(at, m);
            if (app) {
                jclass ctx = env->FindClass("android/content/Context");
                jmethodID gefd = env->GetMethodID(ctx, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
                if (gefd) {
                    jobject f = env->CallObjectMethod(app, gefd, nullptr);
                    if (f) {
                        jclass fc = env->FindClass("java/io/File");
                        jmethodID gap = env->GetMethodID(fc, "getAbsolutePath", "()Ljava/lang/String;");
                        jstring jp = (jstring)env->CallObjectMethod(f, gap);
                        if (jp) {
                            const char* c = env->GetStringUTFChars(jp, nullptr);
                            ext = c;
                            env->ReleaseStringUTFChars(jp, c);
                        }
                    }
                }
            }
        }
    }
    if (!ext.empty()) {
        snprintf(g_log_path, sizeof(g_log_path), "%s/DexTool.log", ext.c_str());
        const char* downloads[] = {
            "/sdcard/Download/DexTool.log",
            "/storage/emulated/0/Download/DexTool.log",
        };
        for (auto d : downloads) {
            int fd = open(d, O_WRONLY | O_APPEND | O_CREAT, 0666);
            if (fd >= 0) {
                close(fd);
                snprintf(g_log_path, sizeof(g_log_path), "%s", d);
                break;
            }
        }
    } else {
        snprintf(g_log_path, sizeof(g_log_path),
            "/sdcard/Android/data/com.example.tool/files/DexTool.log");
        mkdir("/sdcard/Android/data/com.example.tool", 0777);
        mkdir("/sdcard/Android/data/com.example.tool/files", 0777);
    }
}

static void setup_log_path(JNIEnv* env) {
    std::string ext;
    jclass at = env->FindClass("android/app/ActivityThread");
    if (at) {
        jmethodID m = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
        if (m) {
            jobject app = env->CallStaticObjectMethod(at, m);
            if (app) {
                jclass ctx = env->FindClass("android/content/Context");
                jmethodID gefd = env->GetMethodID(ctx, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
                if (gefd) {
                    jobject f = env->CallObjectMethod(app, gefd, nullptr);
                    if (f) {
                        jclass fc = env->FindClass("java/io/File");
                        jmethodID gap = env->GetMethodID(fc, "getAbsolutePath", "()Ljava/lang/String;");
                        jstring jp = (jstring)env->CallObjectMethod(f, gap);
                        if (jp) {
                            const char* c = env->GetStringUTFChars(jp, nullptr);
                            ext = c;
                            env->ReleaseStringUTFChars(jp, c);
                        }
                    }
                }
            }
        }
    }
    if (!ext.empty()) {
        snprintf(g_log_path, sizeof(g_log_path), "%s/DexTool.log", ext.c_str());
        const char* downloads[] = {
            "/sdcard/Download/DexTool.log",
            "/storage/emulated/0/Download/DexTool.log",
        };
        for (auto d : downloads) {
            int fd = open(d, O_WRONLY | O_APPEND | O_CREAT, 0666);
            if (fd >= 0) {
                close(fd);
                snprintf(g_log_path, sizeof(g_log_path), "%s", d);
                break;
            }
        }
    } else {
        snprintf(g_log_path, sizeof(g_log_path),
            "/sdcard/Android/data/com.example.tool/files/DexTool.log");
        mkdir("/sdcard/Android/data/com.example.tool", 0777);
        mkdir("/sdcard/Android/data/com.example.tool/files", 0777);
    }
}

void DrawMenu() {
    static int once = 0;
    if (!once) { flog("[menu] DrawMenu first call"); once = 1; }
    if (!g_menuVisible) return;
    ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DexTool", &g_menuVisible)) {
        ImGui::Text("DexTool via eglSwapBuffers + libinput hooks");
        ImGui::Separator();
        static char buf[256] = "";
        ImGui::InputText("Test input", buf, sizeof(buf));
        if (ImGui::Button("Close")) g_menuVisible = false;
    }
    ImGui::End();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    gVM = vm; g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    setup_log_path(env);
    flog("========== JNI_OnLoad start ==========");
    flog("Log path: %s", g_log_path);

    install_crash_handler();
    flog("[1] crash handler installed");

    loadDex(env);
    flog("[2] loadDex done");

    gum_init_embedded();
    flog("[3] gum_init_embedded done");

    init_frida_script();
    flog("[4] init_frida_script done");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_t t;
    pthread_create(&t, &attr, initModMenu, (void*)DrawMenu);
    pthread_attr_destroy(&attr);
    flog("[5] initModMenu thread started");

    hookInput();
    flog("[6] hookInput done");

    flog("========== JNI_OnLoad done ==========");
    return JNI_VERSION_1_6;
}
