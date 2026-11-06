# Dex-tool v1.1

Standalone runtime instrumentation framework for Android APKs.
Ships as one native shared library: libtool.so (armeabi-v7a + arm64-v8a).
No root. No frida-server. Android 8.0+ (API 26).

## Fixes in v1.1

- Touch input offset (2 cm above button): use `getRawX()/getRawY()`
  (screen coords) instead of `getX()/getY()` (view-relative coords).
  These now map 1:1 to the EGL surface ImGui renders to.

- Soft keyboard / text boxes: native input queue + hidden `EditText`
  created on demand as IME target. `InputConnection.commitText` and
  `deleteSurroundingText` are hooked and forwarded to ImGui.
  `Activity.dispatchKeyEvent` handles hardware keys (backspace,
  enter, arrows, etc.).

- Classes tab now shows methods/fields: RPC responses (`classInfo`,
  `instanceList`, `trackerEvent`) are parsed by a small JSON
  tokenizer in `menu.cpp` and populate the UI.

- Font-size slider works: sets `ImGui::GetIO().FontGlobalScale`
  (`s_font_sz / 14.0f`) every time it changes.

- Menu doesn't appear in some apps: hooked `eglSwapBuffers`,
  `eglSwapBuffersWithDamageKHR`, and `eglSwapBuffersWithDamageEXT`.
  Modern HWUI apps call the KHR variant, not the base one — that
  was the previous miss. Vulkan-only apps are still unsupported.

## Usage

1. Take any target APK.
2. Drop libtool.so into lib/arm64-v8a/ and lib/armeabi-v7a/.
3. Inject `System.loadLibrary("tool")` into the launcher Activity
   smali or Application subclass onCreate().
4. Copy smali-fat.jar into assets/.
5. Re-sign and install.

The overlay shows only if the app calls one of the EGL swap
functions (GLSurfaceView apps and every HWUI app do). Vulkan-only
apps will not show the overlay; the Frida agent still runs.
