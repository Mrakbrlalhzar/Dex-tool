/* Dex-tool agent (r35): B reports DisplayMetrics density/dpi in
   the ready payload so the menu sizes the UI in real dp (the
   overlay draws in physical pixels). r34: G2 guarded the delayed
   echo of a programmatic EditText clear. r33: reflection
   method/field search with name/param/return filters,
   createInstance args, listConstructors. r32: FN_ARGS by JSON
   key. r31: releaseHandle. r30: hook-mode prefix match +
   unknown-mode report, stale handle reporting. */
'use strict';
function sendMsg(type, payload) { send({ type: type, payload: payload }); }
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
var gAddChar = null, gBackspace = null, gWantMouse = null;
var gNativeTouch = null, gSetMap = null;
var gHitTest = null, gDisplaySize = null;   /* r23 */
var gWantFrames = null;                     /* r25 frame pump */
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
        var h = Module.findExportByName('libtool.so', 'dextool_hit_test');
        if (h) gHitTest = new NativeFunction(h, 'bool', ['float', 'float']);
        var d = Module.findExportByName('libtool.so', 'dextool_display_size');
        if (d) gDisplaySize = new NativeFunction(d, 'int', []);
        var wf = Module.findExportByName('libtool.so', 'dextool_want_frames');
        if (wf) gWantFrames = new NativeFunction(wf, 'int', []);
        return true;
    } catch (e) { return false; }
}
var gMapSent = { w: -1, h: -1, x: -9999, y: -9999 };
var gMapLastTouch = 0;
var gTouchEat = false;   /* r23: the current gesture started on the menu */
var gPumpTick = 0, gPumpLogged = 0, gPumpMissLogged = 0;
function pump_frames() {
    try {
        if (!gWantFrames) return;
        var lvl = 0;
        try { lvl = gWantFrames(); } catch (eL) { return; }
        if (!lvl || !IME || !IME.act) return;
        gPumpTick++;
        if (lvl === 1 && (gPumpTick % 4) !== 0) return;
        if (!gPumpLogged) {
            gPumpLogged = 1;
            agentLog('frame pump: on (level ' + lvl + ')');
        }
        var w = null, dv = null;
        try { w = IME.act.getWindow(); } catch (eW) { return; }
        if (!w) return;
        try { dv = w.getDecorView(); } catch (eD) { return; }
        if (!dv) return;
        if (typeof dv.postInvalidate !== 'function') {
            if (!gPumpMissLogged) {
                gPumpMissLogged = 1;
                agentLog('frame pump: decor.postInvalidate typeof ' +
                    (typeof dv.postInvalidate) + ' - pump disabled');
            }
            return;
        }
        dv.postInvalidate();
    } catch (e) {}
}
setInterval(pump_frames, 30);
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
        return activity.getWindow().getDecorView()
                        .findViewById(0x01020002);
    } catch (e) { return null; }
}
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
var IME = { act: null, edit: null, actHash: 0, prev: '',
            silent: false, cnt: 0, everLogged: false,
            mapProbed: false,                   /* r20: one-shot view-map probe */
            restore: null,                      /* r23: app view whose focus we borrowed */
            showSeq: 0,                         /* r24: show-request session token */
            upLogged: false };                  /* r24: "already up" logged once per session */
