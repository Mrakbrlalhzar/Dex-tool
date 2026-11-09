# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r18)

1. JNI crash / keyboard dead (field report on r17): "JNI DETECTED
   ERROR IN APPLICATION: use of invalid jobject" -> SIGABRT.
   Root cause: the r17 touch-hook capture stored `this` RAW in
   the long-lived IME.act. A Java wrapper handed to a hook
   implementation is only valid for the duration of that
   callback - its JNI reference is a LOCAL reference that dies
   when the callback returns (the frida-java-bridge docs say
   exactly this and provide Java.retain() for it: "Duplicates a
   JavaScript wrapper for later use outside replacement
   method"). The IME logic later calling
   getSystemService()/showSoftInput() on the stale object aborts
   the process: the keyboard never opens and the app dies.

2. Fix: every assignment into IME.act now goes through
   ime_set_act(), which stores a Java.retain()-ed wrapper (a
   GLOBAL JNI reference - valid after the callback returned,
   from any thread) and releases the previous retained activity
   when the tracked one changes, so activity switches never
   leak global references. The bridge does not export
   Java.release(), so cleanup calls the wrapper's $dispose()
   ("eagerly deletes the underlying JNI global reference"),
   preferring Java.release() if a future bridge provides it.

3. Retention applied to ALL capture sources, not just the touch
   hook: the onResume hook, the Application lifecycle callback,
   the ActivityThread.mActivities walk and the Java.choose heap
   scan all hand out equally callback-scoped wrappers - patching
   only the touch hook would have left the same SIGABRT one
   resume away (onResume can even fire before the first touch).

4. Capture logging rebuilt: "ime: activity captured & retained
   via <source>" fires per NEW activity (identity-checked with
   .equals - cheap, once per switch, never per touch event), so
   it covers first capture AND switches. The r17
   once-per-agent flag is retired, and the "IME host activity"
   info line moved into ime_set_act so every source reports
   identically and exactly once.

## Fixes in this version (r17)

1. IME failure, root cause: missing Activity capture in the touch
   hook (field report on r16). The dispatchTouchEvent hook
   intercepted taps, forwarded them to native and could even
   CONSUME them - but it never assigned IME.act. Only the
   onResume hook did, and that hook is lost when the agent
   attaches after the first resume (hot start: no further resume
   fires until the user leaves and returns). ime_show() was then
   thrown back to the fragile ActivityThread.mActivities walk /
   Java.choose heap scan, which on many hosts fails to find the
   resumed activity, so showSoftInput was never called. The
   touch hook now assigns IME.act = this on every event: the tap
   that opens the menu is itself proof of a live, resumed
   activity. Logged once per agent ("ime: activity captured via
   touch hook"), not at event rate.

2. Stale activity selection: find_current_activity_sync() only
   checked the paused/stopped record flags - a finishing or
   destroyed Activity passed, and getSystemService() then
   succeeded while showSoftInput silently no-opped. The walk
   filters dead activities out now (isFinishing()/isDestroyed(),
   not even eligible as the last-resort fallback), and the
   Java.choose heap scan does the same: destroyed instances
   linger until GC, so onMatch now keeps scanning past them and
   only stops at a live one.

3. Weak diagnostics fixed: the "no activity yet" warning
   carries its payload now (walk threw <exception> vs. walk
   empty vs. only dead/paused records), an empty walk logs what
   it saw, ime_try_show() logs a skipped attempt instead of
   returning false silently, showSoftInput's real return value
   is logged, and a final stage line 2.2 s after the ladder
   reports whether the bridge is up - with the reason when it
   is not (no activity captured vs. show kept failing).

4. Kept from r16: the async resolver callback performs the show
   itself (ime_act_resolved -> ime_try_show(false),
   main-thread scheduled, no text reset), and the retry ladder
   resolves through BOTH layers (sync walk first, heap scan
   second).

## Fixes in this version (r16)

1. The keyboard STILL failed to open on some hosts (field report
   on r15). Three residual timing defects, all fixed:
   a) the async resolve_current_activity() callback stored
      IME.act but never attempted the show - when every sync
      retry missed and the heap scan landed after the retry
      ladder was done, showSoftInput never ran. The callback
      performs the show itself now (ime_act_resolved),
      scheduled on the main thread, without resetting text.
   b) the retry ladder ran the SYNCHRONOUS ActivityThread walk
      only, so an activity found only by the async heap scan
      was never picked up. The ladder calls
      resolve_current_activity() now (sync walk first, heap
      scan second).
   c) ime_try_show() called ime_reset_text() on EVERY attempt;
      a retry landing while the user was already typing wiped
      the hidden EditText (and the TextWatcher diff then
      re-sent the surviving text as fresh insertions). Only
      the first attempt of a focus session resets now.

2. Crash-log plumbing (duplicated blocks / wrong directory):
   a) the Java copier appended the WHOLE source file every 5s
      (FileOutputStream(d, true)) - every pulled log held one
      copy per 5s of uptime. It now writes a temp file and
      rename(2)s it over the target: atomic, no duplication,
      no torn reads; files unchanged since the last pass are
      skipped.
   b) JNI_OnLoad runs inside System.loadLibrary, BEFORE
      Application.onCreate finishes - currentApplication() was
      null there, so native logs landed in /sdcard/Download
      (unwritable under scoped storage on modern devices) while
      the copier read filesDir/dextool-logs. DexToolApp now
      hands getFilesDir()/dextool-logs to the native logger
      right after loadLibrary (nativeSetLogDir ->
      logger_retarget; files reopened append-only, so nothing
      already written is lost).
   c) the uncaught-exception handler appends JAVA_CRASH to the
      SOURCE log now (an entry written straight to the
      Download copy was clobbered by the next copier pass) and
      pushes an immediate copy pass.
   d) the native crash fd opens with O_APPEND - concurrent
      writes (native signal handler, Java handler, copier)
      cannot clobber each other's offsets. Fatal-signal capture
      (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE: signal, si_code,
      fault addr, PC, module+offset) and the streamed logcat
      tombstone backtrace were already in place from r15.

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
