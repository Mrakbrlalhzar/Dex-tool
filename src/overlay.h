#pragma once
#include <EGL/egl.h>
#include <jni.h>
bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_on_touch(float x, float y, bool pressed);
void overlay_on_char(int codepoint);
void overlay_on_backspace();