function ime_release_act(obj) {
    try {
        if (!obj) return;
        if (typeof Java.release === 'function') { Java.release(obj); return; }
        if (typeof obj.$dispose === 'function') obj.$dispose();
    } catch (e) {}
}
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
function ime_set_act(obj, how) {
    if (!obj) return false;
    try {
        if (IME.act) {
            var same = false;
            try { same = IME.act.equals(obj); } catch (eQ) { same = false; }
            if (same) return false;   /* already tracked - reuse */
            ime_release_act(IME.act);
            if (IME.restore) { ime_release_act(IME.restore); IME.restore = null; }
        }
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
        var cur = ime_read_text(ed);
        if (cur === null) return;
        cur = String(cur);
        if (IME.silent) { IME.prev = cur; return; }   /* programmatic */
        var prev = IME.prev;
        if (cur === prev) return;
        if (cur === '' && Date.now() - (IME.clearAt || 0) < 400) {
            /* r34 G2: the watcher fires AFTER ime_reset_text
               returned, so the silent flag is already down - this
               is the echo of our own clear, not a user edit.
               Forwarding it used to wipe the field with
               backspaces (the "single character" reset). */
            IME.prev = '';
            return;
        }
        var s = 0;
        while (s < prev.length && s < cur.length && prev[s] === cur[s]) s++;
        var e = 0;
        while (e < prev.length - s && e < cur.length - s &&
               prev[prev.length - 1 - e] === cur[cur.length - 1 - e]) e++;
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
        IME.clearAt = Date.now();   /* r34 G2: echo guard window */
    } catch (e) { IME.silent = false; }
}
function ime_stage(name, fn) {
    try { return fn(); }
    catch (eS) {
        throw new Error('stage ' + name + ': ' +
            String((eS && eS.message) || eS));
    }
}
function ime_create_edit(act) {
    var h = 0;
    try { h = act.hashCode(); } catch (e0) {}
    if (IME.edit && IME.actHash === h) return IME.edit;
    try {
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
        var edit = ime_stage('EditText.$new', function () {
            var ET = Java.use('android.widget.EditText');
            if (typeof ET.$new !== 'function')
                throw new Error('EditText.$new typeof ' +
                    (typeof ET.$new));
            return ET.$new(act);
        });
        ime_stage('clear background', function () {
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
                        IME.showSeq++;            /* keyboard closing: ladder stops */
                        sendMsg('imeEnter', {});
                        sendMsg('imeHidden', {});
                        ime_reset_text();
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
function ime_ensure(act) {
    return ime_create_edit(act);
}
function ime_try_show(reset) {
    try {
        if (!IME.act) {
            agentLog('ime: try_show skipped - no activity yet');
            return false;
        }
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
        try {
            var cur = IME.act.getWindow().getCurrentFocus();
            if (cur) {
                var isOurs = false;
                try { if (IME.edit && cur.equals(IME.edit)) isOurs = true; }
                catch (eC) {}
                if (!isOurs) ime_set_restore(cur);
            }
        } catch (eF) {}
        ime_stage('edit.requestFocus', function () {
            edit.requestFocus();
        });
        var imm = ime_stage('getSystemService(input_method)', function () {
            var IMMgr = Java.use('android.view.inputmethod.InputMethodManager');
            return Java.cast(IME.act.getSystemService('input_method'), IMMgr);
        });
        ime_stage('showSoftInput', function () {
            var sent = imm.showSoftInput(edit, 0);
            agentLog('ime: showSoftInput sent -> ' + sent);
        });
        return true;
    } catch (e) {
        sendMsg('error', { fn: 'ime_try_show', msg: String(e.message || e) });
        return false;
    }
}
function ime_act_resolved(a, how) {
    if (!a) return;
    ime_set_act(a, 'async resolve (' + how + ')');
    Java.scheduleOnMainThread(function () {
        try {
            if (ime_try_show(false))
                agentLog('ime: show ok after ' + how);
        } catch (e2) {}
    });
}
function ime_show() {
    var seq = ++IME.showSeq;
    IME.upLogged = false;
    Java.perform(function () {
        Java.scheduleOnMainThread(function () {
            try {
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
                    sendMsg('warn', { msg: 'IME: no activity yet (' +
                        (walkErr ? 'walk threw: ' + walkErr
                                 : String(find_current_activity_sync.lastDiag ||
                                          'unknown')) +
                        ') - async resolve, retries armed' });
                    resolve_current_activity(function (a) {
                        ime_act_resolved(a, 'async');
                    });
                }
                ime_try_show(true);   /* r16: only the first attempt resets */
                [150, 450, 900, 1500].forEach(function (delay) {
                    setTimeout(function () {
                        Java.scheduleOnMainThread(function () {
                            try {
                                if (seq !== IME.showSeq) return;
                                if (!IME.act) {
                                    resolve_current_activity(function (a) {
                                        ime_act_resolved(a,
                                            'retry ' + delay + 'ms');
                                    });
                                    return;
                                }
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
    try {
        Java.choose('android.app.Activity', {
            onMatch: function (act) {
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
                ime_restore_focus();
            } catch (e) {}
        });
    });
}
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
            try {
                if (!IME.act || !IME.act.equals(this))
                    ime_set_act(this, 'touch hook');
            } catch (eAct) {}
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
                var rx = ev.getRawX(), ry = ev.getRawY();
                gNativeTouch(rx, ry, act);
                if (down && gHitTest) {
                    try { gTouchEat = !!gHitTest(rx, ry); } catch (eH) {}
                }
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
                    return true;                     /* consume */
                }
            } catch (e) {}
            return this.dispatchTouchEvent(ev);         /* original call */
        };
        agentLog('phase: dispatchTouchEvent hook ok');
        sendMsg('info', { msg: 'touch hook installed (r25, screen coords + hit-test click-through + UP-leak fix + IME activity capture + map refresh on DOWN + frame pump)' });
    });
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
function jpipe(v, cap) {
    try {
        var n = cap || 2000;
        var s = (v === null || v === undefined) ? '' : String(v);
        if (s.length > n) s = s.substring(0, n);
        return s.replace(/\|/g, '!').replace(/#/g, '%');
    } catch (e) { return '?'; }
}
var MON = { on: false, ring: [], origins: {}, okhttp: null };
function mon_push(kind, url, method, origin, note, detail) {
    if (!MON.on) return;
    if (MON.ring.length > 600) MON.ring.shift();
    MON.ring.push({
        k: kind, u: (url || '').substring(0, 120),
        m: method || '', o: origin || '', n: (note || '').substring(0, 120),
        d: jpipe(detail, 2000)
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
        return e.k + '|' + e.u + '|' + e.m + '|' + e.o + '|' + e.n +
               '|' + (e.d || '');
    }) });
}
setInterval(function () { if (MON.on) mon_flush(); }, 600);
function mon_install() {
    Java.perform(function () {
        function req_body_repr(req) {
            try {
                var b = req.body();
                if (!b) return '';
                var Buffer = Java.use('okio.Buffer');
                var buf = Buffer.$new();
                b.writeTo(buf);
                return buf.readUtf8();
            } catch (e) { return ''; }
        }
        function resp_body_repr(resp) {
            try {
                var pb = resp.peekBody(4096);
                var s = pb.string();
                try { pb.close(); } catch (e0) {}
                return s;
            } catch (e) { return ''; }
        }
        var okhttpOk = false;
        try {
            if (dt_is_loaded('okhttp3.RealCall')) {
                install_layer('okhttp3.RealCall', 'execute', 'mon', {
                    fn: function (self, a, out) {
                        try {
                            var req = self.request();
                            var url = String(req.url());
                            var org = mon_origin('okhttp:' + url);
                            mon_push('http-req', url,
                                     String(req.method()), org,
                                     'headers ' + req.headers().size(),
                                     req_body_repr(req));
                            try {
                                var cn = String(out.$className || '');
                                if (cn === 'okhttp3.Response') {
                                    mon_push('http-resp', url,
                                             'resp ' + String(out.code()),
                                             org, '',
                                             resp_body_repr(out));
                                }
                            } catch (eR) {}
                        } catch (e) {}
                    }
                });
                install_layer('okhttp3.RealCall', 'enqueue', 'mon', {
                    fn: function (self, a, out) {
                        try {
                            var req = self.request();
                            var url = String(req.url());
                            mon_push('http-req', url,
                                     String(req.method()),
                                     mon_origin('okhttp:' + url),
                                     'async, headers ' + req.headers().size(),
                                     req_body_repr(req));
                        } catch (e) {}
                    }
                });
                okhttpOk = true;
            }
        } catch (e) { okhttpOk = false; }
        try {
            install_layer('java.net.URL', 'openConnection', 'mon', {
                fn: function (self, a, out) {
                    try {
                        mon_push('urlconn', String(self.toString()), 'open',
                                 mon_origin('url:' + self.toString()), '', '');
                    } catch (e) {}
                }
            });
        } catch (e) {}
        try {
            if (dt_is_loaded('java.net.HttpURLConnection')) {
                install_layer('java.net.HttpURLConnection',
                              'getInputStream', 'mon', {
                    fn: function (self, a, out) {
                        try {
                            var u = String(self.getURL());
                            mon_push('urlconn-resp', u, 'read',
                                     mon_origin('url:' + u),
                                     'response stream open', '');
                        } catch (e) {}
                    }
                });
            }
        } catch (e) {}
        try {
            Java.use('javax.net.ssl.SSLSocket');
            mon_push('ssl', 'engine', 'note',
                     '', 'SSLSocket reachable', '');
        } catch (e) {}
        sendMsg('monitorInfo', {
            note: 'okhttp ' + (okhttpOk ? 'hooked' : 'absent') +
                   ', urlconn hooked'
        });
    });
}
function coerce_arg(t, s) {
    if (s === undefined || s === null || s === '') {
        if (t === 'int' || t === 'short' || t === 'byte' ||
            t === 'long' || t === 'float' || t === 'double') return 0;
        if (t === 'boolean') return false;
        if (t === 'char') return '\u0000';
        return null;
    }
    if (t === 'int' || t === 'long' || t === 'short' || t === 'byte')
        return parseInt(s, 10) || 0;
    if (t === 'float' || t === 'double')
        return parseFloat(s) || 0.0;
    if (t === 'boolean') return s === 'true';
    if (t === 'char') return s.length ? s.charCodeAt(0) : '\u0000';
    if (t.lastIndexOf('[') === 0) return null;
    if (s === 'null') return null;
    return s;
}
function call_with_args(ov, argStrings, isCtor, recv) {
    var params = ov.argumentTypes;
    var real = [];
    for (var i = 0; i < params.length; i++) {
        var a = argStrings[i];
        if (a && typeof a === 'object' && typeof a.handle === 'number') {
            var inst = inst_get(a.handle);
            if (!inst)
                throw new Error('object #' + a.handle +
                    ' expired - Find Objects again');
            real.push(inst);
            continue;
        }
        real.push(coerce_arg(params[i].name, a));
    }
    return ov.apply(recv || null, real);
}
function scan_hit(s) {
    if (SCAN.cs) return s.indexOf(SCAN.fRaw) !== -1;
    return s.toLowerCase().indexOf(SCAN.f) !== -1;
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
function dispatch_layers(K, L, ov, self, args) {
    var p = L ? L.parts : {};
    try {
        if (p.hook || p.trk) {
            L.hits = (L.hits || 0) + 1;
            var nowH = Date.now();
            if (!L.hitAt || nowH - L.hitAt >= 500) {
                L.hitAt = nowH;
                sendMsg('hookHit', { name: K.replace('!', '.'),
                                     count: L.hits });
            }
        }
    } catch (eH) {}
    var hk = p.hook;
    var io = (p.io && p.io.on) ? p.io : null;
    var eff = args;
    if (io) {
        try {
            var at = ov.argumentTypes;
            var a2 = Array.prototype.slice.call(args);
            var any = false;
            for (var kk in io.args) {
                var ii = kk | 0;
                if (ii >= a2.length) continue;
                a2[ii] = coerce_arg(at[ii] ? at[ii].name : 'java.lang.Object',
                                    io.args[kk]);
                any = true;
            }
            if (any) eff = a2;
        } catch (eI) {}
    }
    var out;
    try {
        if (hk && hk.on && hk.mode === 'noop') {
            out = noop_ret(ov);
        } else if (hk && hk.on) {
            try {
                out = hk.fn(Java, send, CONSOLE(), self,
                            Array.prototype.slice.call(eff),
                            function () {
                                return ov.apply(self,
                                    arguments.length ? arguments : eff);
                            });
            } catch (e1) {
                sendMsg('error', { fn: 'hook:' + K,
                                   msg: String(e1.message || e1) });
                out = ov.apply(self, eff);
            }
            if (hk.mode === 'Smali' && out === null)
                out = noop_ret(ov);
        } else {
            out = ov.apply(self, eff);
        }
    } catch (e2) {
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
    try {
        if (p.rec && REC.on && REC.ring.length < 4000) {
            var j = K.indexOf('!');   /* r24: lazy - rows only */
            REC.ring.push({ cls: K.substring(0, j),
                            m: K.substring(j + 1), a: jargs(args),
                            r: jstr(out), x: '' });
        }
    } catch (e4) {}
    try {
        if (p.mon && MON.on) p.mon.fn(self, args, out);
    } catch (e5) {}
    try {
        if (p.ioprobe && p.ioprobe.want) {
            p.ioprobe.want = false;
            var pr = { params: [] };
            try {
                var pt = ov.argumentTypes;
                for (var pi = 0; pi < pt.length; pi++) {
                    pr.params.push({
                        t: String(pt[pi].name),
                        v: field_repr(args[pi])
                    });
                }
            } catch (eP) {}
            sendMsg('methodIO', {
                class: p.ioprobe.cls, method: p.ioprobe.mn,
                ok: 1, error: '',
                params: pr.params,
                rettyp: String(ov.returnType.name),
                ret: field_repr(out)
            });
            try {
                uninstall_layer(p.ioprobe.cls, p.ioprobe.mn, 'ioprobe');
            } catch (eU) {}
        }
    } catch (e5b) {}
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
    if (io && io.ret !== null && io.ret !== undefined) {
        try { out = coerce_arg(String(ov.returnType.name), io.ret); }
        catch (e8) {}
    }
    return out;
}
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
    if (!gLoadedSet || now - gLoadedAt > 3000) {
        dt_rebuild_loaded_set();
        gLoadedRechecked = false;   /* new cache generation */
    }
    if (gLoadedSet && gLoadedSet[cls]) return true;
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
function r33_mopt(mref) {
    /* r33 F5: openClass highlight honors param/return filters */
    try {
        if (gLastSearch.pt) {
            var ps = mref.getParameterTypes(), pk = false;
            for (var p = 0; p < ps.length; p++)
                if (scan_match(gLastSearch.pt,
                        String(ps[p].getName()))) { pk = true; break; }
            if (!pk) return false;
        }
        if (gLastSearch.rt && !scan_match(gLastSearch.rt,
                String(mref.getReturnType().getName()))) return false;
    } catch (e0) {}
    return true;
}
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
    } else {
        /* r30 C3: never fail silently on an unknown mode */
        sendMsg('hookFailed',
                { name: key, error: 'unknown hook mode: ' + mode });
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
rpc.exports = {
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
    addHook: function (cls, method, mode, code) {
        var key = cls + '.' + method;
        var ml = String(mode).toLowerCase();
        if (ml.indexOf('java') === 0) mode = 'Java';
        else if (ml === 'smali') mode = 'Smali';
        else if (ml.indexOf('nop') === 0 || ml.indexOf('noop') === 0)
            /* 'nop' matches the hook editor's "Nop (auto)" (r30 C3),
               'noop' matches the Patcher's Patch NOP button - two
               different sources, both needed; not a copy-paste
               leftover (r32 re-check) */
            mode = 'noop';
        Java.perform(function () {
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
        if (hooks[key]) hooks[key].on = en;
        sendMsg('info', { msg: 'hook ' + key + ' -> ' + (en ? 'on' : 'off') });
    },
    removeHook: function (cls, method) {
        var key = cls + '.' + method;
        Java.perform(function () {
            uninstall_layer(cls, method, 'hook');
            delete hooks[key];
            sendMsg('info', { msg: 'hook removed ' + key });
        });
    },
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
            if (filter === '.') REC.filter = '.';
            REC.cap = (typeof cap === 'number' && cap > 0) ?
                      Math.floor(cap) : 1500;
            var list = Java.enumerateLoadedClassesSync();
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
            setTimeout(function () { rec_install_chunk(list, 12); }, 50);
        });
    },
    stopRecorder: function () {
        REC.on = false;
        REC.busy = false;
        Java.perform(function () { uninstall_all('rec'); });
        rec_flush();
        sendMsg('recorderStopped', { installed: REC.installed });
    },
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
    evalScript: function (src) {
        try {
            var r = (new Function('Java', 'send', 'console', String(src)))(
                        Java, send, CONSOLE());
            if (r !== undefined) sendMsg('scriptResult', { result: cstr(r) });
        } catch (e) {
            sendMsg('scriptError', { msg: String(e && e.stack ? e.stack : e) });
        }
    },
    releaseHandle: function (handle) {
        /* r31 M7: the menu posts this when an object tab closes -
           free the retained instance now instead of waiting for the
           200-entry LRU to evict it */
        var k = String(handle);
        var w = gInstHandles[k];
        if (w) {
            try { ime_release_act(w); } catch (e0) {}
            delete gInstHandles[k];
        }
    },
    dumpLoadedClasses: function () {
        Java.perform(function () {
            var path = '', n = 0, err = '';
            try {
                var AT = Java.use('android.app.ActivityThread');
                var ctx = AT.currentApplication();
                var FOS = Java.use('java.io.FileOutputStream');
                path = ctx.getFilesDir().getAbsolutePath() +
                       '/dextool-classes.txt';
                var fos = FOS.$new(path);
                var all = Java.enumerateLoadedClassesSync();
                all.sort();
                var SJ = Java.use('java.lang.String');
                for (var i = 0; i < all.length; i++) {
                    fos.write(SJ.$new(all[i] + '\n').getBytes('UTF-8'));
                    n++;
                }
                fos.close();
            } catch (e0) { err = String(e0.message || e0); }
            sendMsg('dumpReady', { kind: 'classes', path: path,
                                   count: n, error: err });
        });
    },
    dumpObject: function (cls, handle) {
        Java.perform(function () {
            var path = '', err = '';
            try {
                var inst = inst_get(handle);
                if (!inst) throw new Error('handle 0x' +
                    (handle >>> 0).toString(16) + ' is gone (GC)');
                var jc = Java.use(cls);
                var AT = Java.use('android.app.ActivityThread');
                var ctx = AT.currentApplication();
                path = ctx.getFilesDir().getAbsolutePath() +
                       '/dextool-object-' +
                       String(cls).replace(/\./g, '_') +
                       '-0x' + (handle >>> 0).toString(16) + '.json';
                var rows = field_rows(jc, inst);
                var ts = '';
                try { ts = String(inst); }
                catch (eT) { ts = '(toString failed)'; }
                var j = '{\n  "class": ' + JSON.stringify(String(cls)) +
                        ',\n  "handle": ' + handle +
                        ',\n  "toString": ' + JSON.stringify(ts) +
                        ',\n  "fields": [\n';
                for (var i = 0; i < rows.length; i++) {
                    j += '    ' + JSON.stringify({
                             n: rows[i].n, t: rows[i].t, v: rows[i].v
                         }) + (i + 1 < rows.length ? ',' : '') + '\n';
                }
                j += '  ]\n}\n';
                var fos = Java.use('java.io.FileOutputStream').$new(path);
                fos.write(Java.use('java.lang.String')
                              .$new(j).getBytes('UTF-8'));
                fos.close();
            } catch (e0) { err = String(e0.message || e0); }
            sendMsg('dumpReady', { kind: 'object', path: path,
                                   count: 0, error: err });
        });
    },
    listLoadedClasses: function (filter, qid, mode, sys, cs,
                                 mn, pt, rt) {
        Java.perform(function () {
            try {
                var all = Java.enumerateLoadedClassesSync();
                var fRaw = String(filter || '');
                var f = fRaw.toLowerCase();
                var md = (typeof mode === 'number') ? mode : 0;
                var includeSys = (sys === true || sys === 1);
                var csm = (cs === true || cs === 1);
                gLastSearch = { f: f, fRaw: fRaw, mode: md, cs: csm,
                                mn: String(mn || ''),
                                pt: String(pt || ''),
                                rt: String(rt || '') };
                if (!includeSys) {
                    all = all.filter(function (n) {
                        return !is_system_cls(n);
                    });
                }
                all.sort();
                if (!fRaw || md === 0) {
                    var out0 = fRaw ? all.filter(function (n) {
                        return csm ? n.indexOf(fRaw) !== -1
                                   : n.toLowerCase().indexOf(f) !== -1;
                    }) : all;
                    page_out(out0, qid, all.length);
                    return;
                }
                if (SCAN.on)
                    sendMsg('info', { msg: 'search: previous scan replaced' });
                var want = {};
                for (var i = 0; i < all.length; i++) want[all[i]] = 1;
                SCAN.on = true;
                SCAN.qid = (typeof qid === 'number') ? qid : 0;
                SCAN.f = f;
                SCAN.fRaw = fRaw;
                SCAN.cs = csm;
                SCAN.md = md;
                SCAN.mn = gLastSearch.mn;   /* r33 F5 */
                SCAN.pt = gLastSearch.pt;
                SCAN.rt = gLastSearch.rt;
                SCAN.sys = includeSys;
                SCAN.list = null;
                SCAN.ri = 0;
                SCAN.want = want;
                SCAN.matches = [];
                SCAN.hits = {};
                SCAN.dexes = null;
                SCAN.di = 0;
                SCAN.it = null;
                SCAN.total = all.length;
                sendMsg('info', { msg: 'search: scanning ' +
                                       all.length + ' loaded class(es) ' +
                                       '(mode ' + md + ')' });
                setTimeout(scan_step, 15);
            } catch (e) {
                sendMsg('error', { fn: 'listLoadedClasses',
                                   msg: String(e.message || e) });
            }
        });
    },
    openClass: function (cls) {
        Java.perform(function () {
            try {
                var jc = use_loaded(cls);
                var hm = [], hf = [];
                var fl = gLastSearch.f || '';
                if (fl) {
                    if (gLastSearch.mode === 1) {
                        var ms = jc.class.getDeclaredMethods();
                        for (var i = 0; i < ms.length; i++) {
                            var nm;
                            try { nm = String(ms[i].getName()); }
                            catch (eN) { continue; }
                            if ((gLastSearch.cs ?
                                     nm.indexOf(gLastSearch.fRaw) !== -1 :
                                     nm.toLowerCase().indexOf(fl) !== -1) &&
                                r33_mopt(ms[i]) &&
                                hm.indexOf(nm) === -1) hm.push(nm);
                        }
                    } else if (gLastSearch.mode === 2) {
                        var fs = jc.class.getDeclaredFields();
                        for (var j = 0; j < fs.length; j++) {
                            var fn;
                            try { fn = String(fs[j].getName()); }
                            catch (eF) { continue; }
                            if ((gLastSearch.cs ?
                                     fn.indexOf(gLastSearch.fRaw) !== -1 :
                                     fn.toLowerCase().indexOf(fl) !== -1) &&
                                hf.indexOf(fn) === -1) hf.push(fn);
                        }
                    } else if (gLastSearch.mode === 3) {
                        var h3 = gLastScanHits[cls];
                        if (h3) { hm = h3.m || []; hf = h3.f || []; }
                    }
                }
                sendMsg('classInfo', {
                    class: cls,
                    methods: method_lines(jc),
                    fields: field_lines_light(jc),
                    hits_m: hm,
                    hits_f: hf
                });
            } catch (e) {
                sendMsg('classInfo', { class: cls, methods: [], fields: [],
                                       hits_m: [], hits_f: [] });
                sendMsg('error', { fn: 'openClass', msg: String(e.message || e) });
            }
        });
    },
    getMethodSrc: function (cls, method) {
        Java.perform(function () {
            var done = function (ok, src, err) {
                sendMsg('methodSrc', { class: cls, method: method,
                                       ok: ok ? 1 : 0,
                                       src: ok ? src : '',
                                       error: ok ? '' : String(err) });
            };
            try {
                var L = baksmali_libs();
                if (!L) throw new Error('baksmali dex unavailable ' +
                                        '(smali-fat.jar failed to load)');
                var type = 'L' + cls.replace(/\./g, '/') + ';';
                var dexes = dex_files();
                if (!dexes || !dexes.length)
                    throw new Error('app dex files not found');
                var cdef = null;
                for (var d = 0; d < dexes.length && !cdef; d++) {
                    try {
                        var it = dexes[d].getClasses().iterator();
                        while (it.hasNext()) {
                            var c = it.next();
                            if (String(c.getType()) === type) {
                                cdef = c;
                                break;
                            }
                        }
                    } catch (e0) {}
                }
                if (!cdef)
                    throw new Error('class not in the app dex files: ' + cls);
                var ctx = Java.use('android.app.ActivityThread')
                              .currentApplication();
                var dir = ctx.getFilesDir().getAbsolutePath() +
                          '/dxt-src-' + (Date.now() % 1000000);
                Java.use('java.io.File').$new(dir).mkdirs();
                var handler = L.CFH.$new(
                    Java.use('java.io.File').$new(dir), '.smali');
                var opts = L.Opts.$new();
                var ok = L.Baksmali.disassembleClass(cdef, handler, opts);
                if (!ok) throw new Error('baksmali failed for ' + cls);
                var sp = dir + '/' + cls.replace(/\./g, '/') + '.smali';
                var text = read_file_text(sp);
                rm_rf(Java.use('java.io.File').$new(dir));
                var src = extract_method_blocks(text, method);
                if (!src)
                    throw new Error('no bytecode found for ' + method +
                        ' (abstract/native methods have none)');
                done(true, src, '');
            } catch (e) {
                done(false, '', e.message || e);
            }
        });
    },
    getMethodIO: function (cls, method) {
        Java.perform(function () {
            try {
                if (!dt_is_loaded(cls))
                    throw new Error('class not loaded yet: ' + cls);
                var n = install_layer(cls, method, 'ioprobe',
                                      { want: true, cls: cls, mn: method });
                if (!n) throw new Error('method not found: ' + method);
                sendMsg('info', { msg: 'IO probe armed on ' + cls + '.' +
                                       method + ' - values arrive on ' +
                                       'its next call' });
            } catch (e) {
                sendMsg('methodIO', { class: cls, method: method,
                                      ok: 0, error: String(e.message || e),
                                      params: [], rettyp: '', ret: '' });
            }
        });
    },
    setMethodIO: function (cls, method, args, ret) {
        Java.perform(function () {
            var key = cls + '.' + method;
            try {
                var map = {};
                if (args) {
                    for (var i = 0; i < args.length; i++) {
                        var a = args[i];
                        if (!a) continue;
                        map[String(a.i | 0)] =
                            String(a.v === undefined || a.v === null ?
                                   '' : a.v);
                    }
                }
                var retPatch = (ret && ret.on) ?
                    String(ret.v === undefined || ret.v === null ?
                           '' : ret.v) : null;
                var any = false;
                for (var k in map) { any = true; break; }
                if (!any && retPatch === null) {
                    uninstall_layer(cls, method, 'io');
                    sendMsg('info', { msg: 'IO patch removed ' + key });
                    return;
                }
                if (!install_layer(cls, method, 'io',
                                   { on: true, args: map, ret: retPatch }))
                    throw new Error('method not found: ' + method);
                sendMsg('info', { msg: 'IO patch armed ' + key });
            } catch (e) {
                sendMsg('error', { fn: 'setMethodIO',
                                   msg: String(e.message || e) });
            }
        });
    },
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
            /* r30 M7: the old fallback re-ran Java.choose and
               patched found[index] - that index came from an earlier
               scan and could resolve to a DIFFERENT object. Report
               instead of silently patching the wrong instance. */
            sendMsg('patchFailed', { name: key,
                error: 'instance handle gone (GC) - re-scan and pick again' });
        });
    },
    callMethod: function (cls, method, args, handle, count, main) {
        var n = 1;
        if (typeof count === 'number' && count > 0) n = Math.min(count, 50);
        var onMain = (main === true || main === 1);
        var done = 0;
        var once = function () {
            Java.perform(function () {
                try {
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
                    var r;
                    if (recv) {
                        r = call_with_args(best, args || [], false, recv);
                    } else {
                        if (!is_static_method(jc, method))
                            throw new Error('instance method - Find ' +
                                'instances, pick one (or use the Object ' +
                                'Finder), then Call (a retained handle is ' +
                                'required as the receiver)');
                        r = call_with_args(best, args || [], false, null);
                    }
                    sendMsg('callResult', {
                        class: cls, ok: 1, result: jstr(r), error: '',
                        idx: done, of: n
                    });
                } catch (e) {
                    sendMsg('callResult', {
                        class: cls, ok: 0, result: '',
                        error: String(e.message || e), idx: done, of: n
                    });
                }
                done++;
                if (done < n) {
                    setTimeout(function () {
                        if (onMain) Java.scheduleOnMainThread(once);
                        else once();
                    }, 30);
                }
            });
        };
        if (onMain) Java.scheduleOnMainThread(once);
        else once();
    },
    listConstructors: function (cls) {
        /* r33 F8: constructor list for the CreateInstancePopup */
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                var cs = jc.class.getDeclaredConstructors();
                var out = [];
                for (var i = 0; i < cs.length && out.length < 32; i++) {
                    var ps = cs[i].getParameterTypes(), pn = [];
                    for (var p = 0; p < ps.length; p++)
                        pn.push(String(ps[p].getName()));
                    out.push({
                        name: cls.substring(cls.lastIndexOf('.') + 1),
                        params: pn.join(', ') });
                }
                sendMsg('ctorList', { class: cls, ctors: out });
            } catch (e) {
                sendMsg('error', { fn: 'listConstructors',
                                   msg: String(e.message || e) });
            }
        });
    },
    createInstance: function (cls, args) {
        Java.perform(function () {
            try {
                var jc = Java.use(cls);
                /* r33 F8: dynamic construction - "@handle:N"
                   resolves a retained instance, numbers and
                   booleans coerce, everything else stays a string */
                var av = [];
                if (args && args.length) {
                    for (var ai = 0; ai < args.length; ai++) {
                        var raw = args[ai];
                        if (raw === null || raw === undefined) {
                            av.push(null);
                            continue;
                        }
                        var sv = String(raw);
                        if (sv.indexOf('@handle:') === 0) {
                            var oh =
                                inst_get(parseInt(sv.substring(8), 10));
                            if (!oh)
                                throw new Error('instance handle ' +
                                    'gone (GC): ' + sv);
                            av.push(oh);
                        } else if (sv === 'true') {
                            av.push(true);
                        } else if (sv === 'false') {
                            av.push(false);
                        } else if (/^-?\d+$/.test(sv)) {
                            av.push(parseInt(sv, 10));
                        } else if (/^-?(\d+\.\d*|\.\d+)([eE][-+]?\d+)?$/.test(sv)) {
                            av.push(parseFloat(sv));
                        } else {
                            av.push(sv);
                        }
                    }
                }
                var inst = jc.$new.apply(jc, av);
                var h = inst_put(inst);
                var lbl = '';
                try { lbl = String(inst); }
                catch (eL) { lbl = '(toString failed)'; }
                if (lbl.length > 160) lbl = lbl.substring(0, 157) + '...';
                sendMsg('instanceCreated', { class: cls, str: lbl,
                                             handle: h });
            } catch (e) {
                sendMsg('error', { fn: 'createInstance',
                    msg: String(e.message || e) +
                         ' (check the constructor arguments)' });
            }
        });
    }
};
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
var gSmaliFactory = null;
function smali_factory() {
    if (gSmaliFactory) return gSmaliFactory;
    Java.perform(function () {
        try {
            var ctx = null;
            try {
                var AT = Java.use('android.app.ActivityThread');
                ctx = AT.currentApplication();
            } catch (e0) { ctx = null; }
            if (!ctx) return;
            var files = ctx.getFilesDir().getAbsolutePath();
            var jar = files + '/smali-fat.jar';
            var File = Java.use('java.io.File');
            if (!File.$new(jar).exists()) {
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
            File.$new(jar).setReadOnly();
            var PC = Java.use('dalvik.system.PathClassLoader');
            var CL = Java.use('java.lang.ClassLoader');
            var loader = PC.$new(jar, CL.getSystemClassLoader());
            gSmaliFactory = Java.ClassFactory.get(loader);
        } catch (e) {
            agentLog('smali factory: ' + String(e.message || e));
            gSmaliFactory = null;
        }
    });
    return gSmaliFactory;
}
function smali_compile(src) {
    var out = { err: '', dex: null };
    Java.perform(function () {
        try {
            var F = smali_factory();
            if (!F) { out.err = 'no app context / smali-fat.jar unusable'; return; }
            var ctx = Java.use('android.app.ActivityThread').currentApplication();
            var files = ctx.getFilesDir().getAbsolutePath();
            var File = Java.use('java.io.File');
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
var gBak = null;
function baksmali_libs() {
    if (gBak) return gBak;
    try {
        var F = smali_factory();
        if (!F) return null;
        gBak = {
            DexFileFactory: F.use('org.jf.dexlib2.DexFileFactory'),
            Opcodes:        F.use('org.jf.dexlib2.Opcodes'),
            Baksmali:       F.use('org.jf.baksmali.Baksmali'),
            Opts:           F.use('org.jf.baksmali.BaksmaliOptions'),
            CFH:            F.use('org.jf.util.ClassFileNameHandler'),
            StrRef:         F.use('org.jf.dexlib2.iface.reference.StringReference'),
            RefIns:         F.use('org.jf.dexlib2.iface.instruction.ReferenceInstruction'),
            StrVal:         F.use('org.jf.dexlib2.iface.value.StringEncodedValue')
        };
        return gBak;
    } catch (e) {
        agentLog('baksmali libs: ' + String(e.message || e));
        return null;
    }
}
function apk_paths() {
    var out = [];
    try {
        var AT = Java.use('android.app.ActivityThread');
        var app = AT.currentApplication();
        if (app) {
            var ai = app.getApplicationInfo();
            out.push(String(ai.sourceDir));
            var sp = ai.splitSourceDirs;
            if (sp) for (var i = 0; i < sp.length; i++) out.push(String(sp[i]));
        }
    } catch (e) {}
    return out;
}
var gDexes = null, gDexesAt = 0;
function dex_files() {
    var now = Date.now();
    if (gDexes && now - gDexesAt < 60000) return gDexes;
    var L = baksmali_libs();
    if (!L) return null;
    var out = [];
    var paths = apk_paths();
    var opc = L.Opcodes.getDefault();
    for (var i = 0; i < paths.length; i++) {
        try {
            var cont = L.DexFileFactory.loadDexContainer(
                Java.use('java.io.File').$new(paths[i]), opc);
            var it = cont.getDexFiles().iterator();
            while (it.hasNext()) out.push(it.next());
        } catch (e) {}
    }
    gDexes = out;
    gDexesAt = now;
    return out;
}
function is_system_cls(n) {
    return n.indexOf('android.') === 0 ||
           n.indexOf('androidx.') === 0 ||
           n.indexOf('com.android.') === 0 ||
           n.indexOf('com.google.') === 0 ||
           n.indexOf('java.') === 0 ||
           n.indexOf('javax.') === 0 ||
           n.indexOf('dalvik.') === 0 ||
           n.indexOf('kotlin.') === 0 ||
           n.indexOf('kotlinx.') === 0;
}
function ins_const_string(L, ins) {
    try {
        var op = String(ins.getOpcode());
        if (op.indexOf('CONST_STRING') !== 0) return '';
        var ri = Java.cast(ins, L.RefIns);
        var ref = ri.getReference();
        var sr = Java.cast(ref, L.StrRef);
        return String(sr.getString());
    } catch (e) { return ''; }
}
function page_out(list, qid, total) {
    var PAGE = 400;
    var pages = Math.max(1, Math.ceil(list.length / PAGE));
    var q = (typeof qid === 'number') ? qid : 0;
    var idx = 0;
    var send_page = function () {
        var chunk = list.slice(idx, idx + PAGE);
        idx += PAGE;
        sendMsg('classList', {
            qid: q, page: Math.floor((idx - PAGE) / PAGE),
            pages: pages, total: total,
            shown: Math.min(idx, list.length),
            classes: chunk
        });
        if (idx < list.length)
            setTimeout(send_page, 15);
    };
    send_page();
}
var SCAN = { on: false, qid: -1, f: '', md: 0, want: {}, wantList: [],
             matches: [], hits: {}, dexes: null, di: 0, it: null,
             total: 0,
             mn: '', pt: '', rt: '', sys: false,
             list: null, ri: 0 };   /* r33 F5: reflection scan */
var gLastScanHits = {};
var gLastSearch = { f: '', mode: 0 };
function scan_finish() {
    SCAN.on = false;
    gLastScanHits = SCAN.hits;
    var matches = SCAN.matches.slice().sort();
    sendMsg('info', { msg: 'search: ' + matches.length +
                           ' matching class(es) (scan done)' });
    page_out(matches, SCAN.qid, SCAN.total);
}
function scan_match(pat, s) {
    /* r33 F5: pattern match honoring the case-sensitive flag */
    if (!pat) return true;
    return SCAN.cs
        ? s.indexOf(pat) !== -1
        : s.toLowerCase().indexOf(String(pat).toLowerCase()) !== -1;
}
function scan_step() {
    if (!SCAN.on) return;
    /* r33 F5: method(1)/field(2) searches run on Java reflection -
       no baksmali/dexlib2 dependency (the old path died silently
       wherever smali-fat.jar failed to load). Mode 3 (code text)
       still needs the disassembler and says so when it is gone. */
    if (SCAN.md === 1 || SCAN.md === 2) {
        try {
            if (!SCAN.list) {
                var allL = Java.enumerateLoadedClassesSync();
                if (!SCAN.sys)
                    allL = allL.filter(function (n) {
                        return !is_system_cls(n); });
                allL.sort();
                SCAN.list = allL;
                SCAN.ri = 0;
            }
            var budget = 30;
            while (budget-- > 0) {
                if (SCAN.ri >= SCAN.list.length ||
                    SCAN.matches.length >= 4000) {
                    scan_finish();
                    return;
                }
                var cls = SCAN.list[SCAN.ri++];
                try {
                    var jc = Java.use(cls);
                    var mh = [], fh = [], matched = false;
                    if (SCAN.md === 1) {
                        var ms = jc.class.getDeclaredMethods();
                        for (var i = 0; i < ms.length; i++) {
                            var mn = String(ms[i].getName());
                            if (!scan_match(SCAN.mn, mn)) continue;
                            if (SCAN.pt) {
                                var ps = ms[i].getParameterTypes();
                                var pk = false;
                                for (var p = 0; p < ps.length; p++)
                                    if (scan_match(SCAN.pt,
                                            String(ps[p].getName()))) {
                                        pk = true;
                                        break;
                                    }
                                if (!pk) continue;
                            }
                            if (SCAN.rt && !scan_match(SCAN.rt,
                                    String(ms[i].getReturnType()
                                        .getName()))) continue;
                            matched = true;
                            mh.push(mn);
                        }
                    } else {
                        var fs = jc.class.getDeclaredFields();
                        for (var j2 = 0; j2 < fs.length; j2++) {
                            var fn = String(fs[j2].getName());
                            if (scan_match(SCAN.fRaw, fn)) {
                                matched = true;
                                fh.push(fn);
                            }
                        }
                    }
                    if (matched) {
                        SCAN.matches.push(cls);
                        SCAN.hits[cls] = { m: mh, f: fh };
                    }
                } catch (eC) {}
            }
            setTimeout(scan_step, 15);
        } catch (eR) {
            SCAN.on = false;
            sendMsg('error', { fn: 'listLoadedClasses',
                               msg: String(eR.message || eR) });
        }
        return;
    }
    var L = baksmali_libs();
    if (!L) { SCAN.on = false;
              sendMsg('error', { fn: 'listLoadedClasses',
                  msg: 'dexlib2 unavailable (smali-fat.jar failed)' });
              return; }
    try {
        if (!SCAN.dexes) SCAN.dexes = dex_files();
    } catch (eD) { SCAN.on = false;
              sendMsg('error', { fn: 'listLoadedClasses',
                  msg: String(eD.message || eD) });
              return; }
    if (!SCAN.dexes || !SCAN.dexes.length) {
        SCAN.on = false;
        sendMsg('info', { msg: 'search: app dex files not found' });
        page_out([], SCAN.qid, SCAN.total);
        return;
    }
    var budget = 150;
    while (budget-- > 0) {
        if (SCAN.matches.length >= 4000) { scan_finish(); return; }
        if (SCAN.di >= SCAN.dexes.length) { scan_finish(); return; }
        if (!SCAN.it) {
            try {
                SCAN.it = SCAN.dexes[SCAN.di].getClasses().iterator();
            } catch (eI) { SCAN.di++; continue; }
        }
        if (!SCAN.it.hasNext()) { SCAN.di++; SCAN.it = null; continue; }
        var cd = null;
        try { cd = SCAN.it.next(); } catch (eN) { continue; }
        var tn = '';
        try { tn = String(cd.getType()); } catch (eT) { continue; }
        if (tn.length < 3) continue;
        var cls = tn.substring(1, tn.length - 1);
        if (!SCAN.want[cls]) continue;
        var matched = false, mh = null, fh = null;
        try {
            if (SCAN.md === 1) {                       /* method name */
                var mit = cd.getMethods().iterator();
                while (mit.hasNext()) {
                    var mm = mit.next();
                    var mn = String(mm.getName());
                    if (scan_hit(mn)) {
                        matched = true;
                        mh = [mn];
                        break;
                    }
                }
            } else if (SCAN.md === 2) {                /* field name */
                var fit = cd.getFields().iterator();
                while (fit.hasNext()) {
                    var ff = fit.next();
                    var fn = String(ff.getName());
                    if (scan_hit(fn)) {
                        matched = true;
                        fh = [fn];
                        break;
                    }
                }
            } else {                                   /* code text   */
                var m2 = cd.getMethods().iterator();
                while (m2.hasNext()) {
                    var m3 = m2.next();
                    var impl = null;
                    try { impl = m3.getImplementation(); } catch (e0) {}
                    if (!impl) continue;
                    var iit = impl.getInstructions().iterator();
                    while (iit.hasNext()) {
                        var s = ins_const_string(L, iit.next());
                        if (s && scan_hit(s)) {
                            matched = true;
                            var nm = String(m3.getName());
                            if (!mh) mh = [];
                            if (mh.indexOf(nm) === -1 && mh.length < 40)
                                mh.push(nm);
                            break;
                        }
                    }
                    if (matched) break;
                }
                if (!fh) fh = [];
                if (!matched || fh.length < 40) {
                    var sfit = null;
                    try { sfit = cd.getStaticFields().iterator(); }
                    catch (e1) {}
                    while (sfit && sfit.hasNext()) {
                        var sf = sfit.next();
                        var sfn = '';
                        try { sfn = String(sf.getName()); } catch (e2) {}
                        try {
                            var ev = sf.getInitialValue();
                            if (ev) {
                                var sv = Java.cast(ev, L.StrVal).getValue();
                                if (sv && scan_hit(String(sv))) {
                                    matched = true;
                                    if (fh.indexOf(sfn) === -1 && fh.length < 40)
                                        fh.push(sfn);
                                }
                            }
                        } catch (e3) {}
                    }
                }
            }
        } catch (eScan) {}
        if (matched) {
            SCAN.matches.push(cls);
            SCAN.hits[cls] = { m: mh || [], f: fh || [] };
        }
    }
    setTimeout(scan_step, 15);
}
function read_file_text(path) {
    var FIS = Java.use('java.io.FileInputStream');
    var fis = FIS.$new(path);
    var all = [], chunk = Java.array('byte', new Array(8192).fill(0));
    var k;
    while ((k = fis.read(chunk)) > 0) {
        for (var c = 0; c < k; c++) all.push(chunk[c]);
    }
    fis.close();
    return Java.use('java.lang.String').$new(
        Java.array('byte', all), 'UTF-8');
}
function rm_rf(f) {
    try {
        if (f.isDirectory()) {
            var ls = f.listFiles();
            if (ls) for (var i = 0; i < ls.length; i++) rm_rf(ls[i]);
        }
        f.delete();
    } catch (e) {}
}
function extract_method_blocks(text, method) {
    var lines = text.split('\n');
    var out = [], cur = null;
    for (var i = 0; i < lines.length; i++) {
        var ln = lines[i];
        if (cur === null) {
            if (ln.indexOf('.method') === 0) {
                var body = ln.substring(8);
                var po = body.indexOf('(');
                if (po !== -1) {
                    var head = body.substring(0, po).trim();
                    var sp = head.lastIndexOf(' ');
                    var nm = (sp === -1) ? head : head.substring(sp + 1);
                    if (nm === method) cur = [ln];
                }
            }
        } else {
            cur.push(ln);
            if (ln.indexOf('.end method') === 0) {
                out.push(cur.join('\n'));
                cur = null;
                if (out.join('\n').length > 60000) break;
            }
        }
    }
    return out.join('\n\n');
}
recv(function onMsg(msg) {
    if (!msg) { recv(onMsg); return; }
    try {
        if (msg.type === 'rpc' && msg.fn && rpc.exports[msg.fn]) {
            var a = msg.args || {};
            var names = FN_ARGS[msg.fn] || [];
            var vals = names.map(function (n) { return a[n]; });
            rpc.exports[msg.fn].apply(rpc.exports, vals);
        }
    } catch (e) {
        sendMsg('error', { fn: 'recv', msg: String(e.message || e) });
    }
    recv(onMsg);
});
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
    listLoadedClasses: ['filter', 'qid', 'mode', 'sys', 'cs',
                        'mn', 'pt', 'rt'],   /* r33 F5 */
    getMethodSrc:    ['class', 'method'],
    getMethodIO:     ['class', 'method'],
    setMethodIO:     ['class', 'method', 'args', 'ret'],
    openClass:       ['class'],
    readStaticFields: ['class'],
    showIme:         [],
    hideIme:         [],
    listInstances:   ['class'],
    setInstanceField:['class', 'index', 'field', 'value', 'handle'],
    callMethod:      ['class', 'method', 'args', 'handle', 'count', 'main'],
    createInstance:  ['class', 'args'],  /* r33 F8 */
    listConstructors: ['class'],         /* r33 F8 */
    dumpLoadedClasses: [],              /* r30 C4 */
    dumpObject:      ['class', 'handle'],/* r32 R1: FN_ARGS resolves
                                           by JSON key and the menu
                                           sends "class" - r30 wired
                                           the export's parameter
                                           name ('cls'), so the dump
                                           stayed dead */
    releaseHandle:   ['handle']         /* r31 M7 */
};
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
    var pkgR = '', densR = 1.0, dpiR = 0;
    try {
        var ATr = Java.use('android.app.ActivityThread');
        var appR = ATr.currentApplication();
        if (appR) {
            pkgR = String(appR.getPackageName());
            try {
                var dm = appR.getResources().getDisplayMetrics();
                densR = parseFloat(String(dm.density)) || 1.0;
                dpiR  = parseInt(String(dm.densityDpi)) || 0;
            } catch (eDm) {}
        }
    } catch (eP) {}
    /* r35 B: density lets the menu size the UI in real dp */
    sendMsg('ready', { msg: 'Dex-tool agent r35 loaded', pkg: pkgR,
                       density: densR, dpi: dpiR });
    agentLog('phase: ready sent');
}, 0);
