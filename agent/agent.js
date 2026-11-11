'use strict';
/* Dex-tool agent (r24). All Java logic lives here (frida-java-bridge);
   libtool.so only feeds input/GL. Messages both ways are JSON via
   send()/recv(); the C++ menu decodes them in menu.cpp.

   r24 changes (patch #3 - performance on heavy apps + logic/CI
   leftovers from the latest static audit; the menu froze and
   logcat showed "Skipped 40 frames! The application may be doing
   too much work on its main thread"):
   - Field-tracker flooding fixed: field_layer_emit() sent a
     trackerEvent on EVERY method call of the tracked class,
     changed or not (the old else-branch). Values are polled at
     most every 20 ms, emitted only when the value actually
     changed, and at most once per 100 ms per field - hot classes
     no longer flood the bridge.
   - dt_is_loaded() race fixed: the 3 s loaded-class cache could
     predate a class actually loading, and loadClass would never
     fire again - the queued install waited forever. A cache miss
     now forces ONE rebuild of the set (once per cache
     generation) before the class is declared unloaded.
   - Dispatcher overhead: cls/mn are parsed from the LAYERS key
     LAZILY (only recorder rows need them) instead of substring'ing
     the key on every hooked call; the dispatchTouchEvent hook
     keeps doing the minimum on the main thread.
   - Eaten gestures no longer leak UP: gTouchEat was cleared on
     UP/CANCEL BEFORE the consume check, so the app received an
     UP for a gesture whose DOWN the menu had consumed. The flag
     is captured first and consumed after.
   - IME ladder stops when the keyboard is already up: retries
     now check isAcceptingText() (logged once), and every show
     request carries a session token - newer requests and
     Done/hide invalidate the pending ladder instead of
     re-sending showSoftInput 4x per tap.
   - Enter is forwarded to ImGui: onEditorAction sends an
     "imeEnter" message BEFORE imeHidden; the menu injects
     ImGuiKey_Enter (DOWN, then UP next frame) so fields with
     EnterReturnsTrue (the class filter) actually search.

   r23 changes (patch #2 of the 47-item audit - 22 open + 7 partial
   items finished):
   - ONE layering registry (LAYERS) per class+method: Hook, Tracker,
     field-Tracker, Recorder and Monitor now STACK on the same method
     instead of overwriting each other's ov.implementation. A single
     dispatcher runs recorder -> monitor -> tracker -> field-diff ->
     user hook; removing the last layer of a method restores the
     original implementation.
   - Recorder Stop and Monitor Stop really UNHOOK: their layer is
     removed and the method's original implementation returns (the
     r22 stop only flipped a flag and left every hook installed).
   - listInstances retains every instance (Java.retain) and returns a
     structured protocol {str, handle, fields:[{n,t,v}]} - the old
     "toString##name=value;name=value" string corrupted every value
     containing | # or ;. setInstanceField/callMethod address the
     instance by HANDLE, so instance methods finally have a receiver
     (callMethod without a handle now refuses non-static calls with a
     hint instead of failing on ov.apply(null)).
   - Class access avoids premature <clinit> where possible: hook and
     tracker installs only Java.use classes that are ALREADY loaded;
     anything else is DEFERRED and installed by a loadClass watcher
     when the target loads the class (frida-java-bridge always runs
     ensureClassInitialized in use(), so "not loaded yet" is the one
     case we can avoid triggering).
   - trackField/tracker values no longer call toString() inside
     hooks: field_repr() stringifies primitives and boxes only, and
     reports '@class#identityHashCode' for anything else - no more
     recursion/crash through an app's own toString (which this agent
     may itself be recording).
   - Click-through: the dispatchTouchEvent hook asks
     dextool_hit_test(rawX, rawY) BEFORE the app sees the gesture and
     consumes the whole gesture when it starts on a live menu window
     (menu.cpp publishes every ImGui window rect each frame; the old
     gWantMouse path still called the app's dispatch and leaked taps).
   - Touch map view choice: the probe now scores SurfaceView/
     TextureView candidates against the overlay's ACTUAL GL surface
     size (dextool_display_size) instead of blindly taking the
     largest, and overlay.cpp dropped its coordinate clamping - taps
     outside the view no longer snap onto edge widgets (both axes
     were already proportional since r15).
   - IME focus: before the hidden EditText takes focus the app's
     currently-focused view is retained; ime_hide / Done / imeHidden
     restore focus to it and clear the EditText - the app gets its
     focus state back instead of the 1x1 view keeping it forever.
   - Everything from r22 (script new Function fix, mode
     normalisation, orig() receiver/args, patch snapshot/restore,
     ordered input queue + UTF-16, code-point deletions, OkHttp
     enqueue, readable console, smali loader hardening) and r21
     (addContentView host attach, ime_read_text cast chain) is kept.

   r22 changes (third-party audit, the 18 patch-fixed items ported
   here and MERGED with the r21 IME code - the audit's raw patch
   targeted a pre-r19 snapshot and its ime_on_text hunk would have
   clobbered the r21 fixes):
   - Script tab runs user code now: the LAST argument of
     new Function() is the body - the old call made "src" the body
     and passed the code as a parameter VALUE, so nothing executed.
   - Java(JS) hooks install: the menu label 'Java(JS)' is
     normalised to 'Java' (mode strings are compared literally).
   - orig() in Java hooks keeps the REAL receiver + arguments.
   - Patch OFF/delete RESTORES the original value (snapshotted
     before the first write); noop/'char' fallbacks return
     '\u0000' instead of 'a'.
   - Recorder pre-filters enumerateLoadedClassesSync() ONCE (the
     install loop counted skipped classes against its 12-class
     batch, so a package filter took ~a minute over 30k names).
   - Tracker callers no longer list the tracked method itself.
   - Monitor also hooks OkHttp enqueue (async requests).
   - Script console output is readable via cstr() (no 160-char
     cut, no '|'->'!' rewrite, objects JSON-stringified).
   - Smali jar: extracted to a temp file + rename (a killed
     process cannot leave a truncated jar), setReadOnly() for
     Android 14+ (writable dex refused), assembled via
     ClassFactory.get(loader) (loadClass returns a Class wrapper
     without static main()) trying both Main class names, source
     written as UTF-8.
   - smali_load uses the single-ByteBuffer InMemoryDexClassLoader
     ctor (the ByteBuffer[] one needs API 27; minSdk is 26).
   - IME deletions are counted in CODE POINTS: an astral char went
     in as 2 UTF-16 units but ImGui deletes one CHARACTER per
     backspace, so unit-counting over-deleted the field.
     Insertions stay per-UTF-16-unit; overlay.cpp (r22) routes the
     halves through io.AddInputCharacterUTF16() so pairs
     recombine.
   - Touch map also refreshed on ACTION_DOWN (throttled), not just
     on moves.
   - Dead code removed: DOTS/DMAX, parse_rules, lit_to_java,
     CONS_Lines/CONS_Dummy batching, gDispSize lookup.

   r21 changes (two field reports on the r20 build, both pinned by
   the r20 stage tags):
   - "stage decor.addView: decor.addView typeof undefined": on this
     bridge the wrapper returned by win.getDecorView() does not
     resolve ViewGroup methods, so the hidden EditText could never
     be attached. Fix per field fix-spec: attach through
     act.addContentView(edit, lp) instead - a plain public Activity
     method on the same wrapper whose hashCode/getWindow/
     getSystemService all work (proven by the successful r20 run
     that opened the keyboard). The win/decor stages are gone; the
     r19 early guard now probes BOTH act.getWindow and
     act.addContentView before any state mutation.
   - first keystroke typed "[object Object]" into the menu and then
     nothing: the Editable handed to afterTextChanged() had no
     bridged toString either, so ed.toString() fell through to
     QuickJS's Object.prototype.toString - it does NOT throw, it
     silently returns that constant, the watcher stores it as
     IME.prev and every later diff comes out empty. Fix:
     ime_read_text() re-wraps the Editable through
     Java.cast(ed, CharSequence).toString() (the same cast
     machinery as the proven cast to InputMethodManager), falls
     back to a java.lang.Object cast, then to
     CharSequence.toString.call(ed); every path rejects the
     "[object Object]" poison literal and a total read failure
     returns null - ime_on_text() then SKIPS the delta (the
     fix-spec fallback cur = '' would have fired backspaces for
     the whole field).
   - everything from r20 (stage tags, one-shot view-map probe) and
     r19 (retain + probe/rewrap, defensive factory resolution,
     delegating ime_ensure alias) is kept.

   r20 changes (field report on r19: the r19 diagnostics worked -
   the payload now reads {"fn":"ime_create_edit","msg":"not a
   function"}, once per ladder attempt. The failure is INSIDE
   the hidden-EditText factory):
   - root finding: the GumJS backend is QuickJS, and QuickJS
     reports any call through a missing/broken callee as a bare
     "not a function" - NO callee name (V8 would say
     "x.y is not a function"). Every r19 typeof guard passed
     silently (no "lacks act.getWindow", no probe/rewrap
     failure, no "no background-clear API"), so the dying call
     is one of the unguarded ones between act.getWindow() and
     setOnEditorActionListener() - indistinguishable from a
     bare message.
   - fix: ime_stage(name, fn) wraps EVERY call in the IME path
     (factory + ime_try_show); each callee is typeof-verified
     BEFORE the call and each failure rethrows as
     "stage <name>: <original>", so the next [agent error]
     names the exact call, e.g. "stage decor.addView: not a
     function". No behavior change on success.
   - one-shot view-map probe after the first activity capture:
     report_map()/find_gl_view() swallow their own errors, so
     the same failure class could hide there; the probe logs
     "view map probe ok/failed: <reason>" once.
   - everything from r19 (retain + probe/rewrap, defensive
     factory resolution, delegating ime_ensure alias) is kept.

   r19 changes (field report: tapping a text field logged
   "[agent error] ... ime_ensure ... not a function" and the
   keyboard never opened):
   - the truncated log line cannot distinguish a TypeError thrown
     AT the ime_ensure() call site from one thrown INSIDE
     ime_ensure() and reported through its own {fn:'ime_ensure'}
     error channel. Both shapes are now impossible, and any future
     failure names the exact missing member:
     - the EditText factory is renamed to ime_create_edit() (the
       canonical name) and the bare ime_ensure() INVOCATION is
       replaced: ime_try_show() resolves the factory through
       typeof checks and, if a binding were ever lost, logs BOTH
       typeofs instead of throwing a bare "ime_ensure is not a
       function" that halts the IME flow.
     - ime_ensure() stays defined as a DELEGATING legacy alias ->
       ime_create_edit(), so stale references in any build of
       this script resolve to the REAL factory (an EMPTY stub
       would make ime_try_show() bail and the keyboard would
       never open - do not replace the alias with a no-op).
     - ime_set_act() VERIFIES the Java.retain() result before
       trusting it: probes the retained wrapper for a real
       Activity method and, if a bridge ever returned a bare
       handle object, re-wraps it through Java.cast() (which
       also promotes the handle to a global reference).
     - ime_create_edit() typeof-guards the Activity/View methods
       it touches (act.getWindow, setBackgroundDrawable/
       setBackground, setAlpha): if a member is missing the log
       names it exactly, no more bare "X is not a function".

   r18 changes (field report: SIGABRT with "JNI DETECTED ERROR IN
   APPLICATION: use of invalid jobject" - the keyboard never opened
   and, when the IME path was forced, the app itself crashed):
   - root cause: the r17 touch-hook capture stored `this` RAW in the
     long-lived IME.act. A wrapper handed to a hook implementation
     is only valid for the duration of that callback (its JNI
     reference is LOCAL and dies when the callback returns); the
     later getSystemService()/showSoftInput() on the stale object
     aborts the process. The official bridge docs state exactly
     this and provide Java.retain() for it: "Duplicates a JavaScript
     wrapper for later use outside replacement method".
   - IME.act is stored through ime_set_act() now, which keeps a
     Java.retain()-ed GLOBAL reference (valid after the callback,
     on any thread) and releases the previous one when the tracked
     activity changes, so switches never leak. The bridge does not
     export Java.release(), so cleanup uses the wrapper's $dispose()
     ("eagerly deletes the underlying JNI global reference"), with
     Java.release() preferred if a bridge ever provides it.
   - retention covers ALL capture sources, not just the touch hook:
     onResume, the Application lifecycle callback, the
     ActivityThread walk and the Java.choose heap scan hand out
     equally callback-scoped wrappers - patching only the touch
     hook would have left the same crash one resume away.
   - capture logging: "ime: activity captured & retained via <src>"
     fires per NEW activity (identity-checked with .equals), i.e.
     on first capture AND on switches; the r17 once-per-agent flag
     is retired. The "IME host activity" info line moved into
     ime_set_act so every source reports identically, exactly once.

   r17 changes (field report: on many hosts the touch hook ran but
   no Activity capture ever reached IME.act, so ime_show() fell
   back to the fragile ActivityThread walk / heap scan, those
   fallbacks failed to find the resumed activity, and
   showSoftInput was never called):
   - the dispatchTouchEvent hook forwarded taps to native but never
     assigned IME.act - only the onResume hook did, and that hook
     is lost when the agent attaches after the first resume (hot
     start: no further resume fires until the user leaves and
     returns). The touch hook assigns IME.act = this on EVERY
     event now - the tap that opens the menu is itself proof of a
     live, resumed activity. Logged once ("ime: activity captured
     via touch hook"), not at event rate.
   - find_current_activity_sync() checked only the paused/stopped
     record flags: a finishing or destroyed Activity passed, then
     getSystemService() succeeded but showSoftInput silently
     no-opped - indistinguishable from "keyboard broken". The walk
     skips dead activities now (not even eligible as the
     last-resort fallback), and so does the Java.choose heap scan,
     which can surface destroyed-but-not-yet-collected instances.
   - diagnostics rebuilt: the "no activity yet" warning carries the
     walk's diagnosis as payload, ime_try_show() logs a skipped
     attempt instead of a silent false, showSoftInput's real
     return value is logged, and a final stage line reports the
     post-ladder bridge state with the reason when it stayed down.
   - kept from r16: the async resolver callback performs the show
     itself (ime_act_resolved -> ime_try_show(false)).

   r16 changes (field report: the keyboard STILL failed to open):
   - the async resolve_current_activity() callback stored IME.act
     but never attempted the show: when every synchronous retry
     missed and the heap scan landed after the retry ladder was
     done, showSoftInput never ran. The callback performs the show
     itself now (ime_act_resolved), scheduled on the main thread,
     without touching any text.
   - the retry ladder resolved the activity SYNCHRONOUSLY only;
     an activity found only by the async heap scan was never
     picked up. The ladder runs resolve_current_activity() now
     (sync walk first, heap scan second).
   - ime_try_show() reset the hidden EditText on EVERY attempt;
     a retry landing while the user was already typing wiped the
     field. Only the first attempt of a focus session resets.

   r15 changes (code-audit fixes):
   - IME: activity resolution is now ASYNC-correct. Java.choose
     fires onMatch LATER - the r14 fallback read the result before
     the callback ran and always got null. Split into
     find_current_activity_sync() (ActivityThread walk) and
     resolve_current_activity(cb) (sync walk first, heap scan
     second, result consumed strictly inside the callbacks).
   - IME: the showSoftInput retry ladder is armed even when no
     activity is known yet; every retry re-resolves and retries,
     so the hidden EditText + TextWatcher are created as soon as
     any activity appears (the r14 early return skipped them).
   - touch: the hook forwards getRawX()/getRawY() (SCREEN coords)
     and the map is measured with getLocationOnScreen() of the
     largest SurfaceView/GLSurfaceView/TextureView (content view
     fallback) - one coordinate space on both sides of the
     transform; overlay.cpp scales BOTH axes, the -80 px bias
     default is gone.

   r12 changes (field issues from the r11 run):
   - keyboard: the menu pops the PHONE's own keyboard (system IME).
     showIme/hideIme focus a hidden 1x1 EditText on the resumed
     activity; a TextWatcher diffs .toString() (the METHOD - the r10
     bug was String(editable) -> "[object Object]") and forwards the
     deltas as dextool_add_char()/dextool_backspace(). The r11 drawn
     keyboard is gone.
   - classes: the Classes tab lists ALL loaded classes by default
     (listLoadedClasses with an empty filter, still paged 400/qid).
   - openClass: no static field VALUE reads anymore - f.get(null) can
     run the class's <clinit>, which may block forever inside app
     code (that, plus a nested-lock deadlock in menu.cpp, froze the
     app when a class was opened). Values on demand via
     readStaticFields.
   - recorder: Stop now also aborts a running installation; progress
     messages are throttled (the r11 log flooded ~200
     recorderProgress lines per install) and install chunks are
     smaller so the JS thread never stalls for seconds.
   - touch map: measured on Activity.onResume AND on every touch
     (throttled); onWindowFocusChanged never fired on the field host.

   r14 changes (field issues from the r13 run):
   - keyboard never opened: ime_show() called act.getSystemService()
     with `act` undefined, so EVERY show attempt died with a
     ReferenceError before showSoftInput, and the retry (scheduled
     after the throw) never ran. Fixed; the activity is resolved
     from four sources (lifecycle callbacks, onResume hook,
     ActivityThread walk, heap-scan fallback) and showSoftInput is
     retried at 150/450/900 ms.
   - SIGABRT at agent load in some hosts ("opened and run but no
     menu", CRASH signal=6 right after "script created"): all Java
     init is deferred out of gum_script_load_sync and each phase is
     marked via the native dextool_agent_log export.
   - "small clone of our menu" in popup surfaces: the overlay
     refuses to adopt the parked ImGui world into surfaces far
     smaller than the parked DisplaySize (native side, r14).
*/

