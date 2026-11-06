# Dex-tool

Standalone runtime instrumentation framework for Android APKs.
Ships as one native shared library: libtool.so (armeabi-v7a + arm64-v8a).
No root. No frida-server. Android 8.0+ (API 26).

## Bug fixes in this version

    FIX 1 - Touch input (Java apps):
      AInputQueue_getEvent only fires for NativeActivity apps.
      Java Activity apps get touch via GumJS hooking Activity.dispatchTouchEvent,
      which calls dextool_on_touch() (JNIEXPORT symbol in libtool.so) via
      NativeFunction. The NativeActivity hook remains as a fallback.

    FIX 2 - Menu disappears on pause/resume:
      eglGetCurrentContext() is checked on every swap. When the EGL context
      handle changes (new context after GLSurfaceView recreates on resume),
      ImGui is shut down cleanly and re-initialized with the new context.

    FIX 3 - Menu not shown in ES2 apps:
      Compiled with IMGUI_IMPL_OPENGL_ES2=1. ImGui uses GLES2 function set
      and #version 100 GLSL, which works on ES2 and ES3. The ES3-only guard
      is replaced with an OpenGL ES (any version) check.

## Usage

    1. Take any target APK.
    2. Drop libtool.so into lib/arm64-v8a/ and lib/armeabi-v7a/.
    3. Inject System.loadLibrary("tool") into the launcher Activity smali
       or Application subclass onCreate().
    4. Copy smali-fat.jar into assets/.
    5. Re-sign and install.
    Note: overlay only appears if the app calls eglSwapBuffers (OpenGL ES).
    Vulkan-only apps will not show the overlay but the Frida agent still runs.

## Architecture

    libtool.so
      JNI_OnLoad
        write boot marker (raw I/O, no gum)
        logger_init()
        install_swap_hook() via Dobby (LSPosed/Dobby)
      eglSwapBuffers replacement (GL thread)
        First call:
          gum_init_embedded()      <- FIRST gum call, valid EGL context
          spawn frida pthread      <- GumJS / QuickJS agent
          touch_install_hooks()    <- NativeActivity fallback
          imgui_init()             <- ES2 or ES3, any OpenGL ES
        On context change (pause/resume):
          imgui_shutdown() + imgui_init()
        Every frame:
          inject touch (from Java hook via dextool_on_touch)
          ImGui render
          g_orig_swap()           <- never recurses

## Components

    frida-gum 16.7.19   QuickJS / GumJS scripting runtime
    Dobby (LSPosed)     inline function hook framework
    ImGui 1.90.4        immediate-mode overlay GUI (ES2 mode)
    smali 3.0.7         runtime .smali to DEX assembler (bundled JAR)
