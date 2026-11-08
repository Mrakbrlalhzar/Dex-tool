# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r12)

1. "Open a class and the app freezes" - the field log shows the tap on
   a class is the last event the UI ever processed. Two independent
   causes, both fixed:
   a) menu.cpp locked g_mtx a SECOND time in tab_classes() (MLK inside
      MLK), and class_tab_content() nested again - with the plain
      mutex the render thread self-deadlocked, sitting inside the
      eglSwapBuffers hook, which froze the host's GL thread: the
      whole app hung. g_mtx is a RECURSIVE mutex now and the stray
      lock is gone.
   b) openClass() read every STATIC field value (f.get(null)); a
      static read can run the class's <clinit>, which may block
      forever inside app code. openClass() now lists names/types
      only; a "Read static values" button in the Fields sub-tab
      loads the values on demand for the open class.
   Tapping a recorded row / tracker caller froze the app through the
   same path (it opens the class tab too).

2. Y-axis scaling mismatch ("must tap above top items / below bottom
   items, only the center is right"). map_touch() scaled Y by
   ds.y / g_map.h - the view rect is TALLER than the GL surface
   (960 vs 832), so a false 0.867 factor pulled taps toward the
   center. Y is now a pure translation (sy = 1.0); the static
   status-bar offset is handled by the existing "Touch Y offset"
   slider (g_bias_y).

3. The built-in on-screen keyboard is REMOVED. Text fields use the
   PHONE's own keyboard: focusing a field posts showIme to the agent,
   which focuses a hidden 1x1 EditText on the resumed activity and
   opens the system IME. A TextWatcher diffs each edit against the
   previous text (.toString() - the method; the r10 bug was
   String(editable) -> "[object Object]") and forwards the delta to
   libtool.so as dextool_add_char()/dextool_backspace(). Done on the
   IME releases the ImGui field, so the next tap re-opens the
   keyboard; tapping the active field again re-opens it too.

4. The Classes tab lists ALL loaded classes BY DEFAULT - no
   "+ search" gate. The agent streams the full list in pages of 400
   with a query id (r11 mechanism, now triggered automatically when
   the tab opens); the filter row re-queries with a substring.

5. Recorder: Stop now also ABORTS a still-running installation (r11
   kept hooking classes after Stop, so the tab looked stuck on
   "installing ..."). Progress messages are throttled to one per
   250 ms (the r11 log flooded ~200 recorderProgress lines per
   install) and install chunks are 12 classes instead of 25, so the
   JS thread never stalls for seconds.

Also (r11, still in): scoped recorder (package filter + method cap),
eglTerminate slot retirement against the GL_INVALID_VALUE abort
class, park/adopt of the ImGui world across context death, UI state
persistence to <logdir>/dextool-ui.conf.


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