function sendMsg(type, payload) { send({ type: type, payload: payload }); }

/* r14: native phase logging. dextool_agent_log writes straight into
   dextool-crash.log + dextool-debug.log from libtool.so, so markers
   survive even when the GumJS message pump never runs again. */
var gAgentLog = null;
function agentLog(msg) {
    try {
        if (!gAgentLog) {
            var p = Module.findExportByName('libtool.so',
                                            'dextool_agent_log');
            if (!p) return;
            gAgentLog = new NativeFunction(p, 'void', ['pointer']);
        }
        gAgentLog(Memory.allocUtf8String(
            String(msg).substring(0, 400)));
    } catch (e) {}
}
agentLog('agent: script running (entry)');
function jstr(v) {
    try {
        if (v === null || v === undefined) return 'null';
        var s = String(v);
        if (s.length > 160) s = s.substring(0, 157) + '...';
        return s.replace(/\|/g, '!').replace(/#/g, '%');
    } catch (e) { return '?'; }
}
function jargs(a) {
    try {
        if (!a || !a.length) return '';
        return Array.prototype.map.call(a, jstr).join(',').substring(0, 240);
    } catch (e) { return '?'; }
}
/* (r23: describe() removed - describe_safe()/field_repr() render
   tracker values without calling the app's toString()) */

/* Java stack of the calling thread, app frames only, capped */
function jstack(limit) {
    try {
        var st = Java.use('java.lang.Thread').currentThread().getStackTrace();
        var out = [];
        for (var i = 0; i < st.length && out.length < (limit || 4); i++) {
            var f = st[i].toString();
            if (f.indexOf('java.') === 0 || f.indexOf('android.') === 0 ||
                f.indexOf('com.android.') === 0 ||
                f.indexOf('dalvik.') === 0) continue;
            out.push(f.split('(')[0]);
        }
        return out;
    } catch (e) { return []; }
}

/* r23: recursion-safe value renderer for tracker/field reads.
   jstr()/String(v) call the app's toString() INSIDE our hooks - when
   the recorded/tracked class's own toString (or something it calls)
   comes back through a hook, that recurses, floods the ring and can
   crash (the audit's "Tracker Side Effects" item). Only primitives
   and their boxes get real text here; every other object is reported
   as '@class#identityHashCode'. */
var gSysCls = null;
function field_repr(v) {
    try {
        if (v === null || v === undefined) return 'null';
        if (typeof v === 'string') return v.substring(0, 160);
        if (typeof v === 'number' || typeof v === 'boolean')
            return String(v);
        var cn = '';
        try { cn = String(v.$className || ''); } catch (e0) {}
        if (cn === 'java.lang.String' || cn === 'java.lang.Integer' ||
            cn === 'java.lang.Long'    || cn === 'java.lang.Boolean' ||
            cn === 'java.lang.Short'   || cn === 'java.lang.Byte' ||
            cn === 'java.lang.Float'   || cn === 'java.lang.Double' ||
            cn === 'java.lang.Character')
            return String(v).substring(0, 160);   /* boxes: toString is safe */
        if (cn) {
            var id = '?';
            try {
                if (!gSysCls) gSysCls = Java.use('java.lang.System');
                id = String(gSysCls.identityHashCode(v));
            } catch (e1) {}
            return '@' + cn + '#' + id;
        }
        return String(v).substring(0, 160);   /* plain JS values */
    } catch (e) { return '?'; }
}

/* r22/audit: parse_rules()/lit_to_java() removed - dead code,
   nothing called them */

var gAddChar = null, gBackspace = null, gWantMouse = null;
var gNativeTouch = null, gSetMap = null;
var gHitTest = null, gDisplaySize = null;   /* r23 */

function ensure_native_fns() {
    if (gNativeTouch) return true;
    try {
        var m = Module.findExportByName('libtool.so', 'dextool_add_char');
        var b = Module.findExportByName('libtool.so', 'dextool_backspace');
        var w = Module.findExportByName('libtool.so', 'dextool_want_mouse');
        var t = Module.findExportByName('libtool.so', 'dextool_on_touch');
        var s = Module.findExportByName('libtool.so', 'dextool_set_touch_map');
        if (!m || !b || !t || !s) return false;
        gAddChar   = new NativeFunction(m, 'void', ['int']);
        gBackspace = new NativeFunction(b, 'void', []);
        gNativeTouch = new NativeFunction(t, 'void', ['float', 'float', 'int']);
        gSetMap    = new NativeFunction(s, 'void', ['int', 'int', 'int', 'int']);
        if (w) gWantMouse = new NativeFunction(w, 'bool', []);
        /* r23: click-through filter + the owner surface's real size
           (both exports exist since r23; older libtool builds simply
           leave them null and the hooks fall back to r22 behaviour) */
        var h = Module.findExportByName('libtool.so', 'dextool_hit_test');
        if (h) gHitTest = new NativeFunction(h, 'bool', ['float', 'float']);
        var d = Module.findExportByName('libtool.so', 'dextool_display_size');
        if (d) gDisplaySize = new NativeFunction(d, 'int', []);
        return true;
    } catch (e) { return false; }
}

/* r15: measure where the overlay's GL view sits ON SCREEN and tell
   the overlay. Two changes vs r14:
   - getLocationOnScreen() (screen space) instead of
     getLocationInWindow() (window space): the hook now forwards
     getRawX()/getRawY(), so both sides of the transform share the
     same origin and no device-specific Y bias is needed.
   - the view measured is the largest SurfaceView / GLSurfaceView /
     TextureView in the window (that is the surface the overlay
     actually draws onto); android.R.id.content stays as fallback.
   Still called from Activity.onResume and throttled from every
   touch, so a relayout at any point in the app's life still
   updates the map. */
var gMapSent = { w: -1, h: -1, x: -9999, y: -9999 };
var gMapLastTouch = 0;
var gTouchEat = false;   /* r23: the current gesture started on the menu */
function report_map(view) {
    try {
        if (!view) return;
        var loc = Java.array('int', [0, 0]);
        view.getLocationOnScreen(loc);
        var w = view.getWidth(), h = view.getHeight();
        if (w <= 0 || h <= 0) return;
        if (w === gMapSent.w && h === gMapSent.h &&
            loc[0] === gMapSent.x && loc[1] === gMapSent.y) return;
        gMapSent = { w: w, h: h, x: loc[0], y: loc[1] };
        gSetMap(loc[0], loc[1], w, h);
        sendMsg('info', { msg: 'touch map (screen): ' + loc[0] + ',' + loc[1] + ' ' + w + 'x' + h });
    } catch (e) {}
}
function content_view(activity) {
    try {
        /* 0x01020002 = android.R.id.content */
        return activity.getWindow().getDecorView()
                        .findViewById(0x01020002);
    } catch (e) { return null; }
}
/* the view the overlay actually renders onto. r23: candidates are
   SCORED against the overlay's real GL surface size
   (dextool_display_size) instead of blindly taking the largest - an
   ad/video SurfaceView bigger than the game surface used to win and
   the map then belonged to the wrong view. Falls back to the largest
   candidate, then to android.R.id.content. */
function gl_surface_hint() {
    try {
        if (!gDisplaySize && !ensure_native_fns()) return null;
        if (!gDisplaySize) return null;
        var v = gDisplaySize();
        if (!v || v <= 0) return null;
        return { w: (v >>> 16) & 0xffff, h: v & 0xffff };
    } catch (e) { return null; }
}
function find_gl_view(activity) {
    try {
        var decor = activity.getWindow().getDecorView();
        if (!decor) return content_view(activity);
        var VG = Java.use('android.view.ViewGroup');
        var cands = [];
        var stack = [decor];
        while (stack.length) {
            var v = stack.pop();
            if (v === null || v === undefined) continue;
            var cn = '';
            try { cn = String(v.getClass().getName()); } catch (e0) {}
            if (cn.indexOf('SurfaceView') >= 0 ||
                cn.indexOf('GLSurfaceView') >= 0 ||
                cn.indexOf('TextureView') >= 0) {
                var w = 0, h = 0;
                try { w = v.getWidth(); h = v.getHeight(); } catch (e1) {}
                if (w > 0 && h > 0)
                    cands.push({ v: v, w: w, h: h });
            }
            var grp = null;
            try { grp = Java.cast(v, VG); } catch (e2) {}
            if (grp) {
                var n = grp.getChildCount();
                for (var i = 0; i < n; i++) stack.push(grp.getChildAt(i));
            }
        }
        if (!cands.length) return content_view(activity);
        var hint = gl_surface_hint();
        var best = null, bestScore = -1e30;
        for (var k = 0; k < cands.length; k++) {
            var c = cands[k];
            var score = -Math.abs(c.w * c.h);
            if (hint && hint.w > 0 && hint.h > 0) {
                /* closest total area wins, aspect-compatible preferred */
                score = -Math.abs(Math.log((c.w * c.h + 1) /
                                           (hint.w * hint.h + 1))) * 100;
                var ar1 = c.w / c.h, ar2 = hint.w / hint.h;
                if (Math.abs(ar1 - ar2) / ar2 < 0.25) score += 50;
            }
            if (score > bestScore) { bestScore = score; best = c; }
        }
        if (best) return best.v;
        return content_view(activity);
    } catch (e3) {}
    return content_view(activity);
}

/* ---------------- system IME bridge (r12) ----------------
   Pops the PHONE's own soft keyboard when a menu field is focused.
   A 1x1 transparent EditText is added to the resumed activity's
   window; its TextWatcher diffs each new text against the previous
   one (via .toString() - NOT String(), which is what turned the r10
   bridge into "[object Object]") and forwards the delta to
   libtool.so as dextool_add_char()/dextool_backspace(). */
/* r18: assign IME.act ONLY through ime_set_act() below. A wrapper
   handed to a hook or callback is a JNI LOCAL reference - valid
   for the duration of that callback only; stored raw and used
   later, it aborts the process with "JNI DETECTED ERROR IN
   APPLICATION: use of invalid jobject" (SIGABRT). ime_set_act()
   keeps a retained (global) reference instead and releases the
   previous one when the tracked activity changes. */
var IME = { act: null, edit: null, actHash: 0, prev: '',
            silent: false, cnt: 0, everLogged: false,
            mapProbed: false,                   /* r20: one-shot view-map probe */
            restore: null,                      /* r23: app view whose focus we borrowed */
            showSeq: 0,                         /* r24: show-request session token */
            upLogged: false };                  /* r24: "already up" logged once per session */

/* r18: release the previous retained activity. The bridge does not
   export Java.release(), so the documented cleanup is the
   wrapper's $dispose() ("eagerly deletes the underlying JNI
   global reference"); Java.release() is preferred if some bridge
   ever provides it. Best-effort: if neither exists, the JS-GC
   finalizer releases the ref eventually. */
function ime_release_act(obj) {
    try {
        if (!obj) return;
        if (typeof Java.release === 'function') { Java.release(obj); return; }
        if (typeof obj.$dispose === 'function') obj.$dispose();
    } catch (e) {}
}
/* r23: the hidden EditText STEALS focus from the app while the
   keyboard is open (unavoidable - the IME needs a focused view) -
   the fix is to give it BACK: remember the app's focused view before
   ours takes it, and restore + clearFocus when the keyboard closes
   (ime_hide, the IME's Done button, or the menu closing). Retained
   like IME.act: the wrapper is callback-scoped otherwise. */
function ime_set_restore(v) {
    try {
        if (IME.restore && v) {
            try { if (IME.restore.equals(v)) return; } catch (eQ) {}
        }
        if (IME.restore) ime_release_act(IME.restore);
        IME.restore = v ? Java.retain(v) : null;
    } catch (e) { IME.restore = null; }
}
function ime_restore_focus() {
    try {
        if (IME.edit) IME.edit.clearFocus();
    } catch (e0) {}
    try {
        if (IME.restore) IME.restore.requestFocus();
    } catch (e1) {}
}

/* r18: the ONLY writer of IME.act. Stores a Java.retain()-ed
   wrapper (global reference, valid after the producing callback
   returned and from any thread), releases the previous retained
   activity when the tracked one changes, and logs each real
   capture transition exactly once. */
function ime_set_act(obj, how) {
    if (!obj) return false;
    try {
        if (IME.act) {
            var same = false;
            try { same = IME.act.equals(obj); } catch (eQ) { same = false; }
            if (same) return false;   /* already tracked - reuse */
            ime_release_act(IME.act);
            /* r23: the saved focus view belonged to the OLD activity */
            if (IME.restore) { ime_release_act(IME.restore); IME.restore = null; }
        }
        /* r19: retain, then VERIFY the retained wrapper actually
           exposes Activity methods before trusting it. Every
           bridge checked (frida-java-bridge 5.x) returns a full
           method-capable Proxy from retain(), but if any build
           ever handed back a bare handle object, the first
           method call in the factory below would throw
           "X is not a function" and the keyboard would die
           with an undiagnosable log. The probe is one typeof;
           the re-wrap goes through Java.cast(), which promotes
           the handle to a global reference again. */
        var kept = null;
        try {
            kept = Java.retain(obj);   /* promote to GLOBAL ref */
        } catch (eT) {
            kept = obj;                /* ancient bridge fallback */
        }
        try {
            if (typeof kept.getWindow !== 'function') {
                var ACTc = Java.use('android.app.Activity');
                kept = Java.cast(kept, ACTc);
            }
        } catch (eW) {
            agentLog('ime: retained-wrapper probe/rewrap failed: ' +
                String(eW.message || eW));
        }
        IME.act = kept;
        agentLog('ime: activity captured & retained via ' + how);
        if (!IME.everLogged) {
            IME.everLogged = true;
            try {
                sendMsg('info', { msg: 'IME host activity: ' +
                    IME.act.getClass().getName() });
            } catch (eN) {}
        }
        return true;
    } catch (e) {
        return false;
    }
}

/* r21: read the watcher's Editable as a JS string. The r12-era
   ed.toString() is POISON on this bridge: the Editable handed to
   afterTextChanged() gets no bridged toString, so QuickJS falls
   through to Object.prototype.toString - no exception, just the
   constant "[object Object]" (the same poison the r10 String()
   bug produced, now through a third path). Every path here
   either returns a real string or null; the poison literal is
   rejected as well. */
var imeReadWarned = false;
function ime_read_text(ed) {
    try {
        if (typeof ed === 'string') return ed;
    } catch (eS) {}
    try {
        var seq = Java.cast(ed, Java.use('java.lang.CharSequence'));
        var s1 = seq.toString();
        if (typeof s1 === 'string' && s1 !== '[object Object]')
            return s1;
    } catch (e0) {}
    try {
        var s2 = Java.cast(ed, Java.use('java.lang.Object')).toString();
        if (typeof s2 === 'string' && s2 !== '[object Object]')
            return s2;
    } catch (e1) {}
    try {
        var s3 = Java.use('java.lang.CharSequence').toString.call(ed);
        if (typeof s3 === 'string' && s3 !== '[object Object]')
            return s3;
    } catch (e2) {}
    if (!imeReadWarned) {
        imeReadWarned = true;
        agentLog('ime: Editable unreadable (typeof ed=' + (typeof ed) +
            ', all CharSequence casts failed) - text updates skipped');
    }
    return null;
}

function ime_on_text(ed) {
    try {
        if (!ensure_native_fns()) return;
        /* r21: a failed read must NOT run the delta - diffing
           against '' would fire backspaces for the whole field
           (the fix-spec fallback had exactly that hazard). Skip
           this change and keep IME.prev; the next successful read
           diffs against the last KNOWN text and recovers. */
        var cur = ime_read_text(ed);
        if (cur === null) return;
        cur = String(cur);
        if (IME.silent) { IME.prev = cur; return; }   /* programmatic */
        var prev = IME.prev;
        if (cur === prev) return;
        /* longest common prefix / suffix -> the delta */
        var s = 0;
        while (s < prev.length && s < cur.length && prev[s] === cur[s]) s++;
        var e = 0;
        while (e < prev.length - s && e < cur.length - s &&
               prev[prev.length - 1 - e] === cur[cur.length - 1 - e]) e++;
        /* r22/audit: deletions are counted in CODE POINTS - an
           astral char entered as 2 UTF-16 units, but ImGui deletes
           one CHARACTER per backspace, so counting UTF-16 units
           over-deleted the field. Insertions stay per-UTF-16-unit;
           overlay.cpp (r22) keeps this order in ONE queue and
           recombines surrogate halves via
           io.AddInputCharacterUTF16(). */
        var gone = prev.substring(s, prev.length - e), delN = 0;
        for (var gi = 0; gi < gone.length; gi++) {
            var gc = gone.charCodeAt(gi);
            if (gc < 0xDC00 || gc > 0xDFFF) delN++;   /* skip low surrogates */
        }
        for (var d = 0; d < delN; d++) gBackspace();
        var ins = cur.substring(s, cur.length - e);
        for (var i = 0; i < ins.length; i++) gAddChar(ins.charCodeAt(i));
        IME.prev = cur;
    } catch (e) {}
}

function ime_reset_text() {
    try {
        IME.silent = true;
        if (IME.edit) IME.edit.setText('');
        IME.prev = '';
        IME.silent = false;
    } catch (e) { IME.silent = false; }
}

/* r20: QuickJS (the GumJS QJS backend) reports ANY call through a
   missing or non-callable value as a bare "not a function" -
   WITHOUT the callee name (V8 says "x.y is not a function",
   QuickJS does not). The r19 field log ("msg":"not a function"
   under fn:ime_create_edit, 5x per tap = the whole retry
   ladder) therefore proved the failure is inside the factory
   but not WHICH call dies. Every risky call in the IME path is
   wrapped in a stage tag now: the next error payload reads
   "stage <name>: <original message>" and pins the exact call. */
function ime_stage(name, fn) {
    try { return fn(); }
    catch (eS) {
        throw new Error('stage ' + name + ': ' +
            String((eS && eS.message) || eS));
    }
}

/* r19: renamed from ime_ensure() to ime_create_edit() - the
   canonical factory name. Every Activity/View method touched
   here is typeof-guarded now: if a bridge or OEM build lacks a
   member, the log names the exact member instead of a bare
   "X is not a function" that is impossible to triage. */
function ime_create_edit(act) {
    var h = 0;
    try { h = act.hashCode(); } catch (e0) {}
    if (IME.edit && IME.actHash === h) return IME.edit;
    try {
        /* r21: sanity-probe the retained wrapper BEFORE any
           mutation. getWindow is the r19 Activity probe (kept);
           addContentView is now the attach call itself, so a
           missing member is caught here - before IME.edit and
           IME.prev are wiped - instead of on every ladder
           attempt. */
        if (typeof act.getWindow !== 'function' ||
            typeof act.addContentView !== 'function') {
            sendMsg('error', { fn: 'ime_create_edit',
                msg: 'retained activity missing members: getWindow typeof ' +
                    (typeof act.getWindow) + ', addContentView typeof ' +
                    (typeof act.addContentView) });
            return null;
        }
        IME.edit = null;               /* activity switched */
        IME.prev = '';
        /* r20: every step below is stage-tagged - see ime_stage().
           Each callee is typeof-verified BEFORE the call, so a
           missing member is named exactly; a present-but-throwing
           member surfaces as "stage <name>: <original>".
           r21: the win/getDecorView stages are GONE - on this
           bridge the DecorView wrapper does not resolve ViewGroup
           methods (decor.addView typeof undefined, 35x in the
           field log), so the EditText is attached through
           act.addContentView() below instead. */
        var edit = ime_stage('EditText.$new', function () {
            var ET = Java.use('android.widget.EditText');
            if (typeof ET.$new !== 'function')
                throw new Error('EditText.$new typeof ' +
                    (typeof ET.$new));
            return ET.$new(act);
        });
        ime_stage('clear background', function () {
            /* r19: setBackgroundDrawable is deprecated-but-present on
               every current API; setBackground is the modern name.
               Use whichever this bridge exposes; if neither exists,
               say so instead of throwing. */
            if (typeof edit.setBackgroundDrawable === 'function')
                edit.setBackgroundDrawable(null);   /* 1x1, transparent */
            else if (typeof edit.setBackground === 'function')
                edit.setBackground(null);
            else
                agentLog('ime: no background-clear API on EditText (skipped)');
        });
        ime_stage('set alpha', function () {
            if (typeof edit.setAlpha === 'function') edit.setAlpha(0);
        });
        var lp = ime_stage('FrameLayout$LayoutParams.$new', function () {
            var LP = Java.use('android.widget.FrameLayout$LayoutParams');
            if (typeof LP.$new !== 'function')
                throw new Error('LayoutParams.$new typeof ' +
                    (typeof LP.$new));
            return LP.$new(1, 1);
        });
        ime_stage('act.addContentView', function () {
            if (typeof act.addContentView !== 'function')
                throw new Error('act.addContentView typeof ' +
                    (typeof act.addContentView));
            /* lands the edit in android.R.id.content - the same
               attached window hierarchy a decor.addView() used to
               reach, but through the Activity methods this bridge
               resolves; lp (FrameLayout.LayoutParams) matches
               that container exactly. */
            act.addContentView(edit, lp);
        });
        var tag = (++IME.cnt) + '_' + (Date.now() % 1000000);
        var tw = ime_stage('registerClass TextWatcher', function () {
            var TW = Java.registerClass({
                name: 'com.dextool.ime.W' + tag,
                implements: [Java.use('android.text.TextWatcher')],
                methods: {
                    beforeTextChanged: function (s, a, b, c) {},
                    onTextChanged: function (s, a, b, c) {},
                    afterTextChanged: function (ed) { ime_on_text(ed); }
                }
            });
            if (typeof TW.$new !== 'function')
                throw new Error('watcher class $new typeof ' +
                    (typeof TW.$new));
            return TW.$new();
        });
        ime_stage('addTextChangedListener', function () {
            if (typeof edit.addTextChangedListener !== 'function')
                throw new Error('edit.addTextChangedListener typeof ' +
                    (typeof edit.addTextChangedListener));
            edit.addTextChangedListener(tw);
        });
        var ea = ime_stage('registerClass EditorActionListener', function () {
            var EA = Java.registerClass({
                name: 'com.dextool.ime.E' + tag,
                implements: [Java.use('android.widget.TextView$OnEditorActionListener')],
                methods: {
                    onEditorAction: function (v, actionId, ev) {
                        /* r24: Done/Search on the phone IME now SEARCHES -
                           the filter field declares EnterReturnsTrue but the
                           old handler only hid the keyboard. Forward a real
                           Enter key press FIRST (the menu injects it as a
                           two-frame key event), then release the field so
                           the next tap on it re-opens the keyboard. */
                        IME.showSeq++;            /* keyboard closing: ladder stops */
                        sendMsg('imeEnter', {});
                        sendMsg('imeHidden', {});
                        ime_reset_text();
                        /* r23: give the app its focused view back */
                        ime_restore_focus();
                        return false;
                    }
                }
            });
            if (typeof EA.$new !== 'function')
                throw new Error('editor-action class $new typeof ' +
                    (typeof EA.$new));
            return EA.$new();
        });
        ime_stage('setOnEditorActionListener', function () {
            if (typeof edit.setOnEditorActionListener !== 'function')
                throw new Error('edit.setOnEditorActionListener typeof ' +
                    (typeof edit.setOnEditorActionListener));
            edit.setOnEditorActionListener(ea);
        });
        IME.edit = edit;
        IME.actHash = h;
        return edit;
    } catch (e) {
        sendMsg('error', { fn: 'ime_create_edit', msg: String(e.message || e) });
        return null;
    }
}
/* r19: legacy alias (fix-spec step 3) - delegates to the canonical
   ime_create_edit() factory so any leftover ime_ensure() reference
   in any build of this script resolves to the REAL factory. An
   EMPTY stub here would make ime_try_show() bail (edit === null)
   and the keyboard would never open - keep the delegation. */
function ime_ensure(act) {
    return ime_create_edit(act);
}

function ime_try_show(reset) {
    /* one complete attempt on the main thread: create the hidden
       EditText (+ TextWatcher) if needed, focus it, and send
       showSoftInput. Returns true only if showSoftInput was sent.
       r16: reset only when the caller says so - the first attempt
       of a focus session passes true, retries and async-resolved
       shows pass false. Resetting on every attempt wiped user
       input when a retry landed while typing was already under
       way. */
    try {
        if (!IME.act) {
            /* r17: stage log - the silent false return made "why
               did the keyboard not show" undiagnosable */
            agentLog('ime: try_show skipped - no activity yet');
            return false;
        }
        /* r19 (fix-spec step 2): the bare ime_ensure() INVOCATION
           is replaced by the canonical ime_create_edit() factory,
           resolved defensively - if either binding were ever lost
           the log names both typeofs instead of throwing a bare
           "ime_ensure is not a function" that halts the flow. */
        var makeEdit = (typeof ime_create_edit === 'function')
            ? ime_create_edit : ime_ensure;
        if (typeof makeEdit !== 'function') {
            sendMsg('error', { fn: 'ime_try_show',
                msg: 'ime factory unavailable: ime_create_edit typeof=' +
                    (typeof ime_create_edit) + ', ime_ensure typeof=' +
                    (typeof ime_ensure) });
            return false;
        }
        var edit = makeEdit(IME.act);
        if (!edit) return false;
        if (reset !== false) ime_reset_text();
        /* r23: remember which app view currently holds focus BEFORE our
           hidden EditText steals it, so it can be handed back later */
        try {
            var cur = IME.act.getWindow().getCurrentFocus();
            if (cur) {
                var isOurs = false;
                try { if (IME.edit && cur.equals(IME.edit)) isOurs = true; }
                catch (eC) {}
                if (!isOurs) ime_set_restore(cur);
            }
        } catch (eF) {}
        /* r20: same stage tagging as the factory - if the factory
           passes and the next failure lands here, the payload
           names the exact call instead of a bare TypeError. */
        ime_stage('edit.requestFocus', function () {
            edit.requestFocus();
        });
        var imm = ime_stage('getSystemService(input_method)', function () {
            var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
            return Java.cast(IME.act.getSystemService('input_method'), IMMgr);
        });
        ime_stage('showSoftInput', function () {
            var sent = imm.showSoftInput(edit, 0);
            /* r17: log the REAL result - false here (window token not
               ready, focus not granted yet) was invisible before and
               is the classic "sent but nothing happened" failure that
               the retry ladder exists for */
            agentLog('ime: showSoftInput sent -> ' + sent);
        });
        return true;
    } catch (e) {
        sendMsg('error', { fn: 'ime_try_show', msg: String(e.message || e) });
        return false;
    }
}

/* r16: shared handler for an asynchronously resolved activity.
   The r15 callback only stored IME.act - when every sync retry
   missed (ActivityThread walk empty right after resume) and the
   heap scan landed AFTER the retry ladder had finished, nothing
   ever called showSoftInput and the keyboard never opened. The
   callback now attempts the show itself:
   - the UI work (ime_create_edit creates a view) must run on the
     main thread, hence Java.scheduleOnMainThread;
   - no ime_reset_text() here: by the time the async scan lands
     the user may already be typing. */
function ime_act_resolved(a, how) {
    if (!a) return;
    /* r18: retained through ime_set_act, NOT stored raw - the
       walk/heap-scan wrapper is callback-scoped exactly like the
       touch hook's `this` (same invalid-jobject abort). */
    ime_set_act(a, 'async resolve (' + how + ')');
    Java.scheduleOnMainThread(function () {
        try {
            if (ime_try_show(false))
                agentLog('ime: show ok after ' + how);
        } catch (e2) {}
    });
}

function ime_show() {
    /* r24: every show request starts a NEW session - pending retries
       of an older ladder are invalidated (they used to pile up when
       fields were tapped quickly and kept re-sending showSoftInput
       long after their keyboard was gone) */
    var seq = ++IME.showSeq;
    IME.upLogged = false;
    Java.perform(function () {
        Java.scheduleOnMainThread(function () {
            try {
                /* r15: NO early return before the retries. The r14
                   version bailed out when IME.act was still null and
                   the whole retry ladder sat behind that return - the
                   hidden EditText and TextWatcher were never created
                   and the keyboard never opened. Now: resolve the
                   activity as far as possible RIGHT NOW (sync walk;
                   the async heap scan fills IME.act in the
                   background), then arm the retry ladder
                   UNCONDITIONALLY - each retry re-resolves and makes
                   a full attempt, so the bridge comes up as soon as
                   any activity appears. */
                var walkErr = null;   /* r17: warn payload, belt+braces */
                if (!IME.act) {
                    try { ime_set_act(find_current_activity_sync(),
                                      'sync walk'); }   /* r18: retained */
                    catch (e0) {
                        walkErr = String(e0.message || e0);
                        agentLog('ime: findAct failed: ' + walkErr);
                    }
                }
                if (!IME.act) {
                    /* r17 FIX: the warning carries its payload now -
                       WHY no activity was found (the walk's own
                       diagnosis: exception / no records / only dead
                       or paused ones) instead of a bare "[msg]" that
                       made IME failures impossible to triage */
                    sendMsg('warn', { msg: 'IME: no activity yet (' +
                        (walkErr ? 'walk threw: ' + walkErr
                                 : String(find_current_activity_sync.lastDiag ||
                                          'unknown')) +
                        ') - async resolve, retries armed' });
                    /* r16 CRITICAL FIX: the r15 callback only stored
                       the activity and returned - if the heap scan
                       landed after the retries were exhausted,
                       showSoftInput never ran. The callback attempts
                       the show itself now. */
                    resolve_current_activity(function (a) {
                        ime_act_resolved(a, 'async');
                    });
                }
                /* r18: the "IME host activity" info line moved into
                   ime_set_act (fires on the FIRST capture from any
                   source); removed here - no double logging. */
                ime_try_show(true);   /* r16: only the first attempt resets */
                /* the first show right after focus often loses the
                   race with the window - retry on a ladder; each
                   retry also re-resolves the activity (the r14
                   ladder lived behind the early return and never
                   ran when IME.act was null) */
                [150, 450, 900, 1500].forEach(function (delay) {
                    setTimeout(function () {
                        Java.scheduleOnMainThread(function () {
                            try {
                                /* r24: stale ladder (a newer show request
                                   or Done/hide superseded it) - stop */
                                if (seq !== IME.showSeq) return;
                                if (!IME.act) {
                                    /* r16: BOTH resolution layers run
                                       in the ladder. resolve_current_
                                       activity walks ActivityThread
                                       synchronously first and only
                                       falls back to the async heap
                                       scan; the callback attempts the
                                       show itself when it wins. The
                                       r15 ladder ran the sync walk
                                       only, so an activity that was
                                       heap-scan-only was never found
                                       here. */
                                    resolve_current_activity(function (a) {
                                        ime_act_resolved(a,
                                            'retry ' + delay + 'ms');
                                    });
                                    return;
                                }
                                /* r24: keyboard already up - the ladder
                                   STOPS instead of re-sending showSoftInput
                                   4x per tap (the "showSoftInput sent ->
                                   true" console flooding) */
                                var up = false;
                                try {
                                    var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
                                    var immx = Java.cast(
                                        IME.act.getSystemService('input_method'),
                                        IMMgr);
                                    up = !!immx.isAcceptingText();
                                } catch (eI) {}
                                if (up) {
                                    if (!IME.upLogged) {
                                        IME.upLogged = true;
                                        agentLog('ime: keyboard already up - ladder stops');
                                    }
                                    return;
                                }
                                if (ime_try_show(false))
                                    agentLog('ime: retry ok at ' + delay + 'ms');
                            } catch (e2) {}
                        });
                    }, delay);
                });
                /* r17: final stage line - one entry that says whether
                   the bridge actually came up once the ladder is
                   done, with the reason when it did not. Without it
                   "keyboard never opened" arrived with zero clues. */
                setTimeout(function () {
                    Java.perform(function () {
                        if (IME.edit)
                            agentLog('ime: bridge up after ladder');
                        else
                            sendMsg('warn', { msg: 'IME: bridge still down after retries (' +
                                (IME.act ? 'activity captured, showSoftInput kept failing'
                                         : 'no activity captured at all') + ')' });
                    });
                }, 2200);
            } catch (e) {
                sendMsg('error', { fn: 'showIme', msg: String(e.message || e) });
            }
        });
    });
}

/* r15: activity resolution, two layers.
   find_current_activity_sync(): walk ActivityThread.mActivities
   (IBinder -> ActivityClientRecord) and return a live activity;
   works with ArrayMap and HashMap because both expose values().
   Prefers a record that is neither paused nor stopped. No hook
   timing involved, so it also covers the very first resume that
   the onResume hook may have missed. Purely synchronous.
   resolve_current_activity(cb): sync walk first; if that comes up
   empty, scan the heap with Java.choose. Java.choose is
   ASYNCHRONOUS - the r14 fallback read `found` right after the
   call, before onMatch ever ran, so it ALWAYS returned null. The
   result is now consumed strictly inside the callbacks: onMatch
   stores it in IME.act and invokes cb. */
function find_current_activity_sync() {
    var best = null;
    var seen = 0, deadSkipped = 0, threw = null;
    try {
        var at = Java.use('android.app.ActivityThread').currentActivityThread();
        var acts = at.mActivities.value;
        var it = acts.values().iterator();
        while (it.hasNext()) {
            var rec = it.next();
            if (rec === null) continue;
            var a = null;
            try { a = rec.activity.value; } catch (e1) { continue; }
            if (a === null) continue;
            seen++;
            /* r17 FIX: skip dying activities. The r16 walk checked
               only paused/stopped from the client record - a
               finishing or destroyed Activity passed, and ime_show()
               then called getSystemService() on a dead window: no
               throw, but showSoftInput silently no-ops. Dead
               records are not even eligible for the last-resort
               fallback; paused/stopped still are. */
            var dead = false;
            try { dead = a.isFinishing() || a.isDestroyed(); }
            catch (eZ) {}
            if (dead) { deadSkipped++; continue; }
            best = a;
            var paused = false, stopped = false;
            try { paused = rec.paused.value; } catch (e2) {}
            try { stopped = rec.stopped.value; } catch (e3) {}
            if (!paused && !stopped) return a;
        }
    } catch (e4) { threw = String(e4.message || e4); }
    /* r17: stage diagnostics - an empty walk explains itself, and
       the reason is exposed on the function object so the "no
       activity yet" warning can carry it as real payload */
    var diag;
    if (threw !== null) {
        diag = 'walk threw: ' + threw;
    } else if (seen > 0 && !best) {
        diag = 'walk saw ' + seen + ' act(s), ' + deadSkipped +
               ' dead, none usable';
    } else if (seen === 0) {
        diag = 'walk empty (no mActivities records)';
    } else {
        diag = 'only paused/stopped records';
    }
    agentLog('ime: ' + diag);
    find_current_activity_sync.lastDiag = diag;
    return best;
}

function resolve_current_activity(cb) {
    var a = null;
    try { a = find_current_activity_sync(); } catch (e0) {}
    if (a) { try { cb(a); } catch (e1) {} return; }
    /* r15: heap scan - ASYNC; the result is handled in onMatch */
    try {
        Java.choose('android.app.Activity', {
            onMatch: function (act) {
                /* r17 FIX (same defect as the sync walk): the heap
                   also holds DESTROYED activities until GC collects
                   them. Handing ime_show() one of those reproduces
                   the exact field failure - so keep scanning past
                   dead instances and only stop at a live one. */
                var dead = false;
                try { dead = act.isFinishing() || act.isDestroyed(); }
                catch (eX) {}
                if (dead) return;   /* keep scanning */
                ime_set_act(act, 'heap scan');   /* r18: retained */
                try { cb(act); } catch (e3) {}
                return 'stop';
            },
            onComplete: function () {}
        });
    } catch (e4) {}
}

/* r13: keep IME.act fresh for every future resume via Application
   callbacks. Unlike the Activity.onResume implementation hook this
   fires for all activities even when the agent lost the race with the
   first resume, and it also tracks activity SWITCHES. It deliberately
   does NOT call report_map(): onResume (and the throttled touch
   path) already re-measure the view rect there. */
function install_lifecycle() {
    Java.perform(function () {
        try {
            var app = Java.use('android.app.ActivityThread').currentApplication();
            if (!app) throw new Error('Application not up yet');
            var CB = Java.registerClass({
                name: 'com.dextool.ime.LC' + (Date.now() % 1000000),
                implements: [Java.use('android.app.Application$ActivityLifecycleCallbacks')],
                methods: {
                    onActivityCreated: function (a, b) {},
                    onActivityStarted: function (a) {},
                    onActivityResumed: function (a) {
                        /* r18: retained via ime_set_act - the callback
                           argument is as callback-scoped as a hook's
                           `this` (same invalid-jobject abort class) */
                        try { ime_set_act(a, 'lifecycle'); } catch (e0) {}
                    },
                    onActivityPaused: function (a) {},
                    onActivityStopped: function (a) {},
                    onActivitySaveInstanceState: function (a, b) {},
                    onActivityDestroyed: function (a) {}
                }
            });
            app.registerActivityLifecycleCallbacks(CB.$new());
            agentLog('phase: lifecycle tracker armed');
            sendMsg('info', { msg: 'lifecycle tracker armed' });
        } catch (e) {
            /* the Application exists before any activity, so a few
               short retries always succeed */
            if (install_lifecycle.tries === undefined) install_lifecycle.tries = 0;
            if (install_lifecycle.tries++ < 20)
                setTimeout(install_lifecycle, 250);
            else
                sendMsg('warn', { msg: 'lifecycle tracker not armed: ' + String(e.message || e) });
        }
    });
}

function ime_hide() {
    IME.showSeq++;            /* r24: keyboard closing - ladder stops */
    Java.perform(function () {
        Java.scheduleOnMainThread(function () {
            try {
                if (!IME.edit || !IME.act) return;
                var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
                var imm = Java.cast(IME.act.getSystemService('input_method'), IMMgr);
                imm.hideSoftInputFromWindow(IME.edit.getWindowToken(), 0);
                ime_reset_text();
                /* r23: the hidden 1x1 EditText used to KEEP the app's
                   focus forever - hand the previously focused view back */
                ime_restore_focus();
            } catch (e) {}
        });
    });
}


/* ---------------- input + view mapping on the host activity -------- */
function installTouchHook() {
    if (!ensure_native_fns()) {
        sendMsg('error', { fn: 'installTouchHook', msg: 'libtool.so natives missing' });
        return;
    }
    Java.perform(function () {
        var Activity = Java.use('android.app.Activity');
        agentLog('phase: hooking Activity.onResume');
        try {
            Activity.onResume.implementation = function () {
                var r = this.onResume();
                /* r18: retained via ime_set_act - raw `this` is a JNI
                   LOCAL reference that dies when this callback returns;
                   using it later is the exact r17 crash */
                try { ime_set_act(this, 'onResume'); } catch (e0) {}
                try { report_map(find_gl_view(this)); } catch (e) {}
                return r;
            };
            agentLog('phase: onResume hook ok');
        } catch (e1) {
            agentLog('ERR onResume hook: ' + e1);
            sendMsg('error', { fn: 'hookOnResume',
                               msg: String(e1 && e1.message || e1) });
        }
        agentLog('phase: hooking Activity.dispatchTouchEvent');
        Activity.dispatchTouchEvent.implementation = function (ev) {
            /* MAIN THREAD */
            /* r18 FIX (root cause of the r17 field crash): the r17
               capture stored `this` RAW in the long-lived IME.act.
               A wrapper handed to a hook implementation is only
               valid for the duration of that callback - its JNI
               local reference dies when the callback returns, and
               the later getSystemService()/showSoftInput() on
               IME.act then aborts the process with "JNI DETECTED
               ERROR IN APPLICATION: use of invalid jobject"
               (SIGABRT): keyboard dead AND app dead. ime_set_act()
               stores a Java.retain()-ed GLOBAL reference instead
               (valid after the callback, on any thread) and
               releases the previous retained activity when the
               tracked one changes, so nothing leaks. The tap that
               opens the menu is still proof of a live, resumed
               activity (r17 rationale kept); the identity check
               keeps this branch cheap and the log per-activity,
               not per-event. */
            try {
                if (!IME.act || !IME.act.equals(this))
                    ime_set_act(this, 'touch hook');
            } catch (eAct) {}
            /* r20: one-shot view-map probe. report_map() and
               find_gl_view() swallow their own errors, so the same
               failure class that kills the IME factory would hide
               here. Run the measurement once after the first
               capture and log the outcome verbatim. */
            if (IME.act && !IME.mapProbed) {
                IME.mapProbed = true;
                try {
                    report_map(find_gl_view(this));
                    agentLog('ime: view map probe ok');
                } catch (eM) {
                    agentLog('ime: view map probe failed: ' +
                        String((eM && eM.message) || eM));
                }
            }
            try {
                var act = ev.getAction() & 0xff;
                var down = (act === 0);
                /* r15: raw SCREEN coords (getRawX/getRawY) - the
                   same space the map is measured in
                   (getLocationOnScreen), so overlay.cpp's transform
                   is exact on BOTH axes */
                var rx = ev.getRawX(), ry = ev.getRawY();
                gNativeTouch(rx, ry, act);
                /* r23 click-through: ask the overlay whether this
                   gesture STARTS on a live menu window BEFORE the app
                   sees anything. The r22 attempt returned true when
                   WantCaptureMouse was set but still CALLED
                   this.dispatchTouchEvent(ev) for down/up first - the
                   tap reached the app anyway (the audit's
                   "taps on the menu still leak to the host app").
                   The hit test reads the window rects menu.cpp/overlay
                   publish each frame, mapped through the touch map. */
                if (down && gHitTest) {
                    try { gTouchEat = !!gHitTest(rx, ry); } catch (eH) {}
                }
                /* r24: capture BEFORE clearing - the old order cleared
                   the flag on UP/CANCEL first, so the consume check
                   below never saw it and the app received an UP for a
                   gesture whose DOWN the menu had eaten ("eaten
                   gestures leak UP") */
                var eat = gTouchEat;
                if (act === 1 || act === 3) gTouchEat = false;  /* UP/CANCEL */
                if (act === 0 || act === 2) {   /* r22/audit: throttled map
                                                   refresh on DOWN + MOVE */
                    var now = Date.now();
                    if (now - gMapLastTouch > 2000) {
                        gMapLastTouch = now;
                        report_map(find_gl_view(this));
                    }
                }
                if (eat) {
                    /* menu owns the whole gesture: ImGui already got
                       every event via gNativeTouch above; the app
                       never does - INCLUDING the UP/CANCEL that ends
                       it (r24). MOVE/UP keep flowing to ImGui so
                       drags and button releases work. */
                    return true;                     /* consume */
                }
            } catch (e) {}
            return this.dispatchTouchEvent(ev);         /* original call */
        };
        agentLog('phase: dispatchTouchEvent hook ok');
        sendMsg('info', { msg: 'touch hook installed (r24, screen coords + hit-test click-through + UP-leak fix + IME activity capture + map refresh on DOWN)' });
    });
}

/* ---------------- class info ---------------- */
/* (r23: field_lines() removed with the "n:t=v;n=v" instance protocol;
   instances now use the structured field_rows() below) */
function method_lines(jc) {
    var out = [];
    var ms = jc.class.getDeclaredMethods();
    for (var i = 0; i < ms.length; i++) {
        var m = ms[i];
        var ps = m.getParameterTypes(), sig = [];
        for (var k = 0; k < ps.length; k++) sig.push(ps[k].getName());
        out.push(m.getName() + '(' + sig.join(',') + ') -> ' +
                m.getReturnType().getName());
    }
    return out;
}
/* r12: names+types only - NO value reads. f.get(null) can trigger
   the class's <clinit>, which may block the agent thread forever
   (the "open a class and the app freezes" bug). Static values are
   read on demand via readStaticFields. */
function field_lines_light(jc) {
    var out = [];
    var fs = jc.class.getDeclaredFields();
    for (var i = 0; i < fs.length; i++) {
        var f = fs[i];
        try {
            f.setAccessible(true);
            var st = (f.getModifiers() & 8) !== 0;
            out.push(f.getName() + ':' + f.getType().getName() + '=' +
                     (st ? '(static)' : '(instance)'));
        } catch (e) { try { out.push(f.getName() + ':?'); } catch (e2) {} }
    }
    return out;
}

/* r23: structured instance fields - values through field_repr()
   (toString-safe); replaces the mangled "n:t=v;n:t=v" protocol whose
   delimiters corrupted values containing | # or ; */
function field_rows(jc, inst) {
    var out = [];
    try {
        var fs = jc.class.getDeclaredFields();
        for (var i = 0; i < fs.length; i++) {
            var f = fs[i];
            var nm = '?';
            try { nm = String(f.getName()); } catch (e0) {}
            try {
                f.setAccessible(true);
                out.push({ n: nm,
                           t: String(f.getType().getName()),
                           v: field_repr(f.get(inst)) });
            } catch (e1) { out.push({ n: nm, t: '?', v: '?' }); }
        }
    } catch (e2) {}
    return out;
}

/* ---------------- recorder ----------------
   r11: scoped. Only classes matching the package filter are hooked and
   installation stops at cap methods; the r10 version hooked every
   method of every class in the process (tens of thousands of ArtMethod
   hook slots on a 32-bit process) and died with SIGABRT mid-install. */
var REC = { on: false, busy: false, ring: [], installed: 0, flushBusy: false,
            lastProg: 0 };
function rec_wanted(name, filter) {
    if (filter === '.') return true;                 /* everything */
    if (!filter) return true;                        /* agent default */
    return name.indexOf(filter) === 0;
}
function rec_flush() {
    if (REC.flushBusy || !REC.ring.length) return;
    REC.flushBusy = true;
    var items = REC.ring.splice(0, REC.ring.length);
    REC.flushBusy = false;
    sendMsg('recBatch', { items: items.map(function (e) {
        return e.cls + '.' + e.m + '|' + (e.a || '') + '|' + (e.r || '') +
               '|' + (e.x || '');
    }) });
}
setInterval(rec_flush, 500);

function rec_install_chunk(list, batch) {
    /* r12: Stop must also abort a still-running installation (the
       r11 Stop left the loop hooking classes, so the Recorder tab
       looked stuck on "installing ...").
       r23: every method goes through the layering registry, so
       stopRecorder can remove all recorder layers again. */
    if (!REC.on) {
        REC.busy = false;
        sendMsg('recorderStopped', { installed: REC.installed });
        return;
    }
    for (var i = 0; i < batch && REC.idx < list.length; i++, REC.idx++) {
        if (REC.installed >= REC.cap) break;
        var cls = list[REC.idx];
        try {
            var jc = Java.use(cls);
            var ms = jc.class.getDeclaredMethods();
            var seen = {};
            for (var k = 0; k < ms.length; k++) {
                if (REC.installed >= REC.cap) break;
                var name;
                try { name = String(ms[k].getName()); } catch (eN) { continue; }
                if (seen[name]) continue;
                seen[name] = 1;
                try {
                    REC.installed += install_layer(cls, name, 'rec', {});
                } catch (e0) {}
            }
        } catch (e3) {}
    }
    /* r12: throttle progress - the r11 log flooded ~200
       recorderProgress messages for a single install */
    var now = Date.now();
    if (now - REC.lastProg > 250) {
        REC.lastProg = now;
        sendMsg('recorderProgress', {
            installed: REC.installed, total: list.length - REC.idx });
    }
    if (REC.idx < list.length && REC.installed < REC.cap) {
        setTimeout(function () { rec_install_chunk(list, batch); }, 25);
    } else {
        REC.busy = false;
        sendMsg('recorderProgress', { installed: REC.installed, total: 0 });
        sendMsg('recorderStarted', { installed: REC.installed });
    }
}

/* ---------------- monitor ---------------- */
var MON = { on: false, ring: [], origins: {}, okhttp: null };
function mon_push(kind, url, method, origin, note) {
    if (!MON.on) return;
    if (MON.ring.length > 600) MON.ring.shift();
    MON.ring.push({
        k: kind, u: (url || '').substring(0, 120),
        m: method || '', o: origin || '', n: (note || '').substring(0, 120)
    });
}
function mon_origin(site) {
    if (!site) return '';
    if (MON.origins[site] !== undefined) return MON.origins[site];
    var top = '';
    try {
        var st = Java.use('java.lang.Thread').currentThread().getStackTrace();
        for (var i = 0; i < st.length; i++) {
            var f = st[i].toString().split('(')[0];
            if (f.indexOf('java.') === 0 || f.indexOf('javax.') === 0 ||
                f.indexOf('android.') === 0 || f.indexOf('okhttp3.') === 0 ||
                f.indexOf('okio.') === 0 || f.indexOf('dalvik.') === 0) continue;
            top = f; break;
        }
    } catch (e) {}
    MON.origins[site] = top;
    return top;
}
function mon_flush() {
    if (!MON.ring.length) return;
    var items = MON.ring.splice(0, MON.ring.length);
    sendMsg('monitorEvent', { items: items.map(function (e) {
        return e.k + '|' + e.u + '|' + e.m + '|' + e.o + '|' + e.n;
    }) });
}
setInterval(function () { if (MON.on) mon_flush(); }, 600);

function mon_install() {
    Java.perform(function () {
        /* r23: monitor hooks go through the layering registry, so
           stopMonitor removes them again (r22 left them installed
           forever - the audit's "stopMonitor doesn't unhook") */
        var okhttpOk = false;
        try {
            if (dt_is_loaded('okhttp3.RealCall')) {
                install_layer('okhttp3.RealCall', 'execute', 'mon', {
                    fn: function (self) {
                        try {
                            var req = self.request();
                            var url = String(req.url());
                            mon_push('http', url, String(req.method()),
                                     mon_origin('okhttp:' + url),
                                     'headers ' + req.headers().size());
                        } catch (e) {}
                    }
                });
                install_layer('okhttp3.RealCall', 'enqueue', 'mon', {
                    fn: function (self) {
                        try {
                            var req = self.request();
                            var url = String(req.url());
                            mon_push('http', url, String(req.method()),
                                     mon_origin('okhttp:' + url),
                                     'async, headers ' + req.headers().size());
                        } catch (e) {}
                    }
                });
                okhttpOk = true;
            }
        } catch (e) { okhttpOk = false; }

        /* HttpURLConnection: URL at connect time */
        try {
            install_layer('java.net.URL', 'openConnection', 'mon', {
                fn: function (self) {
                    try {
                        mon_push('urlconn', String(self.toString()), 'open',
                                 mon_origin('url:' + self.toString()), '');
                    } catch (e) {}
                }
            });
        } catch (e) {}

        /* SSL: best-effort note only; full plaintext would need
           SSL_read/SSL_write native hooks */
        try {
            Java.use('javax.net.ssl.SSLSocket');
            mon_push('ssl', 'engine', 'note',
                     '', 'SSLSocket reachable');
        } catch (e) {}

        sendMsg('monitorInfo', {
            note: 'okhttp ' + (okhttpOk ? 'hooked' : 'absent') +
                   ', urlconn hooked'
        });
    });
}

/* ---------------- helpers for exports ---------------- */
/* (r23: use_cls_parts() removed - layer installs resolve the member
   themselves) */

function call_with_args(ov, argStrings, isCtor, recv) {
    /* map the text args onto the overload's parameter types.
       r23: `recv` is the retained-instance receiver for instance
       methods (the old call was ov.apply(null, ...) - no receiver,
       so instance methods could never be invoked). */
    var params = ov.argumentTypes;
    var real = [];
    for (var i = 0; i < params.length; i++) {
        var t = params[i].name, s = argStrings[i];
        if (s === undefined || s === null || s === '') {
            if (t === 'int' || t === 'short' || t === 'byte' ||
                t === 'long' || t === 'float' || t === 'double') { real.push(0); continue; }
            if (t === 'boolean') { real.push(false); continue; }
            if (t === 'char') { real.push('\u0000'); continue; }
            real.push(null); continue;
        }
        if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
            real.push(parseInt(s, 10) || 0);
        else if (t === 'float' || t === 'double')
            real.push(parseFloat(s) || 0.0);
        else if (t === 'boolean') real.push(s === 'true');
        else if (t === 'char') real.push(s.length ? s.charCodeAt(0) : '\u0000');
        else if (t.lastIndexOf('[') === 0) real.push(null);
        else real.push(s);
    }
    return ov.apply(recv || null, real);
}

var gModCls = null;
function is_static_method(jc, method) {
    try {
        var ms = jc.class.getDeclaredMethods();
        for (var i = 0; i < ms.length; i++) {
            if (String(ms[i].getName()) !== method) continue;
            try {
                if (!gModCls) gModCls = Java.use('java.lang.reflect.Modifier');
                if (gModCls.isStatic(ms[i].getModifiers())) return true;
            } catch (e0) {}
        }
    } catch (e1) {}
    return false;
}

/* r23: recursion-safe args renderer for tracker events (field_repr
   per value instead of String(v) - see the note above field_repr) */
function describe_safe(a) {
    try {
        var s = '';
        for (var i = 0; i < a.length && i < 8; i++) {
            var d = field_repr(a[i]);
            if (d.length > 60) d = d.substring(0, 57) + '...';
            s += (i ? ',' : '') + d;
        }
        if (s.length > 240) s = s.substring(0, 237) + '...';
        return s;
    } catch (e) { return '?'; }
}

/* r23: retained-instance registry (the audit's "Instance-method
   Call": callMethod/setInstanceField had NO receiver - instances
   were addressed by a bare scan index that died when the scan
   returned). Handles are Java.retain()ed global references; the
   table is capped and evicts oldest-first. */
var gInstHandles = {};     /* id -> retained wrapper */
var INST_ID = 0, INST_GEN = 0;
var INST_LIMIT = 200;
function inst_put(inst) {
    var id = ++INST_ID;
    try {
        gInstHandles[id] = Java.retain(inst);
        var keys = Object.keys(gInstHandles);
        if (keys.length > INST_LIMIT) {
            keys.sort(function (a, b) { return (a | 0) - (b | 0); });
            while (keys.length > INST_LIMIT) {
                var old = keys.shift();
                try { if (gInstHandles[old]) ime_release_act(gInstHandles[old]); }
                catch (e0) {}
                delete gInstHandles[old];
            }
        }
        return id;
    } catch (e) { return -1; }
}
function inst_get(handle) {
    if (typeof handle !== 'number' || handle <= 0) return null;
    return gInstHandles[handle] || null;
}

function set_field_value(f, recv, value) {
    var t = String(f.getType().getName());
    if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
        f.set(recv, parseInt(value, 10) || 0);
    else if (t === 'boolean') f.set(recv, value === 'true');
    else if (t === 'float' || t === 'double')
        f.set(recv, parseFloat(value) || 0.0);
    else if (t === 'char')
        f.set(recv, (value || 'a').charCodeAt(0));
    else f.set(recv, value);
}

/* ---------------- unified hook layering registry (r23) ----------------
   Hook, Tracker, field-Tracker, Recorder and Monitor used to assign
   ov.implementation INDEPENDENTLY - the last install silently
   replaced every previous one (audit: "Hook, Tracker, and Recorder
   overwrite each other's ov.implementation"). Every layer of a
   class+method now lives in ONE registry entry; a single dispatcher
   per overload runs all active layers around exactly ONE call into
   the original. Removing the last layer of a method restores the
   original implementation - which is what makes Recorder/Monitor
   Stop really unhook this time. */
var LAYERS = {};   /* 'cls!method' -> { ovs: [overloads], parts: {kind: state} } */

function noop_ret(ov) {
    var rt = ov.returnType.name;
    if (rt === 'int' || rt === 'long' || rt === 'short' || rt === 'byte')
        return 0;
    if (rt === 'float' || rt === 'double') return 0;
    if (rt === 'boolean') return false;
    if (rt === 'char') return '\u0000';
    return null;
}

/* r24: L comes in from the hook closure (no registry lookup per
   call) and cls/mn are parsed from K LAZILY - only recorder rows
   need them, the hot path does not (the old version substring'd
   the key on EVERY hooked call of every class) */
function dispatch_layers(K, L, ov, self, args) {
    var p = L ? L.parts : {};
    var hk = p.hook;
    var out;
    try {
        if (hk && hk.on && hk.mode === 'noop') {
            out = noop_ret(ov);
        } else if (hk && hk.on) {
            /* r22 semantics kept: the user fn gets receiver/args/orig();
               a throw falls back to the original call */
            try {
                out = hk.fn(Java, send, CONSOLE(), self,
                            Array.prototype.slice.call(args),
                            function () {
                                return ov.apply(self,
                                    arguments.length ? arguments : args);
                            });
            } catch (e1) {
                sendMsg('error', { fn: 'hook:' + K,
                                   msg: String(e1.message || e1) });
                out = ov.apply(self, args);
            }
            /* smali hooks return null to mean "default value" (r22) */
            if (hk.mode === 'Smali' && out === null)
                out = noop_ret(ov);
        } else {
            out = ov.apply(self, args);
        }
    } catch (e2) {
        /* recorder row for the failing call, then rethrow (r22) */
        try {
            if (p.rec && REC.on && REC.ring.length < 4000) {
                var i = K.indexOf('!');   /* r24: lazy - rows only */
                REC.ring.push({ cls: K.substring(0, i),
                                m: K.substring(i + 1), a: jargs(args),
                                r: '', x: String(e2).substring(0, 80) });
            }
        } catch (e3) {}
        throw e2;
    }
    /* observer layers see the FINAL result; each is isolated */
    try {
        if (p.rec && REC.on && REC.ring.length < 4000) {
            var j = K.indexOf('!');   /* r24: lazy - rows only */
            REC.ring.push({ cls: K.substring(0, j),
                            m: K.substring(j + 1), a: jargs(args),
                            r: jstr(out), x: '' });
        }
    } catch (e4) {}
    try {
        if (p.mon && MON.on) p.mon.fn(self, args);
    } catch (e5) {}
    try {
        if (p.trk) {
            var tk = trackers[p.trk.key];
            if (tk && tk.on) {
                sendMsg('trackerEvent', {
                    name: p.trk.key,
                    callers: jstack(5).filter(function (c) {
                        return c !== p.trk.key;
                    }).slice(0, 4),
                    args: describe_safe(args),
                    ret: field_repr(out), err: ''
                });
            }
        }
    } catch (e6) {}
    try {
        if (p.fld) field_layer_emit(p.fld, self);
    } catch (e7) {}
    return out;
}

/* field-tracker emission - r23: values through field_repr(), never
   the app's toString() (the audit's Tracker Side Effects item).
   r24 PERF FIX: the old else-branch sent a trackerEvent on EVERY
   method call of the class even when the value had NOT changed -
   on a hot class that flooded the bridge and froze the menu
   (logcat: "Skipped 40 frames!"). Now: the field is read at most
   every 20 ms, and an event is emitted only when the value
   ACTUALLY changed and at most once per 100 ms per field. While
   throttled, st.last is kept, so the next allowed emission shows
   the full old -> new transition. */
function field_layer_emit(st, self) {
    var tk = trackers[st.key];
    if (!tk || !tk.on) return;
    var now = Date.now();
    if (st.nextRead && now < st.nextRead) return;
    st.nextRead = now + 20;
    var v = st.stat ? field_repr(st.f.get(null))
                    : (self ? field_repr(st.f.get(self)) : '?');
    if (v === st.last) return;
    if (st.nextEmit && now < st.nextEmit) return;
    sendMsg('trackerEvent', { name: st.key, callers: jstack(3),
                              args: st.last + ' -> ' + v,
                              ret: '', err: '' });
    st.last = v;
    st.nextEmit = now + 100;
}

/* returns the number of overloads carrying this method's layers */
function install_layer(cls, mn, kind, state) {
    var K = cls + '!' + mn;
    var L = LAYERS[K];
    if (!L) {
        var jc = Java.use(cls);   /* caller decides the <clinit> policy */
        var node = null;
        try { node = jc[mn]; } catch (eN) {}
        var ovs = node && node.overloads;
        if (!ovs || !ovs.length) return 0;
        L = { ovs: ovs, parts: {} };
        LAYERS[K] = L;
        ovs.forEach(function (ov) {
            ov.implementation = function () {
                /* r24: L closed over - no registry lookup per call */
                return dispatch_layers(K, L, ov, this, arguments);
            };
        });
    }
    L.parts[kind] = state || {};
    return L.ovs.length;
}

function uninstall_layer(cls, mn, kind) {
    var K = cls + '!' + mn;
    var L = LAYERS[K];
    if (!L) return;
    delete L.parts[kind];
    if (!Object.keys(L.parts).length) {
        try {
            L.ovs.forEach(function (ov) { ov.implementation = null; });
        } catch (e) {}
        delete LAYERS[K];
    }
}

function uninstall_all(kind) {
    Object.keys(LAYERS).forEach(function (K) {
        var i = K.indexOf('!');
        uninstall_layer(K.substring(0, i), K.substring(i + 1), kind);
    });
}

/* ---------------- class access without premature <clinit> (r23) ------
   frida-java-bridge runs ensureClassInitialized() on EVERY fresh
   Java.use(), so hooking a class the app has not initialized yet
   force-runs its static initializer (audit: "Class Initialization").
   The bridge exposes no non-initializing path, so the agent only
   Java.use()s classes that are ALREADY LOADED and defers everything
   else: a one-time loadClass watcher installs the queued install the
   moment the target app loads the class.

   KNOWN LIMITATION (r24, documented instead of papered over): the
   deferral only avoids classes that are NOT LOADED YET. A class that
   is loaded but never initialized still gets its <clinit> forced by
   Java.use() - frida-java-bridge always calls
   ensureClassInitialized() and offers no non-initializing use().
   There is no fix on this bridge; apps whose <clinit> misbehaves
   when probed will still be affected. */
var gLoadedSet = null, gLoadedAt = 0, gLoadedRechecked = false;
function dt_rebuild_loaded_set() {
    try {
        var all = Java.enumerateLoadedClassesSync();
        var set = {};
        for (var i = 0; i < all.length; i++) set[all[i]] = 1;
        gLoadedSet = set;
        gLoadedAt = Date.now();
    } catch (e0) {}
}
function dt_is_loaded(cls) {
    var now = Date.now();
    /* r25 PERF FIX: enumerateLoadedClassesSync() suspends every
       thread in the process to walk the class table safely -
       cheap on the sample app, a noticeable stutter on a real
       app's much bigger table. A 3 s TTL re-paid that cost on
       every miss while adding a few hooks/trackers in a row;
       20 s still catches a newly-loaded class well within the
       loadClass watcher's own window, just less often. */
    if (!gLoadedSet || now - gLoadedAt > 20000) {
        dt_rebuild_loaded_set();
        gLoadedRechecked = false;   /* new cache generation */
    }
    if (gLoadedSet && gLoadedSet[cls]) return true;
    /* r24 RACE FIX: the 3 s cache could predate the class actually
       loading - and loadClass never fires again for an already-
       loaded class - so the queued install waited forever (the
       audit's "deferred forever"). On a miss, force ONE rebuild of
       the set (once per cache generation, so a burst of misses does
       not re-enumerate per class) before declaring it unloaded. */
    if (!gLoadedRechecked) {
        gLoadedRechecked = true;
        dt_rebuild_loaded_set();
    }
    return !!(gLoadedSet && gLoadedSet[cls]);
}

function use_loaded(cls) {
    if (!dt_is_loaded(cls))
        throw new Error('class not loaded yet: ' + cls +
                        ' (its <clinit> will not be forced)');
    return Java.use(cls);
}

var gDeferred = [];        /* queued installs: {kind, cls, fn} */
var gWatcherOvs = null;    /* loadClass overloads we hooked, for restore */
function defer_install(kind, cls, fn) {
    gDeferred.push({ kind: kind, cls: cls, fn: fn });
    if (!gWatcherOvs) install_load_watcher();
    sendMsg('info', { msg: 'class ' + cls + ' not loaded yet - ' + kind +
                           ' deferred until it loads' });
}
function install_load_watcher() {
    Java.perform(function () {
        try {
            var CL = Java.use('java.lang.ClassLoader');
            var ovs = [];
            CL.loadClass.overloads.forEach(function (ov) {
                if (ov.argumentTypes.length !== 1) return;   /* name only */
                ov.implementation = function (name) {
                    var c = ov.apply(this, [name]);
                    try { pump_deferred(String(name)); } catch (e0) {}
                    return c;
                };
                ovs.push(ov);
            });
            gWatcherOvs = ovs;
            agentLog('phase: loadClass watcher armed (' +
                     ovs.length + ' overloads)');
        } catch (e) {
            gWatcherOvs = null;
            sendMsg('warn', { msg: 'loadClass watcher failed: ' +
                                  String(e.message || e) });
        }
    });
}
function stop_load_watcher() {
    if (!gWatcherOvs) return;
    try {
        gWatcherOvs.forEach(function (ov) { ov.implementation = null; });
    } catch (e0) {}
    gWatcherOvs = null;
    agentLog('phase: loadClass watcher removed (queue empty)');
}
function pump_deferred(loadedName) {
    if (!gDeferred.length) { stop_load_watcher(); return; }
    /* r23: entries are removed from the queue BEFORE they run - the
       install itself calls Java.use(), which goes through the
       loadClass hook again, and a re-entrant pump must never re-run
       the same install (infinite recursion). */
    var run = [], stay = [];
    for (var i = 0; i < gDeferred.length; i++) {
        (gDeferred[i].cls === loadedName ? run : stay).push(gDeferred[i]);
    }
    gDeferred = stay;
    if (!gDeferred.length) stop_load_watcher();
    for (var j = 0; j < run.length; j++) {
        try { run[j].fn(); } catch (e1) {
            sendMsg('error', { fn: 'deferred:' + run[j].kind,
                               msg: String(e1.message || e1) });
        }
    }
}

var patches = {}, hooks = {}, trackers = {};

/* r23: hook/tracker install bodies, factored out so the deferred
   loader can run them when the target class finally loads */
function do_addHook(cls, method, mode, code) {
    var key = cls + '.' + method;
    var jc = Java.use(cls);
    var node = null;
    try { node = jc[method]; } catch (eN) {}
    if (!node || !node.overloads || !node.overloads.length) {
        sendMsg('hookFailed', { name: key, error: 'class or method not found' });
        return;
    }
    if (mode === 'Java') {
        var fn = new Function('Java', 'send', 'console', 'thiz',
                              'args', 'orig', code);
        var st = { mode: mode, on: true, code: code, fn: fn };
        if (!install_layer(cls, method, 'hook', st)) {
            sendMsg('hookFailed', { name: key, error: 'class or method not found' });
            return;
        }
        hooks[key] = st;
        sendMsg('hookAdded', { name: key });
    } else if (mode === 'noop') {
        var st2 = { mode: mode, on: true, code: code, fn: null };
        if (!install_layer(cls, method, 'hook', st2)) {
            sendMsg('hookFailed', { name: key, error: 'class or method not found' });
            return;
        }
        hooks[key] = st2;
        sendMsg('hookAdded', { name: key });
    } else if (mode === 'Smali') {
        var gen = 'H' + (Date.now() % 1000000);
        var skel = '.class public final Lcom/dextool/hook/' + gen + ';\n' +
                   '.super Ljava/lang/Object;\n' +
                   '.method public static hook(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;\n' +
                   /* r22/audit: default .locals - user code without a
                      registers directive died in the assembler */
                   (/^\s*\.(locals|registers)\b/m.test(code) ? '' : '    .locals 4\n') +
                   code + '\n' +
                   '.end method\n';
        var art = smali_compile(skel);
        if (art.err) {
            sendMsg('hookFailed', { name: key, error: art.err });
            return;
        }
        art.cls = 'com.dextool.hook.' + gen;
        var loaded = smali_load(art);
        if (!loaded.ok) {
            sendMsg('hookFailed', { name: key, error: loaded.err });
            return;
        }
        var st3 = { mode: mode, on: true, code: code,
                    fn: function (J, S, C, thiz, a) {
                        var arr = Java.array('java.lang.Object', a);
                        return loaded.hook(thiz, arr);
                    } };
        if (!install_layer(cls, method, 'hook', st3)) {
            sendMsg('hookFailed', { name: key, error: 'class or method not found' });
            return;
        }
        hooks[key] = st3;
        sendMsg('smaliReady', { name: key, ok: 1, error: '' });
        sendMsg('hookAdded', { name: key });
    }
}

function do_addTracker(cls, method) {
    var key = cls + '.' + method;
    if (!install_layer(cls, method, 'trk', { key: key }))
        throw new Error('method not found: ' + method);
    trackers[key] = { on: true };
    sendMsg('info', { msg: 'tracker added ' + key });
}

function do_trackField(cls, field) {
    var key = cls + '.' + field;
    var jc = Java.use(cls);
    var f = jc.class.getDeclaredField(field);
    f.setAccessible(true);
    var stat = (f.getModifiers() & 8) !== 0;
    var last = stat ? field_repr(f.get(null)) : '(instance)';
    var ms = jc.class.getDeclaredMethods();
    var installed = 0;
    for (var i = 0; i < ms.length; i++) {
        var mn;
        try { mn = String(ms[i].getName()); } catch (e0) { continue; }
        try {
            installed += install_layer(cls, mn, 'fld',
                                       { key: key, f: f, stat: stat,
                                         last: last });
        } catch (e1) {}
    }
    if (!installed) throw new Error('no hookable methods on ' + cls);
    trackers[key] = { on: true };
    sendMsg('info', { msg: 'field tracker added ' + key +
                           ' (approximation, toString-safe)' });
}

/* r22/audit: static-field patch helpers - the ORIGINAL value is
   captured before the first write so OFF / delete can put it back */
function patch_store(f, t, value) {
    if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
        f.set(null, parseInt(value, 10) || 0);
    else if (t === 'boolean') f.set(null, value === 'true');
    else if (t === 'float' || t === 'double')
        f.set(null, parseFloat(value) || 0.0);
    else if (t === 'char') f.set(null, (value || 'a').charCodeAt(0));
    else f.set(null, value);
}
function patch_snapshot(f, t) {
    var v = f.get(null);
    if (v === null || v === undefined) return { kind: 'null' };
    if (t === 'int' || t === 'long' || t === 'short' || t === 'byte' ||
        t === 'boolean' || t === 'float' || t === 'double' || t === 'char' ||
        t === 'java.lang.String')
        return { kind: 'val', s: String(v) };
    return { kind: 'ref', h: Java.retain(v) };
}
function patch_restore(cls, field, key) {
    var p = patches[key];
    if (!p || !p.snap) return;
    Java.perform(function () {
        try {
            var f = Java.use(cls).class.getDeclaredField(field);
            f.setAccessible(true);
            var t = String(f.getType().getName());
            var sn = p.snap;
            if (sn.kind === 'null') f.set(null, null);
            else if (sn.kind === 'ref') f.set(null, sn.h);
            else patch_store(f, t, sn.s);
        } catch (e) {
            sendMsg('error', { fn: 'patchRestore', msg: String(e.message || e) });
        }
    });
}

/* ---------------- rpc exports (called from the menu) ---------------- */
rpc.exports = {
    /* ---- patches: static-field value replacement ---- */
    addPatch: function (cls, field, value) {
        var key = cls + '.' + field;
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                var f = jc.class.getDeclaredField(field);
                f.setAccessible(true);
                var t = String(f.getType().getName());
                var st = (f.getModifiers() & 8) !== 0;
                if (!st) throw new Error('not a static field (instances: Classes tab)');
                /* r22/audit: snapshot the ORIGINAL value once, before
                   the first write, so OFF/delete can restore it */
                var prev = patches[key];
                var snap = (prev && prev.snap) ? prev.snap : patch_snapshot(f, t);
                patch_store(f, t, value);
                patches[key] = { v: value, on: true, snap: snap };
                sendMsg('patchApplied', { name: key });
            } catch (e) {
                sendMsg('patchFailed', { name: key, error: String(e.message || e) });
            }
        });
    },
    setPatchEnabled: function (cls, field, en) {
        var key = cls + '.' + field;
        var p = patches[key];
        if (p) p.on = en;
        /* r22/audit: OFF now RESTORES the snapshotted original
           (the old identity-set was a no-op) */
        if (!en) patch_restore(cls, field, key);
        else rpc.exports.addPatch(cls, field, p ? p.v : '');
        sendMsg('info', { msg: 'patch ' + key + ' -> ' + (en ? 'on' : 'off') });
    },
    removePatch: function (cls, field) {
        var key = cls + '.' + field;
        patch_restore(cls, field, key);
        delete patches[key];
        sendMsg('info', { msg: 'patch removed ' + key });
    },

    /* ---- hooks: Java(JS) / Smali / noop ---- */
    addHook: function (cls, method, mode, code) {
        var key = cls + '.' + method;
        /* r22/audit: the menu labels are 'Java(JS)' / 'Smali' /
           'noop' - normalise them or Java hooks never install */
        var ml = String(mode).toLowerCase();
        if (ml.indexOf('java') === 0) mode = 'Java';
        else if (ml === 'smali') mode = 'Smali';
        else if (ml === 'noop') mode = 'noop';
        Java.perform(function () {
            /* r23: never force <clinit> - defer until the class loads */
            if (!dt_is_loaded(cls)) {
                defer_install('hook', cls, function () {
                    try { do_addHook(cls, method, mode, code); }
                    catch (eD) {
                        sendMsg('hookFailed', { name: key,
                                                error: String(eD.message || eD) });
                    }
                });
                return;
            }
            try { do_addHook(cls, method, mode, code); }
            catch (e0) {
                sendMsg('hookFailed', { name: key,
                                        error: String(e0.message || e0) });
            }
        });
    },
    setHookEnabled: function (cls, method, en) {
        var key = cls + '.' + method;
        /* r23: hooks[key] IS the LAYERS part object - flipping .on is
           enough, the dispatcher reads it live */
        if (hooks[key]) hooks[key].on = en;
        sendMsg('info', { msg: 'hook ' + key + ' -> ' + (en ? 'on' : 'off') });
    },
    removeHook: function (cls, method) {
        var key = cls + '.' + method;
        Java.perform(function () {
            /* r23: remove the layer - other layers on the same method
               (tracker/recorder) survive, and the original
               implementation returns when this was the last one */
            uninstall_layer(cls, method, 'hook');
            delete hooks[key];
            sendMsg('info', { msg: 'hook removed ' + key });
        });
    },

    /* ---- trackers ---- */
    addTracker: function (cls, method) {
        var key = cls + '.' + method;
        Java.perform(function () {
            try {
                if (!dt_is_loaded(cls)) {
                    defer_install('tracker', cls, function () {
                        try { do_addTracker(cls, method); }
                        catch (eD) {
                            sendMsg('error', { fn: 'addTracker',
                                               msg: String(eD.message || eD) });
                        }
                    });
                    return;
                }
                do_addTracker(cls, method);
            } catch (e) {
                sendMsg('error', { fn: 'addTracker',
                                   msg: String(e.message || e) });
            }
        });
    },
    trackField: function (cls, field) {
        /* ART has no field read/write barriers: approximate by
           watching the value before/after every declared method call
           of the class. r23: values go through field_repr() - the old
           jstr(f.get(...)) called the app's toString() INSIDE our
           hooks (recursion/crash when toString is itself recorded). */
        var key = cls + '.' + field;
        Java.perform(function () {
            try {
                if (!dt_is_loaded(cls)) {
                    defer_install('field tracker', cls, function () {
                        try { do_trackField(cls, field); }
                        catch (eD) {
                            sendMsg('error', { fn: 'trackField',
                                               msg: String(eD.message || eD) });
                        }
                    });
                    return;
                }
                do_trackField(cls, field);
            } catch (e) {
                sendMsg('error', { fn: 'trackField',
                                   msg: String(e.message || e) });
            }
        });
    },
    removeTracker: function (name) {
        Java.perform(function () {
            /* r23: the layer(s) are removed for real; the class's
               methods return to their original implementations when
               nothing else tracks/records/hooks them */
            Object.keys(LAYERS).forEach(function (K) {
                var L = LAYERS[K];
                if (!L) return;
                var i = K.indexOf('!');
                var cls = K.substring(0, i), mn = K.substring(i + 1);
                if (L.parts.trk && L.parts.trk.key === name)
                    uninstall_layer(cls, mn, 'trk');
                if (L.parts.fld && L.parts.fld.key === name)
                    uninstall_layer(cls, mn, 'fld');
            });
            delete trackers[name];
            sendMsg('info', { msg: 'tracker removed ' + name });
        });
    },

    /* ---- recorder ---- */
    startRecorder: function (filter, cap) {
        if (REC.on || REC.busy) return;
        Java.perform(function () {
            var pkg = '';
            try {
                var AT = Java.use('android.app.ActivityThread');
                var app = AT.currentApplication();
                if (app) pkg = String(app.getPackageName());
            } catch (e0) {}
            REC.filter = (filter === undefined || filter === null) ? '' :
                          String(filter);
            if (!REC.filter) REC.filter = pkg;   /* default: own package */
            /* '.' sent by the UI means everything; agent: use it directly */
            if (filter === '.') REC.filter = '.';
            REC.cap = (typeof cap === 'number' && cap > 0) ?
                      Math.floor(cap) : 1500;
            var list = Java.enumerateLoadedClassesSync();
            /* r22/audit: filter ONCE - the install loop counted
               skipped classes against its 12-class batch, so a
               package filter took ~a minute over 30k names */
            list = list.filter(function (n) { return rec_wanted(n, REC.filter); });
            REC.on = true;
            REC.busy = true;
            REC.ring = [];
            REC.installed = 0;
            REC.idx = 0;
            REC.lastProg = 0;
            sendMsg('recorderProgress', { installed: 0, total: list.length });
            sendMsg('info', { msg: 'recorder scope: filter="' + REC.filter +
                                   '" cap=' + REC.cap });
            /* r12: smaller chunks (12 classes) - a chunk of 25 with
               big classes stalled the JS thread for seconds */
            setTimeout(function () { rec_install_chunk(list, 12); }, 50);
        });
    },
    stopRecorder: function () {
        /* r12: REC.on=false also aborts a still-running install
           (see rec_install_chunk).
           r23: uninstall_all('rec') removes the recorder layer from
           every method - hooks are GONE, not just silent (the audit's
           "hooks are never removed on stop"). */
        REC.on = false;
        REC.busy = false;
        Java.perform(function () { uninstall_all('rec'); });
        rec_flush();
        sendMsg('recorderStopped', { installed: REC.installed });
    },

    /* ---- monitor ---- */
    startMonitor: function () {
        if (MON.on) return;
        MON.on = true;
        MON.ring = [];
        mon_install();
        sendMsg('monitorStarted', { note: '' });
    },
    stopMonitor: function () {
        MON.on = false;
        mon_flush();
        Java.perform(function () {
            uninstall_all('mon');    /* r23: really unhook */
        });
        sendMsg('monitorStopped', {});
    },

    /* ---- script console ---- */
    evalScript: function (src) {
        try {
            /* r22/audit: the LAST argument of new Function() is the
               BODY - the old call made "src" the body (a parameter
               name) and passed the user's code as a parameter VALUE,
               so the Script tab ran nothing */
            var r = (new Function('Java', 'send', 'console', String(src)))(
                        Java, send, CONSOLE());
            if (r !== undefined) sendMsg('scriptResult', { result: cstr(r) });
        } catch (e) {
            sendMsg('scriptError', { msg: String(e && e.stack ? e.stack : e) });
        }
    },

    /* ---- classes ---- */
    listLoadedClasses: function (filter, qid) {
        Java.perform(function () {
            try {
                var all = Java.enumerateLoadedClassesSync();
                var f = (filter || '').toLowerCase();
                var out = [];
                for (var i = 0; i < all.length; i++) {
                    if (!f || all[i].toLowerCase().indexOf(f) !== -1)
                        out.push(all[i]);
                }
                /* r11: stream the FULL result in pages of 400. The r10
                   version sent one message capped at 800 names, so the
                   Classes tab never displayed all classes. */
                var PAGE = 400;
                var pages = Math.max(1, Math.ceil(out.length / PAGE));
                var q = (typeof qid === 'number') ? qid : 0;
                var idx = 0;
                var send_page = function () {
                    var chunk = out.slice(idx, idx + PAGE);
                    idx += PAGE;
                    sendMsg('classList', {
                        qid: q, page: Math.floor((idx - PAGE) / PAGE),
                        pages: pages, total: all.length,
                        shown: Math.min(idx, out.length),
                        classes: chunk
                    });
                    if (idx < out.length)
                        setTimeout(send_page, 15);
                };
                send_page();
            } catch (e) {
                sendMsg('error', { fn: 'listLoadedClasses',
                                   msg: String(e.message || e) });
            }
        });
    },
    openClass: function (cls) {
        Java.perform(function () {
            try {
                /* r12: field VALUES are no longer read here - a static
                   read can run the class's <clinit> and block forever.
                   r23: use_loaded() also refuses UNLOADED classes -
                   Java.use() itself runs <clinit> through the bridge's
                   ensureClassInitialized. */
                var jc = use_loaded(cls);
                sendMsg('classInfo', {
                    class: cls,
                    methods: method_lines(jc),
                    fields: field_lines_light(jc)
                });
            } catch (e) {
                sendMsg('classInfo', { class: cls, methods: [], fields: [] });
                sendMsg('error', { fn: 'openClass', msg: String(e.message || e) });
            }
        });
    },
    /* r12: on-demand static values for the open class (Fields tab
       "Read static values" button) */
    readStaticFields: function (cls) {
        Java.perform(function () {
            try {
                var jc = use_loaded(cls);   /* r23: no forced <clinit> */
                var fs = jc.class.getDeclaredFields();
                var out = [];
                for (var i = 0; i < fs.length; i++) {
                    var f = fs[i];
                    try {
                        f.setAccessible(true);
                        if ((f.getModifiers() & 8) === 0) continue;
                        out.push(f.getName() + ':' +
                                 f.getType().getName() + '=' +
                                 jstr(f.get(null)));
                    } catch (e1) {}
                }
                sendMsg('staticValues', { class: cls, fields: out });
            } catch (e) {
                sendMsg('error', { fn: 'readStaticFields',
                                   msg: String(e.message || e) });
            }
        });
    },
    /* ---- system keyboard (r12) ---- */
    showIme: function () { ime_show(); },
    hideIme: function () { ime_hide(); },
    listInstances: function (cls) {
        Java.perform(function () {
            var rows = [];
            var gen = ++INST_GEN;
            try {
                var jc = Java.use(cls);
                Java.choose(cls, {
                    onMatch: function (inst) {
                        try {
                            /* r23: retain + structured fields; the old
                               "##"/";" protocol corrupted values and
                               the bare scan index gave later calls no
                               receiver at all. The label is NOT run
                               through jstr() - its | -> ! and # -> %
                               rewrites were only needed by the old
                               delimiter protocol. */
                            var h = inst_put(inst);
                            var lbl = '';
                            try { lbl = String(inst); }
                            catch (eL) { lbl = '(toString failed)'; }
                            if (lbl.length > 160)
                                lbl = lbl.substring(0, 157) + '...';
                            rows.push({ str: lbl, handle: h,
                                        fields: field_rows(jc, inst) });
                        } catch (e0) {}
                        if (rows.length >= 20) return 'stop';
                    },
                    onComplete: function () {
                        sendMsg('instanceList', { class: cls, gen: gen,
                                                  instances: rows });
                    }
                });
            } catch (e) {
                sendMsg('instanceList', { class: cls, gen: gen,
                                          instances: rows });
            }
        });
    },
    setInstanceField: function (cls, index, field, value, handle) {
        Java.perform(function () {
            var key = cls + '.' + field;
            /* r23: a retained handle addresses the instance directly
               (the scan-index path is only the fallback now) */
            var recv = inst_get(handle);
            if (recv) {
                try {
                    var jc = Java.use(cls);
                    var f = jc.class.getDeclaredField(field);
                    f.setAccessible(true);
                    set_field_value(f, recv, value);
                    sendMsg('patchApplied', { name: key });
                } catch (e1) {
                    sendMsg('patchFailed', { name: key,
                                             error: String(e1.message || e1) });
                }
                return;
            }
            try {
                var found = [];
                Java.choose(cls, {
                    onMatch: function (inst) {
                        found.push(inst);
                        if (found.length > index + 1) return 'stop';
                    },
                    onComplete: function () {
                        try {
                            var inst = found[index];
                            if (!inst) throw new Error('no instance #' + index);
                            var jc = Java.use(cls);
                            var f = jc.class.getDeclaredField(field);
                            f.setAccessible(true);
                            set_field_value(f, inst, value);
                            sendMsg('patchApplied', { name: key });
                        } catch (e2) {
                            sendMsg('patchFailed', { name: key,
                                                     error: String(e2.message || e2) });
                        }
                    }
                });
            } catch (e) {}
        });
    },
    callMethod: function (cls, method, args, handle) {
        Java.perform(function () {
            try {
                /* r23: use_loaded - no forced <clinit> on unloaded
                   classes */
                var jc = use_loaded(cls);
                var recv = inst_get(handle);
                var node = null;
                try { node = jc[method]; } catch (e0) {}
                if (!node || !node.overloads || !node.overloads.length)
                    throw new Error('method not found: ' + method);
                var ovs = node.overloads;
                var want = args ? args.length : 0;
                var best = null;
                for (var i = 0; i < ovs.length; i++) {
                    if (ovs[i].argumentTypes.length === want) {
                        best = ovs[i];
                        break;
                    }
                }
                if (!best) best = ovs[0];
                if (recv) {
                    /* r23: instance-method call WITH a receiver (the
                       audit's "callMethod fails because it has no
                       receiver") */
                    var r = call_with_args(best, args || [], false, recv);
                    sendMsg('callResult', {
                        class: cls, ok: 1, result: jstr(r), error: ''
                    });
                    return;
                }
                if (!is_static_method(jc, method))
                    throw new Error('instance method - Find instances, ' +
                        'pick one, then Call (a retained handle is ' +
                        'required as the receiver)');
                var r2 = call_with_args(best, args || [], false, null);
                sendMsg('callResult', {
                    class: cls, ok: 1, result: jstr(r2), error: ''
                });
            } catch (e) {
                sendMsg('callResult', {
                    class: cls, ok: 0, result: '',
                    error: String(e.message || e)
                });
            }
        });
    }
};

