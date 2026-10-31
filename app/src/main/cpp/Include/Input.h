#pragma once
#include <android/input.h>
#include <cstdint>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "Dobby/include/dobby.h"
#include "Log.h"

using Input_orig = void (*)(void*, void*, void*);
static Input_orig o_input = nullptr;

static void input_hook(void* thiz, void* ex_ab, void* ex_ac) {
    if (o_input) o_input(thiz, ex_ab, ex_ac);
    if (!ImGui::GetCurrentContext()) return;
    if (ex_ab) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)ex_ab);
}

using Consume_orig = int32_t (*)(void*, void*, bool, long, uint32_t*, AInputEvent**);
static Consume_orig o_consume = nullptr;

static int32_t consume_hook(void* thiz, void* factory, bool raw, long seq, uint32_t* flags, AInputEvent** event) {
    int32_t ret = o_consume ? o_consume(thiz, factory, raw, seq, flags, event) : -1;
    if (ret == 0 && event && *event && ImGui::GetCurrentContext()) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return ret;
}

static void hookInput() {
    flog("[input] hooking input (legacy path)");
    void* sym = DobbySymbolResolver("libinput.so",
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE");
    flog("[input] legacy symbol=%p", sym);
    if (sym) {
        int rc = DobbyHook(sym, (void*)input_hook, (void**)&o_input);
        flog("[input] legacy DobbyHook rc=%d", rc);
        if (rc == 0) { flog("[input] legacy hook OK"); return; }
    }
    flog("[input] trying consume (Android 15+)");
    sym = DobbySymbolResolver("libinput.so",
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE");
    flog("[input] consume symbol=%p", sym);
    if (sym) {
        int rc = DobbyHook(sym, (void*)consume_hook, (void**)&o_consume);
        flog("[input] consume DobbyHook rc=%d", rc);
        if (rc == 0) { flog("[input] consume hook OK"); return; }
    }
    flog("[input] WARN: no input hook installed");
}
