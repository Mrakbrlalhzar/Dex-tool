'use strict';
/* Dex-tool agent (r11). All Java logic lives here (frida-java-bridge);
   libtool.so only feeds input/GL. Messages both ways are JSON via
   send()/recv(); the C++ menu decodes them in menu.cpp.

   r11 changes (field issues from the r10 run):
   - keyboard: the hidden-EditText bridge is GONE. String(getText()) is
     an android.text.Editable, whose String() form is "[object
     Object]" - which is exactly what got typed into every field. The
     on-screen keyboard is now drawn natively by libtool.so; this side
     only consumes kbd-area taps so they do not fall through to the app.
   - classes: listLoadedClasses streams the FULL result in pages of 400
     with a qid; the r10 version silently capped at 800 names, so the
     tab never displayed all classes.
   - recorder: Start passes {filter, cap}; only classes of the app's
     own package (or the given prefix) are hooked, and installation
     stops at cap methods. Hooking every method of every loaded class
     exhausted the 32-bit process and it died with SIGABRT.
   - touch map: measured on Activity.onResume AND on every touch
     (throttled); onWindowFocusChanged never fired on the field host.
*/

function sendMsg(type, payload) { send({ type: type, payload: payload }); }
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

/* measure where the drawing view sits inside the window and tell the
   overlay. r11: also called from Activity.onResume (onWindowFocusChanged
   never fired on the field host) and throttled from every touch, so a
   relayout at any point in the app's life still updates the map. */
var gMapSent = { w: -1, h: -1, x: -9999, y: -9999 };
var gMapLastTouch = 0;
function report_map(view) {
    try {
        if (!view) return;
        var loc = Java.array('int', [0, 0]);
        view.getLocationInWindow(loc);
        var w = view.getWidth(), h = view.getHeight();
        if (w === gMapSent.w && h === gMapSent.h &&
            loc[0] === gMapSent.x && loc[1] === gMapSent.y) return;
        if (w <= 0 || h <= 0) return;
        gMapSent = { w: w, h: h, x: loc[0], y: loc[1] };
        gSetMap(loc[0], loc[1], w, h);
        sendMsg('info', { msg: 'touch map ' + loc[0] + ',' + loc[1] + ' ' + w + 'x' + h });
    } catch (e) {}
}
function content_view(activity) {
    try {
        /* 0x01020002 = android.R.id.content */
        return activity.getWindow().getDecorView()
                        .findViewById(0x01020002);
    } catch (e) { return null; }
}

/* ---------------- input + view mapping on the host activity -------- */
function installTouchHook() {
    if (!ensure_native_fns()) {
        sendMsg('error', { fn: 'installTouchHook', msg: 'libtool.so natives missing' });
        return;
    }
    Java.perform(function () {
        var Activity = Java.use('android.app.Activity');
        Activity.onResume.implementation = function () {
            var r = this.onResume();
            try { report_map(content_view(this)); } catch (e) {}
            return r;
        };
        Activity.dispatchTouchEvent.implementation = function (ev) {
            /* MAIN THREAD */
            try {
                var act = ev.getAction() & 0xff;
                var down = (act === 0);
                /* raw window coords first: the overlay maps them */
                gNativeTouch(ev.getX(), ev.getY(), act);
                if (act === 2) {          /* throttle map refresh on moves */
                    var now = Date.now();
                    if (now - gMapLastTouch > 2000) {
                        gMapLastTouch = now;
                        report_map(content_view(this));
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
        sendMsg('info', { msg: 'touch hook installed (r11)' });
    });
}
installTouchHook();

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

/* ---------------- recorder ----------------
   r11: scoped. Only classes matching the package filter are hooked and
   installation stops at cap methods; the r10 version hooked every
   method of every class in the process (tens of thousands of ArtMethod
   hook slots on a 32-bit process) and died with SIGABRT mid-install. */
var REC = { on: false, busy: false, ring: [], installed: 0, flushBusy: false };
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
    sendMsg('recorderProgress', {
        installed: REC.installed, total: list.length - REC.idx });
    if (REC.idx < list.length && REC.installed < REC.cap) {
        setTimeout(function () { rec_install_chunk(list, batch); }, 30);
    } else {
        REC.busy = false;
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
            sendMsg('recorderProgress', { installed: 0, total: list.length });
            sendMsg('info', { msg: 'recorder scope: filter="' + REC.filter +
                                   '" cap=' + REC.cap });
            setTimeout(function () { rec_install_chunk(list, 25); }, 50);
        });
    },
    stopRecorder: function () {
        REC.on = false;
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
                sendMsg('classInfo', {
                    class: cls,
                    methods: method_lines(jc),
                    fields: field_lines(jc, null)
                });
            } catch (e) {
                sendMsg('classInfo', { class: cls, methods: [], fields: [] });
                sendMsg('error', { fn: 'openClass', msg: String(e.message || e) });
            }
        });
    },
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
        /* r11: show/hideKeyboard are gone; the on-screen keyboard is
           native. Unknown types fall through silently. */
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
    listInstances:   ['class'],
    setInstanceField:['class', 'index', 'field', 'value'],
    callMethod:      ['class', 'method', 'args']
};

sendMsg('ready', { msg: 'Dex-tool agent r11 loaded' });