/* console for user scripts. r22/audit: cstr() gives readable text
   (jstr() rewrites '|' and '#' and cuts at 160 chars for the
   delimiter protocol - wrong for a console line); the dead
   CONS_Lines/CONS_Dummy batching is gone. */
function cstr(v) {
    try {
        if (v === null) return 'null';
        if (v === undefined) return 'undefined';
        if (typeof v === 'string') return v.substring(0, 2000);
        if (v.$className) return String(v).substring(0, 2000);
        if (typeof v === 'object') return JSON.stringify(v).substring(0, 2000);
        return String(v).substring(0, 2000);
    } catch (e) { return '?'; }
}
function CONSOLE() {
    function line(tag, args) {
        var a = tag + Array.prototype.map.call(args, cstr).join(' ');
        sendMsg('console', { line: a });
    }
    return {
        log:   function () { line('', arguments); },
        error: function () { line('ERR ', arguments); },
        warn:  function () { line('WARN ', arguments); }
    };
}

/* ---------------- smali assemble + load (runtime dex) ---------------- */
function smali_compile(src) {
    var out = { err: '', dex: null };
    Java.perform(function () {
        try {
            var ctx = null;
            try {
                var AT = Java.use('android.app.ActivityThread');
                ctx = AT.currentApplication();
            } catch (e0) { ctx = null; }
            if (!ctx) { out.err = 'no app context'; return; }
            var files = ctx.getFilesDir().getAbsolutePath();
            var jar = files + '/smali-fat.jar';
            var File = Java.use('java.io.File');
            if (!File.$new(jar).exists()) {
                /* r22/audit: extract to a temp file + rename(2), so
                   a killed process cannot leave a truncated jar */
                var FOS0 = Java.use('java.io.FileOutputStream');
                var tmpJar = jar + '.tmp';
                var is = ctx.getAssets().open('smali-fat.jar');
                var fos = FOS0.$new(tmpJar);
                var buf = Java.array('byte', new Array(8192).fill(0));
                var n;
                while ((n = is.read(buf)) > 0) fos.write(buf, 0, n);
                fos.close(); is.close();
                File.$new(tmpJar).renameTo(File.$new(jar));
            }
            /* r22/audit: Android 14+ refuses to load a WRITABLE
               dex/jar (targetSdk 34) */
            File.$new(jar).setReadOnly();
            var PC = Java.use('dalvik.system.PathClassLoader');
            var CL = Java.use('java.lang.ClassLoader');
            var loader = PC.$new(jar, CL.getSystemClassLoader());
            /* r22/audit: loadClass() returns a java.lang.Class
               wrapper, which has no static main(): go through a
               ClassFactory bound to this loader */
            var F = Java.ClassFactory.get(loader);
            var tmp = files + '/dxt-' + (Date.now() % 1000000);
            var inF  = tmp + '.smali';
            var outF = tmp + '.dex';
            var FOS = Java.use('java.io.FileOutputStream');
            var fos2 = FOS.$new(inF);
            fos2.write(Java.use('java.lang.String').$new(src).getBytes('UTF-8'));
            fos2.close();
            var done = false, lastErr = '';
            var mains = ['com.android.tools.smali.smali.Main',
                         'com.android.tools.smali.Main'];
            for (var mi = 0; mi < mains.length && !done; mi++) {
                try {
                    var Main = F.use(mains[mi]);
                    Main.main(Java.array('java.lang.String',
                                         ['assemble', '-o', outF, inF]));
                    done = true;
                } catch (eM) { lastErr = String(eM.message || eM); }
            }
            if (!done) { out.err = 'smali Main not usable: ' + lastErr; }
            else if (!File.$new(outF).exists()) {
                out.err = 'smali produced no dex (see logcat for assembler errors)';
            } else {
                var FIS = Java.use('java.io.FileInputStream');
                var fis = FIS.$new(outF);
                var all = [], chunk = Java.array('byte', new Array(8192).fill(0));
                var k;
                while ((k = fis.read(chunk)) > 0) {
                    for (var c = 0; c < k; c++) all.push(chunk[c]);
                }
                fis.close();
                out.dex = Java.array('byte', all);
            }
            File.$new(inF).delete();
            File.$new(outF).delete();
        } catch (e) {
            out.err = String(e.message || e).substring(0, 300);
        }
    });
    return out;
}

