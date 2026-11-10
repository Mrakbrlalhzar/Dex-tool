'use strict';
/* Dex-tool agent (r22). All Java logic lives here (frida-java-bridge);
   libtool.so only feeds input/GL. Messages both ways are JSON via
   send()/recv(); the C++ menu decodes them in menu.cpp.

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
function describe(a) {           /* deep values for trackers (capped) */
    try {
        var s = '';
        for (var i = 0; i < a.length && i < 8; i++) {
            var v = a[i], d;
            try {
                if (v === null || v === undefined) d = 'null';
                else if (v.$className) d = String(v) + '@' + v.$className;
                else d = String(v);
            } catch (e) { d = '?'; }
            if (d.length > 60) d = d.substring(0, 57) + '...';
            s += (i ? ',' : '') + d;
        }
        if (s.length > 240) s = s.substring(0, 237) + '...';
        return s;
    } catch (e) { return '?'; }
}

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

/* r22/audit: parse_rules()/lit_to_java() removed - dead code,
   nothing called them */

var gAddChar = null, gBackspace = null, gWantMouse = null;
var gNativeTouch = null, gSetMap = null;

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
/* the view the overlay actually renders onto: the largest
   SurfaceView / GLSurfaceView / TextureView in the window;
   falls back to android.R.id.content */
function find_gl_view(activity) {
    try {
        var decor = activity.getWindow().getDecorView();
        if (!decor) return content_view(activity);
        var VG = Java.use('android.view.ViewGroup');
        var best = null, bestArea = 0;
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
                if (w > 0 && h > 0 && w * h > bestArea) {
                    best = v; bestArea = w * h;
                }
            }
            var grp = null;
            try { grp = Java.cast(v, VG); } catch (e2) {}
            if (grp) {
                var n = grp.getChildCount();
                for (var i = 0; i < n; i++) stack.push(grp.getChildAt(i));
            }
        }
        if (best) return best;
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
            mapProbed: false };   /* r20: one-shot view-map probe */

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
                        /* Done/Next on the IME: release the menu field so
                           the next tap on it re-opens the keyboard */
                        sendMsg('imeHidden', {});
                        ime_reset_text();
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
        /* r20: same stage tagging as the factory - if the factory
           ever passes and the next failure lands here, the payload
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
                                } else if (ime_try_show(false)) {
                                    agentLog('ime: retry ok at ' + delay + 'ms');
                                }
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
    Java.perform(function () {
        Java.scheduleOnMainThread(function () {
            try {
                if (!IME.edit || !IME.act) return;
                var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
                var imm = Java.cast(IME.act.getSystemService('input_method'), IMMgr);
                imm.hideSoftInputFromWindow(IME.edit.getWindowToken(), 0);
                ime_reset_text();
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
                gNativeTouch(ev.getRawX(), ev.getRawY(), act);
                if (act === 0 || act === 2) {   /* r22/audit: throttled map
                                                   refresh on DOWN + MOVE */
                    var now = Date.now();
                    if (now - gMapLastTouch > 2000) {
                        gMapLastTouch = now;
                        report_map(find_gl_view(this));
                    }
                }
                /* let the overlay decide (menu/keyboard own the tap) */
                if (gWantMouse && gWantMouse()) {
                    if (down || act === 1 || act === 3)
                        this.dispatchTouchEvent(ev);   /* original call */
                    return true;                        /* consume */
                }
            } catch (e) {}
            return this.dispatchTouchEvent(ev);         /* original call */
        };
        agentLog('phase: dispatchTouchEvent hook ok');
        sendMsg('info', { msg: 'touch hook installed (r22, screen coords + IME activity capture + map refresh on DOWN)' });
    });
}

