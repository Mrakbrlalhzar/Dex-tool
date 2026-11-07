#pragma once
#include <EGL/egl.h>
#include <jni.h>
bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_start(JavaVM* vm);
void overlay_on_touch(float x, float y, bool pressed);
#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void dextool_on_touch(float x, float y, int action);
JNIEXPORT void dextool_add_char(int codepoint);
JNIEXPORT void dextool_backspace();
JNIEXPORT bool dextool_want_mouse();
#ifdef __cplusplus
}
#endif