function smali_load(art) {
    var r = { ok: false, err: '', hook: null };
    var dexBytes = art && art.dex;
    if (!dexBytes) { r.err = (art && art.err) || 'no dex'; return r; }
    Java.perform(function () {
        try {
            var ByteBuffer = Java.use('java.nio.ByteBuffer');
            var IMDC = Java.use('dalvik.system.InMemoryDexClassLoader');
            var CL = Java.use('java.lang.ClassLoader');
            var bb = ByteBuffer.wrap(dexBytes);
            /* r22/audit: single-buffer constructor - the ByteBuffer[]
               one needs API 27+, the sample's minSdk is 26 */
            var loader = IMDC.$new(bb, CL.getSystemClassLoader());
            var cls = loader.loadClass(art.cls);
            var methods = cls.getDeclaredMethods();
            for (var i = 0; i < methods.length; i++) {
                if (String(methods[i].getName()) === 'hook') {
                    methods[i].setAccessible(true);
                    var m = methods[i];
                    r.hook = function (thiz, args) {
                        return m.invoke(null, [thiz, args]);
                    };
                    r.ok = true;
                    return;
                }
            }
            r.err = 'hook method not found in dex';
        } catch (e) {
            r.err = String(e.message || e).substring(0, 300);
        }
    });
    return r;
}