/* ---------------- class info ---------------- */
function field_lines(jc, inst) {
    var out = [];
    var fs = jc.class.getDeclaredFields();
    for (var i = 0; i < fs.length; i++) {
        var f = fs[i];
        try {
            f.setAccessible(true);
            var t = f.getType().getName();
            var val = '?';
            if (inst) val = jstr(f.get(inst));
            else {
                var st = (f.getModifiers() & 8) !== 0;   /* STATIC */
                val = st ? jstr(f.get(null)) : '(instance)';
            }
            out.push(f.getName() + ':' + t + '=' + val);
        } catch (e) { out.push(f.getName() + ':?'); }
    }
    return out;
}
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
       looked stuck on "installing ...") */
    if (!REC.on) {
        REC.busy = false;
        sendMsg('recorderStopped', { installed: REC.installed });
        return;
    }
    for (var i = 0; i < batch && REC.idx < list.length; i++, REC.idx++) {
        if (REC.installed >= REC.cap) break;
        var cls = list[REC.idx];
        if (!rec_wanted(cls, REC.filter)) continue;
        try {
            var jc = Java.use(cls);
            var ms = jc.class.getDeclaredMethods();
            for (var k = 0; k < ms.length; k++) {
                if (REC.installed >= REC.cap) break;
                var name = ms[k].getName();
                var ovl;
                try { ovl = jc[name].overloads; } catch (e0) { continue; }
                if (!ovl) continue;
                var made = 0;
                for (var oi = 0; oi < ovl.length; oi++) {
                    if (REC.installed >= REC.cap) break;
                    try {
                        (function (ov, cn, mn) {
                            ov.implementation = function () {
                                if (!REC.on) {
                                    /* pass-through, no cost when stopped */
                                    return ov.apply(this, arguments);
                                }
                                if (REC.ring.length < 4000) {
                                    var ret, err = '';
                                    try { ret = ov.apply(this, arguments); }
                                    catch (e1) { err = String(e1).substring(0, 80); throw e1; }
                                    REC.ring.push({ cls: cn, m: mn,
                                        a: jargs(arguments),
                                        r: jstr(ret), x: err });
                                    return ret;
                                }
                                return ov.apply(this, arguments);
                            };
                            made++;
                            REC.installed++;
                        })(ovl[oi], cls, name);
                    } catch (e2) {}
                }
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
        /* OkHttp: request line at RealCall.execute/enqueue */
        try {
            var RealCall = Java.use('okhttp3.RealCall');
            RealCall.execute.overloads.forEach(function (ov) {
                ov.implementation = function () {
                    try {
                        if (MON.on) {
                            var req = this.request();
                            var url = String(req.url());
                            mon_push('http', url, String(req.method()),
                                     mon_origin('okhttp:' + url),
                                     'headers ' + req.headers().size());
                        }
                    } catch (e) {}
                    return ov.apply(this, arguments);
                };
            });
            /* r22/audit: enqueue too - most apps issue async calls,
               hooking only execute() saw nothing */
            RealCall.enqueue.overloads.forEach(function (ov) {
                ov.implementation = function () {
                    try {
                        if (MON.on) {
                            var req = this.request();
                            var url = String(req.url());
                            mon_push('http', url, String(req.method()),
                                     mon_origin('okhttp:' + url),
                                     'async, headers ' + req.headers().size());
                        }
                    } catch (e) {}
                    return ov.apply(this, arguments);
                };
            });
            MON.okhttp = true;
        } catch (e) { MON.okhttp = false; }

        /* HttpURLConnection: URL at connect time */
        try {
            var URL = Java.use('java.net.URL');
            URL.openConnection.overloads.forEach(function (ov) {
                ov.implementation = function () {
                    try {
                        if (MON.on)
                            mon_push('urlconn', String(this.toString()), 'open',
                                     mon_origin('url:' + this.toString()), '');
                    } catch (e) {}
                    return ov.apply(this, arguments);
                };
            });
        } catch (e) {}

        /* SSL: best-effort note only; full plaintext would need
           SSL_read/SSL_write native hooks */
        try {
            var SSLSocket = Java.use('javax.net.ssl.SSLSocket');
            mon_push('ssl', 'engine', 'note',
                     '', 'SSLSocket reachable');
        } catch (e) {}

        sendMsg('monitorInfo', {
            note: 'okhttp ' + (MON.okhttp ? 'hooked' : 'absent') +
                   ', urlconn hooked'
        });
    });
}

