#pragma once
#include <EGL/egl.h>
#include <jni.h>
#include <string>
#include <vector>

bool install_swap_hook();
void overlay_set_java_vm(JavaVM* vm);
void overlay_start(JavaVM* vm);

/* owner has a live ImGui world and at least one frame has been drawn */
bool overlay_ready();
/* last known DisplaySize of the owner surface (0,0 if none) */
void overlay_get_display_size(float* w, float* h);
/* rebuild the font atlas at `size` px on the next owner frame */
void overlay_request_font_rebuild(float size);

/* menu.cpp publishes the scrollable rects of the current frame (owner
   thread only); overlay converts drags inside them into wheel events at
   the given speed, which is what makes the huge lists touch-scrollable */
void overlay_publish_scroll_regions(const std::vector<float>& xywh_speed);

/* ---- r12 ---- */
/* manual Y nudge applied after the touch map (Settings slider). With
   the Y rescale gone (map_touch) this bias is what corrects the
   residual status-bar offset on hosts where the view rect cannot
   be measured. */
void overlay_set_touch_bias(float dy);
/* one-line description of the active touch map for the Settings status */
void overlay_touch_map_desc(char* buf, int n);

#ifdef __cplusplus
extern "C" {
#endif
/* JNI surface used by the JS agent (and by the AInputQueue fallback) */
JNIEXPORT void dextool_on_touch(float x, float y, int action);      /* raw window coords */
JNIEXPORT void dextool_set_touch_map(int x, int y, int w, int h);   /* overlay view rect in window */
JNIEXPORT void dextool_add_char(int codepoint);
JNIEXPORT void dextool_backspace();
JNIEXPORT bool dextool_want_mouse();
JNIEXPORT int  dextool_display_size();  /* (w<<16)|h, 0 until the overlay is up */
#ifdef __cplusplus
}
#endif
