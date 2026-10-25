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

// ... (Global variables, structs, etc. - keep as before) ...

// ========== FRIDA SCRIPT AGENT ==========
const gchar* PATCH_AGENT_JS = R"(
  Java.perform(function() {
    console.log("[*] Patch Agent Loaded");
    rpc.exports = {
      patchMethod: function(className, methodName, returnType, value) {
        try {
          var targetClass = Java.use(className);
          var overloads = targetClass[methodName].overloads;
          if (overloads.length === 0) return "Error: Method not found";
          var original = overloads[0];
          overloads[0].implementation = function() {
            console.log("[*] Hooked " + className + "." + methodName + " called.");
            if (returnType === "int") return parseInt(value);
            if (returnType === "boolean") return value === "true";
            if (returnType === "java.lang.String") return value;
            return null;
          };
          return "Success: Hooked " + className + "." + methodName;
        } catch (e) {
          return "Error: " + e.message;
        }
      }
    };
    send({type: "ready"});
  });
)";

static GumScript* g_script = nullptr;

static void on_script_message(GumScript* script, const gchar* message, GBytes* data, gpointer user_data) {
    LOGI("Message from JS: %s", message);
}

static void load_patch_agent() {
    GError* error = nullptr;
    GumScriptBackend* backend = gum_script_backend_obtain_qjs();
    g_script = gum_script_backend_create(backend, "patch_agent", PATCH_AGENT_JS, nullptr, nullptr, &error);
    if (error) {
        LOGI("Error creating script: %s", error->message);
        g_error_free(error);
        return;
    }
    gum_script_set_message_handler(g_script, on_script_message, nullptr, nullptr);
    gum_script_load(g_script, nullptr);
    LOGI("Patch Agent script loaded.");
}

// ... (rest of UI code) ...

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    // gVM = vm; if (!loadDex(env)) return JNI_VERSION_1_6;

    gum_init_embedded();
    load_patch_agent();

    // ... (RegisterNatives, etc.) ...
    return JNI_VERSION_1_6;
}
