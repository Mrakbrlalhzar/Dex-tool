# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r15)

1. The keyboard STILL failed to open on some hosts (code-audit
   follow-up on r14). Three causes, all fixed:
   a) the Java.choose heap-scan fallback consumed its result
      synchronously - Java.choose is ASYNC, onMatch ran after
      `return found`, so the fallback always returned null.
      Activity resolution is now callback-based
      (find_current_activity_sync + resolve_current_activity(cb)).
   b) ime_show() hit `if (!IME.act) return;` BEFORE the retry
      ladder, so the hidden EditText + TextWatcher were never
      created; the showSoftInput retries are armed
      unconditionally now and each retry re-resolves the
      activity and retries the keyboard.
   c) the instance-field editor and the method-call argument
      fields used raw ImGui::InputText and never asked for the
      keyboard; every editable field goes through the
      ime_textfield wrapper now.

2. Touch mapping rebuilt on ONE coordinate space (the -80 px Y
   bias is gone): the hook forwards getRawX()/getRawY() (screen
   coords), the agent measures the GL view (largest
   SurfaceView/GLSurfaceView/TextureView, content-view fallback)
   with getLocationOnScreen(), and map_touch() scales BOTH axes
   proportionally: imgui = (raw - viewOrigin) * (DisplaySize /
   viewSize). The old scheme mixed view-local getX()/getY() with
   a window-relative content-view rect, hardcoded the Y scale to
   1.0 and patched the residue with a device-specific -80 px bias
   (the logged surface delta was 128 px, so the bias was wrong
   even on its own terms). Bias default is 0; the Settings slider
   stays for exotic hosts; the saved-config key was renamed
   (yoff -> ybias) so a stale -80 cannot fight the new mapping.

3. Backspace race (overlay.cpp): dextool_backspace() set a single
   bool, so N backspaces arriving within one frame collapsed into
   ONE deleted character (characters were queued, backspaces were
   not). It is a counter now.

4. smali_runtime.cpp extract_jar(): stored the java.io.File from
   getFilesDir() in a jstring and fed it to GetStringUTFChars - a
   JNI type mismatch that would crash the process the moment the
   native Smali path ran. Calls getAbsolutePath() properly now
   (with exception checks).

5. Log paths now MATCH: the native side logs to
   getFilesDir()/dextool-logs/ - exactly what the DexToolApp
   copier reads (before: native wrote /sdcard/Download while the
   copier read the private dir, so neither saw the other; under
   scoped storage the /sdcard writes failed silently). The JNI
   markers live in the same folder; /sdcard/Download stays as a
   fallback when the JVM is not reachable.

6. Build/git hygiene: the nolib APK is built from a scratch copy,
   so the committed DexToolApp.java keeps its System.loadLibrary()
   call (the old sed -i stripped it from the working tree, leaving
   a misleading "libtool.so loaded" log); the nolib variant also
   drops that log line. .gitignore covers .gradle/, build/,
   local.properties, .idea/, captures/ and *.iml, and the commit
   step untracks them.

## Fixes in r14 (still in)

1. The keyboard never opened. ime_show() dereferenced `act` - a
   variable that does not exist in that scope - so EVERY attempt to
   show the system IME died with a ReferenceError before
   showSoftInput was ever called, and the single 150 ms retry was
   skipped too (it lives after the throw). Fixed: the IME call uses
   IME.act; the activity is resolved from four sources (lifecycle
   callbacks, the onResume hook, the ActivityThread.mActivities
   walk, and a Java.choose heap-scan fallback); showSoftInput is
   retried at 150/450/900 ms with a fresh requestFocus; every IME
   step is written to the logs, so the next field report shows
   exactly which step failed and on which activity.

2. SIGABRT during agent load ("opened and run but no menu", crash
   log: CRASH signal=6 right after "script created, loading agent",
   with zero JS messages). All Java-touching init (lifecycle
   tracker, Activity hooks) is deferred out of gum_script_load_sync
   into the JS job queue, and each step writes a phase marker
   through the new dextool_agent_log() export into
   dextool-crash.log + dextool-debug.log. A host that kills itself
   (anti-tamper) now shows the exact phase that triggered it - and
   the load itself can no longer abort.

3. "Tapping the app's own menu spawns a small clone of our menu" +
   the clone being untouchable. When the owner context died (app
   menu, dialog, keyboard relayout), the parked ImGui world was
   ADOPTED by whatever surface reached 8 swaps next - including
   small popup/dialog surfaces, which rendered the whole menu
   inside their tiny window (and the clone ignored taps because the
   touch map belongs to the main surface). ensure_ownership() now
   refuses ownership for surfaces far smaller than the parked world
   (area < 50% or a dimension < 45%); tiny surfaces (< 220x220)
   never win election at all. Rejections are logged
   ("skip small surface").

4. Diagnostics: logger_init records VmRSS + MemAvailable (the
   "crash when I open any app" class - lmkd kills are invisible to
   signal handlers) and the agent phase markers above.

## Fixes in r12-r13 (still in)

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
