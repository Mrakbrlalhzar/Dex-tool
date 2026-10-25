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

#include <frida-gumjs.h>

#define MY_PI 3.14159265358979323846f
#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Minimal placeholder - the real UI logic goes here (unchanged from prior)
// This file just demonstrates Frida init. User should merge with full UI code.

const gchar* PATCH_AGENT_JS = R"(
  Java.perform(function() {
    console.log("[*] Patch Agent Loaded from native-lib");
    send({type: "ready"});
  });
)";

static GumScript* g_script = nullptr;

static void on_script_message(GumScript* s, const gchar* msg, GBytes* data, gpointer ud) {
    LOGI("JS message: %s", msg);
}

static void load_patch_agent() {
    GError* error = nullptr;
    GumScriptBackend* backend = gum_script_backend_obtain_qjs();
    g_script = gum_script_backend_create(backend, "patch_agent", PATCH_AGENT_JS, nullptr, nullptr, &error);
    if (error) {
        LOGI("Frida script create error: %s", error->message);
        g_error_free(error);
        return;
    }
    gum_script_set_message_handler(g_script, on_script_message, nullptr, nullptr);
    gum_script_load(g_script, nullptr);
    LOGI("Frida Patch Agent loaded");
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;

    gum_init_embedded();
    LOGI("Frida Gum initialized");
    load_patch_agent();

    return JNI_VERSION_1_6;
}