/* ---------------- helpers for exports ---------------- */
function use_cls_parts(cls, member) {
    var jc;
    try { jc = Java.use(cls); } catch (e) { return null; }
    var found = null;
    try {
        if (jc[member]) found = jc[member];
    } catch (e) {}
    if (!found) {
        try {
            var ms = jc.class.getDeclaredMethods();
            for (var i = 0; i < ms.length; i++)
                if (ms[i].getName() === member) { found = jc[member]; break; }
        } catch (e2) {}
    }
    return found ? { jc: jc, m: found } : null;
}

function call_with_args(ov, argStrings, isCtor) {
    /* map the text args onto the overload's parameter types */
    var params = ov.argumentTypes;
    var real = [];
    for (var i = 0; i < params.length; i++) {
        var t = params[i].name, s = argStrings[i];
        if (s === undefined || s === null || s === '') {
            if (t === 'int' || t === 'short' || t === 'byte' ||
                t === 'long' || t === 'float' || t === 'double') { real.push(0); continue; }
            if (t === 'boolean') { real.push(false); continue; }
            if (t === 'char') { real.push('a'); continue; }
            real.push(null); continue;
        }
        if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
            real.push(parseInt(s, 10) || 0);
        else if (t === 'float' || t === 'double')
            real.push(parseFloat(s) || 0.0);
        else if (t === 'boolean') real.push(s === 'true');
        else if (t === 'char') real.push(s.length ? s.charCodeAt(0) : 'a');
        else if (t.lastIndexOf('[') === 0) real.push(null);
        else real.push(s);
    }
    return ov.apply(null, real);
}