/* ---------------- receive loop (menu -> agent) ---------------- */
recv(function onMsg(msg) {
    if (!msg) { recv(onMsg); return; }
    try {
        if (msg.type === 'rpc' && msg.fn && rpc.exports[msg.fn]) {
            var a = msg.args || {};
            var names = FN_ARGS[msg.fn] || [];
            var vals = names.map(function (n) { return a[n]; });
            rpc.exports[msg.fn].apply(rpc.exports, vals);
        }
        /* r12: showIme/hideIme drive the phone keyboard; fields are
           in FN_ARGS below. Unknown types fall through silently. */
    } catch (e) {
        sendMsg('error', { fn: 'recv', msg: String(e.message || e) });
    }
    recv(onMsg);
});

/* argument-name table: menu sends {class:..., method:...}; exports
   were declared as function(cls, method, ...) positionally */
var FN_ARGS = {
    addPatch:        ['class', 'field', 'value'],
    setPatchEnabled: ['class', 'field', 'enabled'],
    removePatch:     ['class', 'field'],
    addHook:         ['class', 'method', 'mode', 'code'],
    setHookEnabled:  ['class', 'method', 'enabled'],
    removeHook:      ['class', 'method'],
    addTracker:      ['class', 'method'],
    trackField:      ['class', 'field'],
    removeTracker:   ['name'],
    startRecorder:   ['filter', 'cap'],
    stopRecorder:    [],
    startMonitor:    [],
    stopMonitor:     [],
    evalScript:      ['src'],
    listLoadedClasses: ['filter', 'qid'],
    openClass:       ['class'],
    readStaticFields: ['class'],
    showIme:         [],
    hideIme:         [],
    listInstances:   ['class'],
    setInstanceField:['class', 'index', 'field', 'value', 'handle'],
    callMethod:      ['class', 'method', 'args', 'handle']
};

/* r14: phased startup. gum_script_load_sync must ALWAYS return:
   the r13 field crash ("script created, loading agent" then
   CRASH signal=6 with zero JS messages) died inside top-level
   Java init. Every Java-touching step now runs from the JS job
   queue AFTER load completes, and each step is phase-marked into
   the native logs, so an abort pins the exact phase. */
agentLog('phase: deferred init scheduled');
setTimeout(function () {
    try {
        install_lifecycle();
    } catch (eL) {
        agentLog('ERR lifecycle: ' + eL);
        sendMsg('error', { fn: 'lifecycle',
                           msg: String(eL && eL.message || eL) });
    }
    try {
        installTouchHook();
    } catch (eT) {
        agentLog('ERR touch hook: ' + eT);
        sendMsg('error', { fn: 'touchhook',
                           msg: String(eT && eT.message || eT) });
    }
    sendMsg('ready', { msg: 'Dex-tool agent r24 loaded' });
    agentLog('phase: ready sent');
}, 0);
