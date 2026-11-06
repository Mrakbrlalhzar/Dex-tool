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
            float x = AMotionEvent_getX(*ev, 0);
            float y = AMotionEvent_getY(*ev, 0);
            bool p  = (act == AMOTION_EVENT_ACTION_DOWN ||
                       act == AMOTION_EVENT_ACTION_MOVE);
            overlay_on_touch(x, y, p);
        }
    }
    return rc;
}

void touch_install_hooks() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) { LOGD("touch: dlopen libandroid.so failed (OK for Java apps)"); return; }
    void* sym = dlsym(lib, "AInputQueue_getEvent");
    if (!sym) { LOGD("touch: AInputQueue_getEvent not found (OK for Java apps)"); return; }
    int r = DobbyHook(sym,
                      reinterpret_cast<dobby_dummy_func_t>(my_getEvent),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_orig_get));
    if (r != 0) LOGD("touch: DobbyHook failed: %d (OK for Java apps)", r);
    else        LOGI("touch: AInputQueue_getEvent hooked (NativeActivity)");
}
