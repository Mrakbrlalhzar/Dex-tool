# Dex-tool

Standalone runtime instrumentation framework for Android APKs.
Ships as one native shared library: libtool.so (armeabi-v7a + arm64-v8a).
No root. No frida-server. Android 8.0+ (API 26).

## Usage

    1. Take any target APK.
    2. Drop libtool.so into lib/arm64-v8a/ and lib/armeabi-v7a/.
    3. Inject System.loadLibrary("tool") into the launcher Activity smali
       or into an Application subclass onCreate().
    4. Copy smali-fat.jar into the APK assets/.
    5. Re-sign and install.

## Architecture

    libtool.so
      JNI_OnLoad
        write boot marker (raw I/O, no gum)
        logger_init()
        install_swap_hook() via Dobby
      eglSwapBuffers replacement (GL thread, first call only)
        gum_init_embedded()      <- FIRST gum call, valid EGL context
        spawn frida pthread      <- GumJS / QuickJS agent
        touch_install_hooks()
        imgui_init()             <- guarded: OpenGL ES 3 only
      per-frame
        inject touch into ImGui
        ImGui::NewFrame / menu_draw / Render
        g_orig_swap()            <- never recurses into eglSwapBuffers symbol

## Menu Tabs

    Settings  opacity, font size, theme
    Patches   patch Java static fields at runtime
    Hooks     hook methods: Java JS replacement, Smali recompile, noop
    Tracker   log method calls with args and return values
    Recorder  capture all Java method calls while active
    Monitor   network interception best-effort (SSLContext.init)
    Script    run arbitrary Frida JS in the embedded QuickJS runtime
    Classes   browse loaded classes, fields, methods, instances

## Diagnostic files in /sdcard/Download/

    dextool-boot.marker   DexToolApp.onCreate BEFORE loadLibrary
    dextool-jni.marker    JNI_OnLoad at 4 checkpoints
    dextool.log           INFO log
    dextool-debug.log     DEBUG log
    dextool-crash.log     signal handler + Java uncaught exception

## Components

    frida-gum 16.7.19   QuickJS / GumJS scripting runtime
    Dobby               inline function hook framework (LSPosed/Dobby)
    ImGui 1.90.4        immediate-mode overlay GUI
    smali 3.0.7         runtime .smali to DEX assembler (bundled JAR)

## Known Limitations

    Smali hook mode uses InMemoryDexClassLoader (API 26+ only).
    Re-loading the same class name requires a fresh ClassLoader instance.

    Network monitor is best-effort. It hooks Java SSLContext.init.
    Native BoringSSL interception is not included in this release.

    ImGui overlay requires OpenGL ES 3.0+. ES 2.x targets degrade
    gracefully (Frida agent still runs; only the overlay is off).

    gum_init_embedded() is deferred to the first eglSwapBuffers call
    so a valid EGL context exists when it fires, preventing early crashes.

## Build notes

    Dobby is sourced from LSPosed/Dobby. The CMake target is "dobby",
    and DOBBY_GENERATE_SHARED=OFF is required to produce libdobby.a.
    DobbyHook calls use reinterpret_cast<dobby_dummy_func_t> to match
    LSPosed/Dobby's strict function-pointer signature.
    frida-gum 16.7.19's GumScriptMessageHandler takes (const char*, GBytes*, void*)
    without the GumScript* parameter, and gum_script_load_sync takes 2 arguments.
    The sample app enables android.useAndroidX and android.enableJetifier
    in gradle.properties to satisfy its AppCompat dependency.
