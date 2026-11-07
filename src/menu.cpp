#include "menu.h"
#include "frida_bridge.h"
#include "logger.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <pthread.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
#define MLK()   pthread_mutex_lock(&g_mtx)
#define MULK()  pthread_mutex_unlock(&g_mtx)

static float s_font_scale = 1.0f;
static float s_opacity    = 0.92f;
static int   s_theme      = 0;
static bool  s_visible    = true;

struct PatchEntry  { std::string name, value; bool enabled = true; };
struct HookEntry   { std::string name, code;  bool enabled = true; int mode = 0; };
struct TrackerEntry{ std::string name; std::vector<std::string> log; };
struct ClassWin    { std::string cname; std::vector<std::string> methods, fields; };

static std::vector<PatchEntry>   s_patches;
static std::vector<HookEntry>    s_hooks;
static std::vector<TrackerEntry> s_trackers;
static std::vector<ClassWin>     s_classes;
static bool s_rec_on = false, s_mon_on = false;
static std::vector<std::string> s_rec_log, s_mon_log, s_script_out;

static char s_pname[256]={}, s_pval[512]={};
static char s_hname[256]={}, s_hcode[2048]={};
static int  s_hmode=0;
static char s_tname[256]={};
static char s_script[8192]="// Frida JS\n";
static char s_cname[256]={};

static void post_rpc(const char* fn, const char* args) {
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"rpc\",\"fn\":\"%s\",\"args\":%s}", fn, args);
    frida_post(std::string(buf));
}

static std::string jstr(const std::string& j, const char* key) {
    std::string k = std::string("\"")+key+"\":\"";
    auto p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    auto e = j.find('"', p);
    return (e!=std::string::npos) ? j.substr(p,e-p) : "";
}

static std::vector<std::string> jarr(const std::string& j, const char* key) {
    std::vector<std::string> r;
    std::string k = std::string("\"")+key+"\":[";
    auto p = j.find(k);
    if (p==std::string::npos) return r;
    p += k.size();
    auto e = j.find(']',p);
    if (e==std::string::npos) return r;
    std::string seg = j.substr(p,e-p);
    size_t i=0;
    while(i<seg.size()){
        auto s=seg.find('"',i); if(s==std::string::npos)break; s++;
        auto f=seg.find('"',s); if(f==std::string::npos)break;
        r.push_back(seg.substr(s,f-s)); i=f+1;
    }
    return r;
}

static void apply_theme() {
    switch(s_theme){
        case 0: ImGui::StyleColorsDark();    break;
        case 1: ImGui::StyleColorsLight();   break;
        case 2: ImGui::StyleColorsClassic(); break;
    }
    ImGui::GetStyle().Alpha        = s_opacity;
    ImGui::GetIO().FontGlobalScale = s_font_scale;
}

void menu_init() {
    apply_theme();
    frida_set_message_callback([](const std::string& j){
        MLK();
        if (j.find("\"classInfo\"")!=std::string::npos) {
            auto cls=jstr(j,"class");
            auto mths=jarr(j,"methods");
            auto flds=jarr(j,"fields");
            for(auto& cw:s_classes){
                if(cw.cname==cls){ cw.methods=mths; cw.fields=flds; break; }
            }
        } else if(j.find("\"trackerEvent\"")!=std::string::npos){
            auto name=jstr(j,"name");
            auto ret=jstr(j,"ret");
            for(auto& t:s_trackers){
                if(t.name==name){
                    t.log.push_back(ret.empty()?j:ret);
                    if(t.log.size()>100) t.log.erase(t.log.begin());
                    break;
                }
            }
        } else if(j.find("\"recorded\"")!=std::string::npos){
            if(s_rec_log.size()<500) s_rec_log.push_back(j.substr(0,180));
        } else if(j.find("\"sslInit\"")!=std::string::npos||
                  j.find("\"monitorInfo\"")!=std::string::npos||
                  j.find("\"networkConnect\"")!=std::string::npos){
            if(s_mon_log.size()<500) s_mon_log.push_back(j.substr(0,180));
        } else {
            if(s_script_out.size()<300) s_script_out.push_back(j.substr(0,200));
        }
        MULK();
    });
}

static void tab_settings(){
    if(ImGui::SliderFloat("Font Scale",&s_font_scale,.5f,3.f))
        ImGui::GetIO().FontGlobalScale=s_font_scale;
    if(ImGui::SliderFloat("Opacity",&s_opacity,.3f,1.f)) apply_theme();
    const char* th[]={"Dark","Light","Classic"};
    if(ImGui::Combo("Theme",&s_theme,th,3)) apply_theme();
    ImGui::Checkbox("Visible",&s_visible);
}

