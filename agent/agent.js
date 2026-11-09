'use strict';
/* Dex-tool agent (r15). All Java logic lives here (frida-java-bridge);
   libtool.so only feeds input/GL. Messages both ways are JSON via
   send()/recv(); the C++ menu decodes them in menu.cpp.

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
var DOTS = 0, DMAX = 4000;
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
        if (DOTS++ < DMAX && s.length > 240) s = s.substring(0, 237) + '...';
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

/* rewrite rules: parse tiny-language lines from smali code
   supported:  pN = literal | name  /  R = literal   (literal: number,
   true/false/null/"str"/'char' or an identifier=literal arg) */
function parse_rules(code) {
    var rules = [], lines = code.split('\n');
    for (var i = 0; i < lines.length; i++) {
        var ln = lines[i].replace(/#.*$/, '').trim();
        if (!ln) continue;
        var m = ln.match(/^(p\d+|R)\s*=\s*(.+)$/);
        if (m) rules.push({ reg: m[1], lit: m[2].trim(), src: ln });
    }
    return rules;
}
function lit_to_java(lit) {        /* literal string -> java value object */
    if (lit === 'true' || lit === 'false') return lit === 'true';
    if (lit === 'null') return null;
    var q = lit.match(/^"(.*)"$/); if (q) return q[1];
    var c = lit.match(/^'(.)'$/);  if (c) return c[1].charCodeAt(0);
    if (/^-?\d+$/.test(lit)) return parseInt(lit, 10);
    if (/^-?\d+\.\d*$/.test(lit) || /^-?\d*\.\d+$/.test(lit))
        return parseFloat(lit);
    if (/^-?0x[0-9a-fA-F]+$/.test(lit)) return parseInt(lit, 16);
    return undefined;              /* treat as identifier: copy-through */
}

var gAddChar = null, gBackspace = null, gWantMouse = null;
var gNativeTouch = null, gSetMap = null, gDispSize = null;

function ensure_native_fns() {
    if (gNativeTouch) return true;
    try {
        var m = Module.findExportByName('libtool.so', 'dextool_add_char');
        var b = Module.findExportByName('libtool.so', 'dextool_backspace');
        var w = Module.findExportByName('libtool.so', 'dextool_want_mouse');
        var t = Module.findExportByName('libtool.so', 'dextool_on_touch');
        var s = Module.findExportByName('libtool.so', 'dextool_set_touch_map');
        var d = Module.findExportByName('libtool.so', 'dextool_display_size');
        if (!m || !b || !t || !s) return false;
        gAddChar   = new NativeFunction(m, 'void', ['int']);
        gBackspace = new NativeFunction(b, 'void', []);
        gNativeTouch = new NativeFunction(t, 'void', ['float', 'float', 'int']);
        gSetMap    = new NativeFunction(s, 'void', ['int', 'int', 'int', 'int']);
        if (w) gWantMouse = new NativeFunction(w, 'bool', []);
        if (d) gDispSize = new NativeFunction(d, 'int', []);
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
var IME = { act: null, edit: null, actHash: 0, prev: '',
            silent: false, cnt: 0, everLogged: false };

function ime_on_text(ed) {
    try {
        if (!ensure_native_fns()) return;
        var cur = '';
        try { cur = ed.toString(); } catch (e0) { return; }
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
        var delN = prev.length - s - e;      /* UTF-16 units deleted */
        /* deletions first, then insertions - one event per UTF-16
           unit keeps the mapping 1:1 with what we inserted (astral
           chars went in as a surrogate pair, so they also come out
           as two backspaces). */
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

function ime_ensure(act) {
    var h = 0;
    try { h = act.hashCode(); } catch (e0) {}
    if (IME.edit && IME.actHash === h) return IME.edit;
    try {
        IME.edit = null;               /* activity switched */
        IME.prev = '';
        var decor = act.getWindow().getDecorView();
        var ET = Java.use('android.widget.EditText');
        var edit = ET.$new(act);
        edit.setBackgroundDrawable(null);   /* 1x1, transparent */
        edit.setAlpha(0);
        var LP = Java.use('android.widget.FrameLayout$LayoutParams');
        var lp = LP.$new(1, 1);
        decor.addView(edit, lp);
        var tag = (++IME.cnt) + '_' + (Date.now() % 1000000);
        var TW = Java.registerClass({
            name: 'com.dextool.ime.W' + tag,
            implements: [Java.use('android.text.TextWatcher')],
            methods: {
                beforeTextChanged: function (s, a, b, c) {},
                onTextChanged: function (s, a, b, c) {},
                afterTextChanged: function (ed) { ime_on_text(ed); }
            }
        });
        edit.addTextChangedListener(TW.$new());
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
        edit.setOnEditorActionListener(EA.$new());
        IME.edit = edit;
        IME.actHash = h;
        return edit;
    } catch (e) {
        sendMsg('error', { fn: 'ime_ensure', msg: String(e.message || e) });
        return null;
    }
}

function ime_try_show() {
    /* one complete attempt on the main thread: create the hidden
       EditText (+ TextWatcher) if needed, focus it, and send
       showSoftInput. Returns true only if showSoftInput was sent. */
    try {
        if (!IME.act) return false;
        var edit = ime_ensure(IME.act);
        if (!edit) return false;
        ime_reset_text();
        edit.requestFocus();
        var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
        var imm = Java.cast(IME.act.getSystemService('input_method'), IMMgr);
        imm.showSoftInput(edit, 0);
        agentLog('ime: showSoftInput sent');
        return true;
    } catch (e) {
        sendMsg('error', { fn: 'ime_try_show', msg: String(e.message || e) });
        return false;
    }
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
                if (!IME.act) {
                    try { IME.act = find_current_activity_sync(); }
                    catch (e0) { agentLog('ime: findAct failed: ' + e0); }
                }
                if (!IME.act) {
                    sendMsg('warn', { msg: 'IME: no activity yet - async resolve, retries armed' });
                    resolve_current_activity(function (a) {
                        if (!a) return;
                        try { IME.act = a; } catch (e1) {}
                        agentLog('ime: activity resolved async');
                    });
                }
                if (IME.act && !IME.everLogged) {
                    IME.everLogged = true;
                    try {
                        sendMsg('info', { msg: 'IME host activity: ' +
                            IME.act.getClass().getName() });
                    } catch (e5) {}
                }
                ime_try_show();
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
                                    var a = find_current_activity_sync();
                                    if (a) {
                                        IME.act = a;
                                        agentLog('ime: activity resolved at retry ' + delay + 'ms');
                                    }
                                }
                                if (!IME.act) return;
                                if (ime_try_show())
                                    agentLog('ime: retry ok at ' + delay + 'ms');
                            } catch (e2) {}
                        });
                    }, delay);
                });
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
            best = a;
            var paused = false, stopped = false;
            try { paused = rec.paused.value; } catch (e2) {}
            try { stopped = rec.stopped.value; } catch (e3) {}
            if (!paused && !stopped) return a;
        }
    } catch (e4) {}
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
                try { IME.act = act; } catch (e2) {}
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
                    onActivityResumed: function (a) { try { IME.act = a; } catch (e0) {} },
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
                try { IME.act = this; } catch (e0) {}   /* for the IME */
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
            try {
                var act = ev.getAction() & 0xff;
                var down = (act === 0);
                /* r15: raw SCREEN coords (getRawX/getRawY) - the
                   same space the map is measured in
                   (getLocationOnScreen), so overlay.cpp's transform
                   is exact on BOTH axes */
                gNativeTouch(ev.getRawX(), ev.getRawY(), act);
                if (act === 2) {          /* throttle map refresh on moves */
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
        sendMsg('info', { msg: 'touch hook installed (r15, screen coords)' });
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
    var done = 0;
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
                if (made) REC.installed += 0;   /* counted per overload above */
            }
            done++;
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
                var t = f.getType().getName();
                var st = (f.getModifiers() & 8) !== 0;
                if (!st) throw new Error('not a static field (instances: Classes tab)');
                if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
                    f.set(null, parseInt(value, 10) || 0);
                else if (t === 'boolean') f.set(null, value === 'true');
                else if (t === 'float' || t === 'double')
                    f.set(null, parseFloat(value) || 0.0);
                else if (t === 'char') f.set(null, (value || 'a').charCodeAt(0));
                else f.set(null, value);
                patches[key] = { v: value, on: true };
                sendMsg('patchApplied', { name: key });
            } catch (e) {
                sendMsg('patchFailed', { name: key, error: String(e.message || e) });
            }
        });
    },
    setPatchEnabled: function (cls, field, en) {
        var key = cls + '.' + field;
        if (patches[key]) patches[key].on = en;
        if (!en) {
            Java.perform(function () {
                try {
                    var jc = Java.use(cls);
                    var f = jc.class.getDeclaredField(field);
                    f.setAccessible(true);
                    if ((f.getModifiers() & 8) !== 0)
                        f.set(null, f.get(null));   /* identity; real restore
                                                       needs the old value */
                } catch (e) {}
            });
        } else {
            rpc.exports.addPatch(cls, field, patches[key] ? patches[key].v : '');
        }
        sendMsg('info', { msg: 'patch ' + key + ' -> ' + (en ? 'on' : 'off') });
    },
    removePatch: function (cls, field) {
        delete patches[cls + '.' + field];
        sendMsg('info', { msg: 'patch removed ' + cls + '.' + field });
    },

    /* ---- hooks: Java(JS) / Smali / noop ---- */
    addHook: function (cls, method, mode, code) {
        var key = cls + '.' + method;
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
                            try {
                                return fn(Java, send, CONSOLE(),
                                          this, Array.prototype.slice.call(arguments),
                                          function () {
                                              return ov.apply(this, arguments);
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
                            if (rt === 'char') return 'a';
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
                               code + '\n' +
                               '.end method\n.end class';
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
                                    if (rt === 'char') return 'a';
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
                            var ev = { name: key, callers: jstack(4),
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
            if (REC.filter === '.') REC.filter = pkg;  /* menu: explicit all */
            /* '.' sent by the UI means everything; agent: use it directly */
            if (filter === '.') REC.filter = '.';
            REC.cap = (typeof cap === 'number' && cap > 0) ?
                      Math.floor(cap) : 1500;
            var list = Java.enumerateLoadedClassesSync();
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
            var r = (new Function('Java', 'send', 'console', 'require',
                                  'src'))(Java, send, CONSOLE(),
                                          require, src);
            sendMsg('scriptResult', { result: jstr(r) });
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

/* console for user scripts: lines are batched to the Script tab */
var CONS_Lines = [];
function CONSOLE() {
    if (CONS_Lines.flush) return CONS_Dummy;
    CONS_Lines = [];
    return {
        log: function () {
            var a = Array.prototype.map.call(arguments, jstr).join(' ');
            CONS_Lines.push(a);
            sendMsg('console', { line: a });
        },
        error: function () {
            var a = 'ERR ' + Array.prototype.map.call(arguments, jstr).join(' ');
            CONS_Lines.push(a);
            sendMsg('console', { line: a });
        },
        warn: function () {
            var a = 'WARN ' + Array.prototype.map.call(arguments, jstr).join(' ');
            CONS_Lines.push(a);
            sendMsg('console', { line: a });
        }
    };
}
var CONS_Dummy = {
    log: function () {}, error: function () {}, warn: function () {}
};

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
                /* first run: extract from assets */
                var ios = Java.use('java.io.FileOutputStream');
                var am = ctx.getAssets();
                var is = am.open('smali-fat.jar');
                var fos = ios.$new(jar);
                var buf = Java.array('byte', new Array(8192).fill(0));
                var n;
                while ((n = is.read(buf)) > 0) fos.write(buf, 0, n);
                fos.close(); is.close();
            }
            var PC = Java.use('dalvik.system.PathClassLoader');
            var CL = Java.use('java.lang.ClassLoader');
            var parent = CL.getSystemClassLoader();
            var loader = PC.$new(jar, parent);
            var main = loader.loadClass('com.android.tools.smali.Main');
            var tmp = files + '/dxt-' + (Date.now() % 1000000);
            var inF  = tmp + '.smali';
            var outF = tmp + '.dex';
            var FOS = Java.use('java.io.FileOutputStream');
            var fos2 = FOS.$new(inF);
            var jb = Java.array('byte',
                Array.prototype.map.call(src, function (c) { return c.charCodeAt(0) & 0xff; }));
            fos2.write(jb);
            fos2.close();
            var arr = Java.array('java.lang.String', [
                'assemble', '-o', outF, inF ]);
            main.main(arr);
            var FIS = Java.use('java.io.FileInputStream');
            var fis = FIS.$new(outF);
            var all = [], chunk = Java.array('byte', new Array(8192).fill(0));
            var k;
            while ((k = fis.read(chunk)) > 0) {
                for (var c = 0; c < k; c++) all.push(chunk[c] & 0xff);
            }
            fis.close();
            var bytes = Java.array('byte', all);
            out.dex = bytes;
            var Del = Java.use('java.io.File');
            Del.$new(inF).delete();
            Del.$new(outF).delete();
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
            var loader = IMDC.$new(Java.array('java.nio.ByteBuffer', [bb]),
                                   CL.getSystemClassLoader());
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
    sendMsg('ready', { msg: 'Dex-tool agent r14 loaded' });
    agentLog('phase: ready sent');
}, 0);
