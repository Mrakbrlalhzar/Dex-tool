#include <jni.h>
#include <android/log.h>

#define LOG_TAG "ImGuiTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT void JNICALL
Java_com_example_tool_MainActivity_init(JNIEnv* env, jobject /* this */) {
    LOGI("ImGuiTool native init called");
    // TODO: create SurfaceView, set up ImGui, attach to WindowManager
}
