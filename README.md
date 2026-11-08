# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r11)

1. Touch offset ("must tap above the item"). The overlay now tracks
   every EGL surface of the process (not just the owner's), so the
   native fallback map (largest surface ~= full window) is available
   from the first frame on any thread. The agent measures the real
   view rect on Activity.onResume and throttled on every touch
   (onWindowFocusChanged never fired on the field host). On top of the
   map there is now a manual "Touch Y offset" slider in Settings for
   hosts where neither source can be derived; it persists with the
   rest of the UI state.

2. Keyboard typed "[object Object]" into every field. Root cause: the
   bridge polled a hidden EditText with String(getText()) - that is an
   android.text.Editable, not the text, and its String() form is
   "[object Object]", which is exactly what got typed. The whole IME
   bridge (hidden EditText, InputMethodManager, text polling, the
   show/hide message round-trip) is gone. The r11 keyboard is drawn
   natively by libtool.so (qwerty + symbols page, shift, space, bksp):
   a tap inside it is routed to the active ImGui field and never
   reaches the app underneath.

3. Classes tab did not display all classes. The agent sent ONE message
   capped at 800 names; the field run has thousands of classes. The
   agent now streams the FULL result in pages of 400 with a query id,
   and the menu appends pages as they arrive (the list itself is
   clipperized, so tens of thousands of rows stay smooth).

4. Recorder crashed the app (SIGABRT, "glUseProgram -> GL_INVALID_VALUE
   (0x501)"). Two independent causes, both fixed:
   a) r10 hooked EVERY method of EVERY loaded class in the process -
      tens of thousands of ArtMethod replacements on a 32-bit process.
      The install loop never finished (the log shows endless
      "recorderProgress" until death). The recorder is now scoped: the
      Start button passes a package filter (default: the app's own
      package) and a method cap; installation stops when the cap is
      reached.
   b) The GL abort class: eglTerminate was not covered, so after the
      display was torn down the next eglCreateContext could land at a
      recycled address whose stale slot drew with handles that are
      invalid in the new context. eglTerminate is now hooked and
      retires every slot of that display; glUseProgram(0) is never
      restored (some ES wrappers raise GL_INVALID_VALUE for it); any
      error left after a draw is drained and logged instead of being
      left sticky for the app's next check.

Also: UI state (theme, opacity, font size, scroll speeds, Y offset,
keyboard on/off, FAB) persists to <logdir>/dextool-ui.conf.

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