static void tab_patches(){
    ImGui::InputText("Name##pn",s_pname,sizeof(s_pname));
    ImGui::InputText("Value##pv",s_pval,sizeof(s_pval));
    if(ImGui::Button("Add Patch")&&s_pname[0]){
        char a[1024];
        snprintf(a,sizeof(a),"{\"name\":\"%s\",\"value\":\"%s\"}",s_pname,s_pval);
        post_rpc("addPatch",a);
        MLK(); s_patches.push_back({s_pname,s_pval,true}); MULK();
        s_pname[0]=s_pval[0]=0;
    }
    ImGui::Separator();
    MLK();
    for(int i=0;i<(int)s_patches.size();i++){
        ImGui::PushID(i); auto& p=s_patches[i];
        if(ImGui::Checkbox("##e",&p.enabled)){
            char a[256];
            snprintf(a,sizeof(a),"{\"name\":\"%s\",\"enabled\":%s}",
                     p.name.c_str(),p.enabled?"true":"false");
            post_rpc("setPatchEnabled",a);
        }
        ImGui::SameLine(); ImGui::Text("%s=%s",p.name.c_str(),p.value.c_str());
        ImGui::SameLine();
        if(ImGui::SmallButton("X")){
            char a[256]; snprintf(a,sizeof(a),"{\"name\":\"%s\"}",p.name.c_str());
            post_rpc("removePatch",a);
            s_patches.erase(s_patches.begin()+i);
            ImGui::PopID(); MULK(); return;
        }
        ImGui::PopID();
    }
    MULK();
}

static void tab_hooks(){
    const char* modes[]={"Java","Smali","noop"};
    ImGui::InputText("Method##hn",s_hname,sizeof(s_hname));
    ImGui::Combo("Mode",&s_hmode,modes,3);
    ImGui::InputTextMultiline("Code##hc",s_hcode,sizeof(s_hcode),ImVec2(-1,80));
    if(ImGui::Button("Add Hook")&&s_hname[0]){
        char a[2048];
        snprintf(a,sizeof(a),"{\"name\":\"%s\",\"mode\":\"%s\",\"code\":\"%s\"}",
                 s_hname,modes[s_hmode],s_hcode);
        post_rpc("addHook",a);
        MLK(); s_hooks.push_back({s_hname,s_hcode,true,s_hmode}); MULK();
        s_hname[0]=s_hcode[0]=0;
    }
    ImGui::Separator();
    MLK();
    for(int i=0;i<(int)s_hooks.size();i++){
        ImGui::PushID(i); auto& h=s_hooks[i];
        if(ImGui::Checkbox("##he",&h.enabled)){
            char a[256];
            snprintf(a,sizeof(a),"{\"name\":\"%s\",\"enabled\":%s}",
                     h.name.c_str(),h.enabled?"true":"false");
            post_rpc("setHookEnabled",a);
        }
        ImGui::SameLine(); ImGui::Text("[%s] %s",modes[h.mode],h.name.c_str());
        ImGui::SameLine();
        if(ImGui::SmallButton("X")){
            char a[256]; snprintf(a,sizeof(a),"{\"name\":\"%s\"}",h.name.c_str());
            post_rpc("removeHook",a);
            s_hooks.erase(s_hooks.begin()+i);
            ImGui::PopID(); MULK(); return;
        }
        ImGui::PopID();
    }
    MULK();
}

static void tab_tracker(){
    ImGui::InputText("Method##tn",s_tname,sizeof(s_tname));
    if(ImGui::Button("Track")&&s_tname[0]){
        char a[256]; snprintf(a,sizeof(a),"{\"name\":\"%s\"}",s_tname);
        post_rpc("addTracker",a);
        MLK(); s_trackers.push_back({s_tname,{}}); MULK();
        s_tname[0]=0;
    }
    ImGui::Separator();
    MLK();
    for(int i=0;i<(int)s_trackers.size();i++){
        ImGui::PushID(i); auto& t=s_trackers[i];
        if(ImGui::CollapsingHeader(t.name.c_str())){
            for(auto& l:t.log) ImGui::TextUnformatted(l.c_str());
            if(ImGui::SmallButton("Remove")){
                char a[256]; snprintf(a,sizeof(a),"{\"name\":\"%s\"}",t.name.c_str());
                post_rpc("removeTracker",a);
                s_trackers.erase(s_trackers.begin()+i);
                ImGui::PopID(); MULK(); return;
            }
        }
        ImGui::PopID();
    }
    MULK();
}

static void tab_recorder(){
    if(!s_rec_on){
        if(ImGui::Button("Start")){
            s_rec_on=true; MLK(); s_rec_log.clear(); MULK();
            post_rpc("startRecorder","{}");
        }
    } else {
        if(ImGui::Button("Stop")){ s_rec_on=false; post_rpc("stopRecorder","{}"); }
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1),"REC");
    }
    ImGui::Separator();
    ImGui::BeginChild("rl",ImVec2(0,180),true);
    MLK(); for(auto& l:s_rec_log) ImGui::TextUnformatted(l.c_str()); MULK();
    ImGui::SetScrollHereY(1.f); ImGui::EndChild();
}

