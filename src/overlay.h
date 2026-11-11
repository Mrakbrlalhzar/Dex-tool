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

/* ---- r15 ---- */
/* manual Y nudge applied after the touch map (Settings slider).
   Default 0: touches (getRawX/getRawY) and the view rect
   (getLocationOnScreen) now share ONE coordinate space and both
   axes are scaled proportionally, so no static offset is needed.
   The slider only remains for exotic hosts where the view rect
   cannot be measured. */
void overlay_set_touch_bias(float dy);
/* one-line description of the active touch map for the Settings status */
void overlay_touch_map_desc(char* buf, int n);

#ifdef __cplusplus
extern "C" {
#endif
/* JNI surface used by the JS agent (and by the AInputQueue fallback) */
JNIEXPORT void dextool_on_touch(float x, float y, int action);      /* raw screen coords (getRawX/getRawY) */
JNIEXPORT void dextool_set_touch_map(int x, int y, int w, int h);   /* overlay view rect ON SCREEN (getLocationOnScreen) */
JNIEXPORT void dextool_add_char(int codepoint);
JNIEXPORT void dextool_backspace();
JNIEXPORT bool dextool_want_mouse();
/* r23: click-through filter. Called by the Java touch hook BEFORE the
   gesture reaches the host app; true = the tap mapped onto one of the
   live ImGui windows of the last frame and must be consumed. */
JNIEXPORT bool dextool_hit_test(float x, float y);
JNIEXPORT int  dextool_display_size();  /* (w<<16)|h, 0 until the overlay is up */
/* r25: frame-pump level the agent should drive with DecorView
   postInvalidate(): 0 = overlay down (never invalidate), 1 = menu
   hidden but the FAB is up (slow pump keeps it tappable), 2 = menu
   visible (full-rate pump). Render-on-demand hosts never swap
   buffers on their own, so without this pump the queued menu
   touches were only processed when the app happened to redraw -
   the field's "menu hangs/freezes, but touching the app makes the
   earlier menu touch work" report. */
JNIEXPORT int  dextool_want_frames();
JNIEXPORT void dextool_agent_log(const char* msg); /* r14: agent phase markers -> logs */
#ifdef __cplusplus
}
#endif
