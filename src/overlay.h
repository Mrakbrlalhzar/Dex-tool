#pragma once
#include <EGL/egl.h>
#include <jni.h>
#include <string>
#include <vector>
bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_start(JavaVM* vm);
bool overlay_ready();
void overlay_get_display_size(float* w, float* h);
void overlay_request_font_rebuild(float size);
void overlay_publish_scroll_regions(const std::vector<float>& xywh_speed);
void overlay_set_touch_bias(float dy);
void overlay_touch_map_desc(char* buf, int n);
#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void dextool_on_touch(float x, float y, int action);      /* raw screen coords (getRawX/getRawY) */
JNIEXPORT void dextool_set_touch_map(int x, int y, int w, int h);   /* overlay view rect ON SCREEN (getLocationOnScreen) */
JNIEXPORT void dextool_add_char(int codepoint);
JNIEXPORT void dextool_backspace();
JNIEXPORT bool dextool_want_mouse();
JNIEXPORT bool dextool_hit_test(float x, float y);
JNIEXPORT int  dextool_display_size();  /* (w<<16)|h, 0 until the overlay is up */
JNIEXPORT int  dextool_want_frames();
JNIEXPORT void dextool_agent_log(const char* msg); /* r14: agent phase markers -> logs */
#ifdef __cplusplus
}
#endif
