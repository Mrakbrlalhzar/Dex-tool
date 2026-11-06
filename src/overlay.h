#pragma once
#include <EGL/egl.h>
#include <jni.h>
bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_on_touch(float x, float y, bool pressed);

#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void dextool_on_touch(float x, float y, int action);
JNIEXPORT void dextool_on_key(int keycode, int action);
JNIEXPORT void dextool_on_text(const char* utf8);
JNIEXPORT void dextool_on_backspace(void);
#ifdef __cplusplus
}
#endif
