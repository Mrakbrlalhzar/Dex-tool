#include "touch.h"
#include "overlay.h"
#include "logger.h"
#include "dobby.h"
#include <dlfcn.h>
#include <android/input.h>

typedef int32_t (*fn_getEvent)(AInputQueue*, AInputEvent**);
static fn_getEvent g_orig_get = nullptr;

/* Both input paths (this AInputQueue hook for NativeActivity hosts and
   the Java dispatchTouchEvent hook in agent.js) deliver raw event
   pixels. Since r15 the Java side sends SCREEN coords (getRawX/
   getRawY) and the map is measured on screen too, so that path is
   exact on both axes. r7/r8 fed them straight into ImGui's
   DisplaySize, which is the *surface* rect of the GL view - on the
   sample app that surface (540x832) sits inside a taller window
   (540x960), so every tap landed above its target. r9 maps the raw
   coords through the rect the agent measured (dextool_set_touch_map)
   before they reach ImGui; this hook therefore forwards untouched
   coords and lets overlay.cpp do the mapping. (AInputQueue coords
   are window-local - this fallback may need the Settings Y bias.)

   r23 click-through: the JAVA path consumes menu taps before the host
   app sees them (dextool_hit_test in the dispatchTouchEvent hook).
   THIS path only READS events - the app's AInputQueue poll loop owns
   dispatch, so consuming here is not possible without rewriting the
   app's input loop; NativeActivity hosts still receive taps that land
   on the menu (documented limitation, unchanged from r22). */
static int32_t my_getEvent(AInputQueue* q, AInputEvent** ev) {
    int32_t rc = g_orig_get(q, ev);
    if (rc >= 0 && ev && *ev) {
        if (AInputEvent_getType(*ev) == AINPUT_EVENT_TYPE_MOTION) {
            int32_t act = AMotionEvent_getAction(*ev) & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(*ev, 0);
            float y = AMotionEvent_getY(*ev, 0);
            int a = 2;                       /* move */
            if      (act == 0)           a = 0;  /* DOWN */
            else if (act == 1 || act == 3) a = 1; /* UP / CANCEL */
            else if (act == 5 || act == 6) a = 2; /* pointer up/down */
            dextool_on_touch(x, y, a);
        }
    }
    return rc;
}

void touch_install_hooks() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) { LOGD("touch: libandroid not found (OK for Java apps)"); return; }
    void* sym = dlsym(lib, "AInputQueue_getEvent");
    if (!sym) { LOGD("touch: AInputQueue_getEvent not found (OK for Java apps)"); return; }
    int r = DobbyHook(sym,
        reinterpret_cast<dobby_dummy_func_t>(my_getEvent),
        reinterpret_cast<dobby_dummy_func_t*>(&g_orig_get));
    if (r == 0) LOGI("touch: AInputQueue_getEvent hooked (NativeActivity)");
    else        LOGD("touch: DobbyHook AInputQueue_getEvent: %d", r);
}
