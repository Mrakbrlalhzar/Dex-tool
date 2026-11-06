#pragma once
#include <EGL/egl.h>
#include <jni.h>
bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_on_touch(float x, float y, bool pressed);

// Exported C symbol called by GumJS agent via NativeFunction.
// JNIEXPORT ensures visibility despite -fvisibility=hidden.
#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void dextool_on_touch(float x, float y, int action);
#ifdef __cplusplus
}
#endif
