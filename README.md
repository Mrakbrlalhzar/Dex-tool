# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version

    1. Touch offset: Activity.dispatchTouchEvent coordinates are
       corrected by the content-view y-offset measured in
       onWindowFocusChanged (removes ActionBar shift).

    2. Keyboard: invisible EditText + TextWatcher bridge. When ImGui
       sets WantCaptureKeyboard, the agent posts showKeyboard to the
       C++ layer, which calls frida_post back to the GumJS agent,
       which shows the IME on the main thread. Characters arrive via
       TextWatcher -> dextool_add_char NativeFunction -> ImGui
       AddInputCharacter on the GL thread.

    3. Font size: FontGlobalScale is updated immediately when the
       slider changes value.

    4. Classes tab: JSON response from openClass is parsed in the
       message callback and stored in ClassWin.methods/fields under
       mutex. CollapsingHeader now shows method/field counts.

    5. Menu in injected APKs: eglSwapBuffersWithDamageKHR is now
       also hooked (many apps use this instead of eglSwapBuffers).
       glBindFramebuffer(0) is called before ImGui render to ensure
       we draw to the default framebuffer.

    6. Thread safety: menu data structures are protected with a
       pthread_mutex (message callback = GumJS thread, draw = GL thread).

## Usage

    1. Drop libtool.so into lib/arm64-v8a/ and lib/armeabi-v7a/.
    2. Inject System.loadLibrary("tool") into Application.onCreate()
       or the launcher Activity.
    3. Copy smali-fat.jar into assets/.
    4. Re-sign and install.

## Components

    frida-gum 16.7.19   QuickJS / GumJS scripting runtime
    Dobby (LSPosed)     inline function hook framework
    ImGui 1.90.4        immediate-mode overlay GUI (ES2 mode)
    smali 3.0.7         runtime .smali to DEX assembler (bundled JAR)