var patches = {}, hooks = {}, trackers = {};

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
            var hit = use_cls_parts(cls, method);
            if (!hit) {
                sendMsg('hookFailed', { name: key, error: 'class or method not found' });
                return;
            }
            var jc = hit.jc;
            try {
                if (mode === 'Java') {
                    var fn = new Function('Java', 'send', 'console', 'thiz',
                                          'args', 'orig', code);
                    jc[method].overloads.forEach(function (ov) {
                        ov.implementation = function () {
                            if (!hooks[key] || !hooks[key].on)
                                return ov.apply(this, arguments);
                            /* r22/audit: keep the receiver + args for
                               orig() - `this` inside the closure is
                               NOT the intercepted call's receiver */
                            var self = this, orig_args = arguments;
                            try {
                                return fn(Java, send, CONSOLE(),
                                          self, Array.prototype.slice.call(orig_args),
                                          function () {
                                              return ov.apply(self,
                                                  arguments.length ? arguments : orig_args);
                                          });
                            } catch (e1) {
                                sendMsg('error', { fn: 'hook:' + key,
                                                   msg: String(e1.message || e1) });
                                return ov.apply(this, arguments);
                            }
                        };
                    });
                    hooks[key] = { mode: mode, on: true, code: code };
                    sendMsg('hookAdded', { name: key });
                } else if (mode === 'noop') {
                    jc[method].overloads.forEach(function (ov) {
                        ov.implementation = function () {
                            if (!hooks[key] || !hooks[key].on)
                                return ov.apply(this, arguments);
                            var rt = ov.returnType.name;
                            if (rt === 'int' || rt === 'long' || rt === 'short' ||
                                rt === 'byte') return 0;
                            if (rt === 'float' || rt === 'double') return 0;
                            if (rt === 'boolean') return false;
                            if (rt === 'char') return '\u0000';
                            return null;
                        };
                    });
                    hooks[key] = { mode: mode, on: true, code: code };
                    sendMsg('hookAdded', { name: key });
                } else if (mode === 'Smali') {
                    var gen = 'H' + (Date.now() % 1000000);
                    var skel = '.class public final Lcom/dextool/hook/' + gen + ';\n' +
                               '.super Ljava/lang/Object;\n' +
                               '.method public static hook(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;\n' +
                               /* r22/audit: default .locals - user code
                                  without a registers directive died in
                                  the assembler */
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
                    jc[method].overloads.forEach(function (ov) {
                        ov.implementation = function () {
                            if (!hooks[key] || !hooks[key].on)
                                return ov.apply(this, arguments);
                            try {
                                var arr = Java.array('java.lang.Object',
                                    Array.prototype.slice.call(arguments));
                                var r = loaded.hook(this, arr);
                                if (r === null) {
                                    var rt = ov.returnType.name;
                                    if (rt === 'int' || rt === 'long' || rt === 'short' ||
                                        rt === 'byte') return 0;
                                    if (rt === 'boolean') return false;
                                    if (rt === 'char') return '\u0000';
                                    if (rt === 'float' || rt === 'double') return 0;
                                }
                                return r;
                            } catch (e1) {
                                sendMsg('error', { fn: 'smali-hook:' + key,
                                                   msg: String(e1.message || e1) });
                                return ov.apply(this, arguments);
                            }
                        };
                    });
                    hooks[key] = { mode: mode, on: true, code: code };
                    sendMsg('smaliReady', { name: key, ok: 1, error: '' });
                    sendMsg('hookAdded', { name: key });
                }
            } catch (e) {
                sendMsg('hookFailed', { name: key, error: String(e.message || e) });
            }
        });
    },
    setHookEnabled: function (cls, method, en) {
        var key = cls + '.' + method;
        if (hooks[key]) hooks[key].on = en;
        sendMsg('info', { msg: 'hook ' + key + ' -> ' + (en ? 'on' : 'off') });
    },
    removeHook: function (cls, method) {
        var key = cls + '.' + method;
        Java.perform(function () {
            try {
                var hit = use_cls_parts(cls, method);
                if (hit) hit.jc[method].overloads.forEach(function (ov) {
                    ov.implementation = null;
                });
            } catch (e) {}
            delete hooks[key];
            sendMsg('info', { msg: 'hook removed ' + key });
        });
    },

    /* ---- trackers ---- */
    addTracker: function (cls, method) {
        var key = cls + '.' + method;
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                jc[method].overloads.forEach(function (ov) {
                    ov.implementation = function () {
                        var out, err = '';
                        try { out = ov.apply(this, arguments); }
                        catch (e1) { err = String(e1).substring(0, 100); throw e1; }
                        if (trackers[key] && trackers[key].on) {
                            /* r22/audit: exclude the tracked method
                               from its own callers list */
                            var ev = { name: key,
                                       callers: jstack(5).filter(function (c) {
                                           return c !== key;
                                       }).slice(0, 4),
                                       args: describe(arguments), ret: jstr(out),
                                       err: err };
                            sendMsg('trackerEvent', ev);
                        }
                        return out;
                    };
                });
                trackers[key] = { on: true };
                sendMsg('info', { msg: 'tracker added ' + key });
            } catch (e) {
                sendMsg('error', { fn: 'addTracker', msg: String(e.message || e) });
            }
        });
    },
    trackField: function (cls, field) {
        /* ART has no field read/write barriers: approximate by watching
           the value before/after every declared method call of the class */
        var key = cls + '.' + field;
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                var f = jc.class.getDeclaredField(field);
                f.setAccessible(true);
                var st = (f.getModifiers() & 8) !== 0;
                var last = st ? jstr(f.get(null)) : '(instance)';
                jc.class.getDeclaredMethods().forEach(function (md) {
                    var mn;
                    try { mn = md.getName(); } catch (e0) { return; }
                    var m;
                    try { m = jc[mn]; } catch (e1) { return; }
                    if (!m || !m.overloads) return;
                    m.overloads.forEach(function (ov) {
                        try {
                            ov.implementation = function () {
                                var r = ov.apply(this, arguments);
                                try {
                                    if (trackers[key] && trackers[key].on) {
                                        var v = st ?
                                            jstr(f.get(null)) :
                                            (this ? jstr(f.get(this)) : '?');
                                        if (v !== last) {
                                            sendMsg('trackerEvent', {
                                                name: key,
                                                callers: jstack(3),
                                                args: last + ' -> ' + v,
                                                ret: '', err: ''
                                            });
                                            last = v;
                                        } else {
                                            sendMsg('trackerEvent', {
                                                name: key, callers: [],
                                                args: v, ret: '', err: ''
                                            });
                                        }
                                    }
                                } catch (e2) {}
                                return r;
                            };
                        } catch (e3) {}
                    });
                });
                trackers[key] = { on: true };
                sendMsg('info', { msg: 'field tracker added ' + key + ' (approximation)' });
            } catch (e) {
                sendMsg('error', { fn: 'trackField', msg: String(e.message || e) });
            }
        });
    },
    removeTracker: function (name) {
        delete trackers[name];
        sendMsg('info', { msg: 'tracker removed ' + name });
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
        /* r12: REC.on=false now also aborts a still-running install
           (see rec_install_chunk) */
        REC.on = false;
        REC.busy = false;
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
                var jc = Java.use(cls);
                /* r12: field VALUES are no longer read here.
                   field_lines(jc, null) called f.get(null) on every
                   static - and a static read can run the class's
                   <clinit>, which may block forever inside app code.
                   That was the "tap a class and the app hangs" bug
                   (together with the menu-side nested lock). */
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
                var jc = Java.use(cls);
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
            var out = [];
            try {
                Java.choose(cls, {
                    onMatch: function (inst) {
                        var fl = field_lines(Java.use(cls), inst);
                        out.push(jstr(inst) + '##' + fl.join(';'));
                        if (out.length >= 20) return 'stop';
                    },
                    onComplete: function () {
                        sendMsg('instanceList', { class: cls, instances: out });
                    }
                });
            } catch (e) {
                sendMsg('instanceList', { class: cls, instances: out });
            }
        });
    },
    setInstanceField: function (cls, index, field, value) {
        Java.perform(function () {
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
                            var t = f.getType().getName();
                            if (t === 'int' || t === 'long' || t === 'short' ||
                                t === 'byte') f.set(inst, parseInt(value, 10) || 0);
                            else if (t === 'boolean') f.set(inst, value === 'true');
                            else if (t === 'float' || t === 'double')
                                f.set(inst, parseFloat(value) || 0.0);
                            else if (t === 'char')
                                f.set(inst, (value || 'a').charCodeAt(0));
                            else f.set(inst, value);
                            sendMsg('patchApplied', { name: cls + '.' + field });
                        } catch (e1) {
                            sendMsg('patchFailed', { name: cls + '.' + field,
                                                     error: String(e1.message || e1) });
                        }
                    }
                });
            } catch (e) {}
        });
    },
    callMethod: function (cls, method, args) {
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                var best = null;
                jc[method].overloads.forEach(function (ov) {
                    var np = ov.argumentTypes.length;
                    if (np === (args ? args.length : 0)) { best = ov; }
                });
                if (!best) {
                    jc[method].overloads.forEach(function (ov) {
                        if (!best) best = ov;
                    });
                }
                if (!best) throw new Error('no overload');
                var r = call_with_args(best, args || [], false);
                sendMsg('callResult', {
                    class: cls, ok: 1, result: jstr(r), error: ''
                });
            } catch (e) {
                sendMsg('callResult', {
                    class: cls, ok: 0, result: '', error: String(e.message || e)
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
    setInstanceField:['class', 'index', 'field', 'value'],
    callMethod:      ['class', 'method', 'args']
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
    sendMsg('ready', { msg: 'Dex-tool agent r22 loaded' });
    agentLog('phase: ready sent');
}, 0);
