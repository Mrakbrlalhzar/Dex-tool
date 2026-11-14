#include "touch.h"
#include "overlay.h"
#include "logger.h"
#include "dobby.h"
#include <dlfcn.h>
#include <android/input.h>
typedef int32_t (*fn_getEvent)(AInputQueue*, AInputEvent**);
static fn_getEvent g_orig_get = nullptr;
static int32_t my_getEvent(AInputQueue* q, AInputEvent** ev) {
    int32_t rc = g_orig_get(q, ev);
    if (rc >= 0 && ev && *ev) {
        if (AInputEvent_getType(*ev) == AINPUT_EVENT_TYPE_MOTION) {
            int32_t act = AMotionEvent_getAction(*ev) & AMOTION_EVENT_ACTION_MASK;
            /* r36: AInputQueue events carry view-LOCAL x/y, but the
               touch map (dextool_set_touch_map) and the Java-path
               hook both work in SCREEN coords, so a NativeActivity
               host's taps landed offset by the view's on-screen
               position. Use the RAW axes when the NDK defines them. */
            float x, y;
#if defined(AMOTION_EVENT_AXIS_RAWX)
            /* raw = screen coords - the same space the Java-path hook
               and the touch map use */
            x = AMotionEvent_getAxisValue(*ev, AMOTION_EVENT_AXIS_RAWX, 0);
            y = AMotionEvent_getAxisValue(*ev, AMOTION_EVENT_AXIS_RAWY, 0);
#else
            /* older NDK: view-local fallback (same as the old code,
               but documented as screen-space-inaccurate) */
            x = AMotionEvent_getX(*ev, 0);
            y = AMotionEvent_getY(*ev, 0);
#endif
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
