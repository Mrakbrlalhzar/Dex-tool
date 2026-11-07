# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes

    jni.h typedef clash: renamed helper functions jfloat->parse_float
    and jint->parse_int in frida_bridge.cpp to avoid redefinition errors.

    Root cause of SIGSEGV: NativeFunction with float params crashes libffi
    on Android ARM inside frida-gum 16.x. All NativeFunction calls removed.
    Touch and keyboard now route via JSON messages through on_message().

## Architecture

    Touch:    Activity.dispatchTouchEvent -> send({type:'touch',x,y,action})
    -> C++ on_message -> overlay_on_touch()
    Keyboard: EditText TextWatcher -> send({type:'char',cp}) / {type:'backspace'}
    -> C++ on_message -> overlay_on_char() / overlay_on_backspace()
    IME show: C++ frida_post({type:'showKeyboard'}) -> JS recv -> gIMM.showSoftInput()

## Components

    frida-gum 16.7.19   QuickJS / GumJS scripting runtime
    Dobby (LSPosed)     inline hook framework
    ImGui 1.90.4        immediate-mode overlay GUI (ES2 mode, #version 100)
    smali 3.0.7         runtime .smali to DEX assembler (bundled JAR)