static void tab_monitor(){
    ImGui::TextDisabled("(best-effort: Java SSLContext.init)");
    if(!s_mon_on){
        if(ImGui::Button("Start")){
            s_mon_on=true; MLK(); s_mon_log.clear(); MULK();
            post_rpc("startMonitor","{}");
        }
    } else {
        if(ImGui::Button("Stop")){ s_mon_on=false; post_rpc("stopMonitor","{}"); }
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1),"MON");
    }
    ImGui::Separator();
    ImGui::BeginChild("ml",ImVec2(0,180),true);
    MLK(); for(auto& l:s_mon_log) ImGui::TextUnformatted(l.c_str()); MULK();
    ImGui::SetScrollHereY(1.f); ImGui::EndChild();
}

static void tab_script(){
    ImGui::InputTextMultiline("##sc",s_script,sizeof(s_script),ImVec2(-1,140));
    if(ImGui::Button("Run")){
        MLK(); s_script_out.clear(); MULK();
        char a[8256]; snprintf(a,sizeof(a),"{\"src\":\"%s\"}",s_script);
        post_rpc("evalScript",a);
    }
    ImGui::SameLine();
    if(ImGui::Button("Clear")){ MLK(); s_script_out.clear(); MULK(); }
    ImGui::Separator();
    ImGui::BeginChild("so",ImVec2(0,120),true);
    MLK(); for(auto& l:s_script_out) ImGui::TextUnformatted(l.c_str()); MULK();
    ImGui::EndChild();
}

static void tab_classes(){
    ImGui::InputText("Class##cn",s_cname,sizeof(s_cname));
    ImGui::SameLine();
    if(ImGui::Button("Open")&&s_cname[0]){
        char a[256]; snprintf(a,sizeof(a),"{\"class\":\"%s\"}",s_cname);
        post_rpc("openClass",a);
        MLK(); s_classes.push_back({s_cname,{},{}}); MULK();
        s_cname[0]=0;
    }
    MLK();
    for(int i=0;i<(int)s_classes.size();i++){
        ImGui::PushID(i); auto& c=s_classes[i];
        if(ImGui::CollapsingHeader(c.cname.c_str())){
            if(ImGui::Button("List Instances")){
                char a[256]; snprintf(a,sizeof(a),"{\"class\":\"%s\"}",c.cname.c_str());
                post_rpc("listInstances",a);
            }
            ImGui::Text("Methods (%d):",(int)c.methods.size());
            for(auto& m:c.methods){
                ImGui::BulletText("%s",m.c_str()); ImGui::SameLine();
                std::string hid="H##h"+m, tid="T##t"+m;
                if(ImGui::SmallButton(hid.c_str())){
                    char a[256];
                    snprintf(a,sizeof(a),"{\"class\":\"%s\",\"method\":\"%s\"}",
                             c.cname.c_str(),m.c_str());
                    post_rpc("hookMethod",a);
                }
                ImGui::SameLine();
                if(ImGui::SmallButton(tid.c_str())){
                    char a[256];
                    snprintf(a,sizeof(a),"{\"class\":\"%s\",\"method\":\"%s\"}",
                             c.cname.c_str(),m.c_str());
                    post_rpc("trackMethod",a);
                }
            }
            ImGui::Text("Fields (%d):",(int)c.fields.size());
            for(auto& f:c.fields){
                ImGui::BulletText("%s",f.c_str()); ImGui::SameLine();
                std::string pid="P##p"+f;
                if(ImGui::SmallButton(pid.c_str())){
                    char a[256];
                    snprintf(a,sizeof(a),"{\"class\":\"%s\",\"field\":\"%s\"}",
                             c.cname.c_str(),f.c_str());
                    post_rpc("patchField",a);
                }
            }
        }
        ImGui::PopID();
    }
    MULK();
}

void menu_draw(){
    if(!s_visible) return;
    ImGuiIO& io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20,20),ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x*.75f,500),ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(s_opacity);
    ImGui::Begin("Dex-tool v1.0",&s_visible,ImGuiWindowFlags_NoCollapse);
    if(ImGui::BeginTabBar("tb")){
        if(ImGui::BeginTabItem("Settings")) { tab_settings(); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Patches"))  { tab_patches();  ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Hooks"))    { tab_hooks();    ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Tracker"))  { tab_tracker();  ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Recorder")) { tab_recorder(); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Monitor"))  { tab_monitor();  ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Script"))   { tab_script();   ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Classes"))  { tab_classes();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
