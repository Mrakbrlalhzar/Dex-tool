#pragma once
#include <android/input.h>
#include <cstdint>
#include "Dobby/include/dobby.h"
using Input_orig = void (*)(void*, void*, void*);
static Input_orig o_input = nullptr;
static void input_hook(void* thiz, void* ex_ab, void* ex_ac) {
    o_input(thiz, ex_ab, ex_ac);
    ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}
using Consume_orig = int32_t (*)(void*, void*, bool, long, uint32_t*, AInputEvent**);
static Consume_orig o_consume = nullptr;
static int32_t consume_hook(void* thiz, void* factory, bool raw, long seq, uint32_t* flags, AInputEvent** event) {
    int32_t ret = o_consume(thiz, factory, raw, seq, flags, event);
    if (ret == 0 && event && *event) ImGui_ImplAndroid_HandleInputEvent(*event);
    return ret;
}
static void hookInput() {
    void* sym = DobbySymbolResolver("libinput.so", "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE");
    if (sym) { DobbyHook(sym, (void*)input_hook, (void**)&o_input); LOGI("input hooked (legacy)"); return; }
    sym = DobbySymbolResolver("libinput.so", "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE");
    if (sym) { DobbyHook(sym, (void*)consume_hook, (void**)&o_consume); LOGI("consume hooked (Android 15+)"); }
}
