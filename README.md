# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r9)

    1. Touch offset: input events arrive in raw window pixels; the
       agent now measures the drawing view's rect inside the window
       (dextool_set_touch_map) and the overlay maps every event through
       it before it reaches ImGui. Taps land where the finger is.
    2. Keyboard: ImGui's WantTextInput drives a hidden EditText, so
       tapping a text field opens the IME and typing reaches ImGui
       (chars + backspace). Tapping outside closes it again.
    3. App switch / relayout: when the owner GL context is destroyed
       the ImGui world is parked and adopted by the next context
       (8 swaps), so the menu survives coming back from the background
       and the soft-keyboard resize instead of the app closing.
    4. Recorder: hooks are installed in background chunks with a
       package skip-list, events go through a bounded ring flushed in
       500 ms batches; the list renders with ImGuiListClipper. Starting
       a recording no longer hangs the program, and clicking a row
       opens that class in the Classes tab.
    5. Cross-linking everywhere: tracker callers, recorder rows and
       monitor rows all open the originating class in Classes.
    6. Full tab set: Settings (font size via real atlas rebuild from a
       system font, opacity, theme, two scroll speeds), Patches
       (static fields; edit/delete), Hooks (Java(JS)/Smali/noop - smali
       mode assembles with the bundled jar and loads the dex with
       InMemoryDexClassLoader), Tracker (callers, field watch), Script
       (editor + console), Classes (search over loaded classes, open
       tabs, Instances/Methods/Fields, method Call editor).
    7. Floating toggle button: draggable, tap toggles the menu,
       always available.

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
