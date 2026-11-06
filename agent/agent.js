'use strict';

var patches  = {};
var hooks    = {};
var trackers = {};
var recOn    = false;
var monOn    = false;

function sendMsg(type, payload) {
    send(JSON.stringify({ type: type, payload: payload }));
}

// Native symbols exported by libtool.so
var symTouch  = Module.findExportByName('libtool.so', 'dextool_on_touch');
var symKey    = Module.findExportByName('libtool.so', 'dextool_on_key');
var symText   = Module.findExportByName('libtool.so', 'dextool_on_text');
var symBksp   = Module.findExportByName('libtool.so', 'dextool_on_backspace');

var fnTouch = symTouch ? new NativeFunction(symTouch, 'void', ['float','float','int']) : null;
var fnKey   = symKey   ? new NativeFunction(symKey,   'void', ['int','int'])            : null;
var fnText  = symText  ? new NativeFunction(symText,  'void', ['pointer'])              : null;
var fnBksp  = symBksp  ? new NativeFunction(symBksp,  'void', [])                       : null;

// ── Track current foreground Activity ──────────────────────
var currentActivity = null;
Java.perform(function() {
    try {
        var Activity = Java.use('android.app.Activity');
        Activity.onResume.implementation = function() {
            currentActivity = this;
            this.onResume();
        };
        Activity.onPause.implementation = function() {
            if (currentActivity === this) currentActivity = null;
            this.onPause();
        };
    } catch (e) {
        sendMsg('warn', { msg: 'activity track hook failed: ' + e.message });
    }
});

// ── Touch hook (raw screen coordinates) ───────────────────
function installTouchHook() {
    if (!fnTouch) {
        sendMsg('warn', { msg: 'dextool_on_touch not found' });
        return;
    }
    Java.perform(function() {
        try {
            var Activity = Java.use('android.app.Activity');
            Activity.dispatchTouchEvent.implementation = function(ev) {
                try {
                    // getRawX/Y = absolute screen pixels (matches EGL surface)
                    fnTouch(ev.getRawX(), ev.getRawY(), ev.getAction() & 0xff);
                } catch(e2) {}
                return this.dispatchTouchEvent(ev);
            };
            sendMsg('info', { msg: 'Touch hook OK (getRawX/Y)' });
        } catch (e) {
            sendMsg('warn', { msg: 'touch hook failed: ' + e.message });
        }
    });
}
installTouchHook();

// ── Hardware key hook ──────────────────────────────────────
function installKeyHook() {
    if (!fnKey) return;
    Java.perform(function() {
        try {
            var Activity = Java.use('android.app.Activity');
            Activity.dispatchKeyEvent.implementation = function(ev) {
                try {
                    fnKey(ev.getKeyCode(), ev.getAction());
                } catch(e2) {}
                return this.dispatchKeyEvent(ev);
            };
        } catch (e) {
            sendMsg('warn', { msg: 'key hook failed: ' + e.message });
        }
    });
}
installKeyHook();

// ── Soft keyboard (hidden EditText + InputConnection hooks) ─
var imeEditText   = null;
var imeViewGroup  = null;
var TextWatcherCls = null;

function buildTextWatcher() {
    if (TextWatcherCls) return TextWatcherCls;
    var TW = Java.use('android.text.TextWatcher');
    TextWatcherCls = Java.registerClass({
        name: 'DexToolTW',
        implements: [TW],
        methods: {
            beforeTextChanged: function(s, st, c, a) {},
            onTextChanged:     function(s, st, b, c) {},
            afterTextChanged: function(s) {}
        }
    });
    return TextWatcherCls;
}

function showKeyboard() {
    if (!currentActivity || imeEditText) return;
    var act = currentActivity;
    Java.perform(function() {
        try {
            var EditText   = Java.use('android.widget.EditText');
            var ViewGroup  = Java.use('android.view.ViewGroup$LayoutParams');
            var decor      = act.getWindow().getDecorView();
            imeViewGroup   = decor;
            imeEditText    = EditText.$new(act);
            imeEditText.setAlpha(0);
            imeEditText.setSingleLine(true);
            imeEditText.setInputType(0x00000001); // TYPE_CLASS_TEXT
            var lp = ViewGroup.$new(1, 1);
            decor.addView(imeEditText, lp);
            imeEditText.requestFocus();
            var imm = Java.use('android.view.inputmethod.InputMethodManager');
            var immInst = act.getSystemService('input_method');
            immInst.showSoftInput(imeEditText, 0);
            sendMsg('info', { msg: 'IME shown' });
        } catch (e) {
            sendMsg('error', { fn: 'showKeyboard', msg: e.message });
        }
    });
}

function hideKeyboard() {
    if (!currentActivity) return;
    var act = currentActivity;
    Java.perform(function() {
        try {
            var imm = Java.use('android.view.inputmethod.InputMethodManager');
            var immInst = act.getSystemService('input_method');
            if (imeEditText) {
                try { immInst.hideSoftInputFromWindow(imeEditText.getWindowToken(), 0); } catch(e) {}
                try { imeViewGroup.removeView(imeEditText); } catch(e) {}
                imeEditText = null;
            }
            sendMsg('info', { msg: 'IME hidden' });
        } catch (e) {
            sendMsg('error', { fn: 'hideKeyboard', msg: e.message });
        }
    });
}

// Hook InputConnection so soft-keyboard text reaches ImGui
function installImeInputHooks() {
    if (!fnText && !fnBksp) return;
    Java.perform(function() {
        try {
            var BaseIC = Java.use('android.view.inputmethod.BaseInputConnection');
            BaseIC.commitText.implementation = function(text, ncp) {
                try {
                    if (imeEditText && text && text.length() > 0) {
                        var s = Memory.allocUtf8String(text.toString());
                        fnText(s);
                    }
                } catch(e) {}
                return this.commitText(text, ncp);
            };
            BaseIC.deleteSurroundingText.implementation = function(before, after) {
                try {
                    if (imeEditText && fnBksp) {
                        for (var i = 0; i < (before | 0); i++) fnBksp();
                    }
                } catch(e) {}
                return this.deleteSurroundingText(before, after);
            };
            sendMsg('info', { msg: 'IME input hooks OK' });
        } catch (e) {
            sendMsg('warn', { msg: 'IME hooks failed: ' + e.message });
        }
    });
}
installImeInputHooks();

// ── RPC exports ────────────────────────────────────────────
rpc.exports = {
    addPatch: function(name, value) {
        try {
            Java.perform(function() {
                var parts = name.split('.');
                var field = parts.pop();
                var cls   = parts.join('.');
                var jcls  = Java.use(cls);
                var f     = jcls.class.getDeclaredField(field);
                f.setAccessible(true);
                var t = f.getType().getName();
                if (t==='int'||t==='long'||t==='short'||t==='byte') f.set(null, parseInt(value));
                else if (t==='boolean') f.set(null, value === 'true');
                else if (t==='float'||t==='double') f.set(null, parseFloat(value));
                else f.set(null, value);
                patches[name] = { value: value, enabled: true };
                sendMsg('patchApplied', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'addPatch', msg: e.message }); }
    },
    removePatch: function(name) { delete patches[name]; sendMsg('patchRemoved', { name: name }); },
    setPatchEnabled: function(name, enabled) {
        if (patches[name]) patches[name].enabled = enabled;
        sendMsg('patchUpdated', { name: name, enabled: enabled });
    },
    addHook: function(name, mode, code) {
        try {
            Java.perform(function() {
                var parts = name.split('.'); var method = parts.pop();
                var jcls  = Java.use(parts.join('.'));
                if (mode === 'noop') {
                    jcls[method].implementation = function() {
                        sendMsg('hookFired', { name: name, mode: 'noop' });
                        return null;
                    };
                } else if (mode === 'Java') {
                    var fn = new Function('Java', 'send', 'args', code);
                    jcls[method].implementation = function() {
                        return fn(Java, send, Array.from(arguments));
                    };
                }
                hooks[name] = { mode: mode, enabled: true };
                sendMsg('hookAdded', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'addHook', msg: e.message }); }
    },
    removeHook: function(name) {
        try {
            Java.perform(function() {
                var parts = name.split('.'); var m = parts.pop();
                Java.use(parts.join('.'))[m].implementation = null;
                delete hooks[name];
                sendMsg('hookRemoved', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'removeHook', msg: e.message }); }
    },
    setHookEnabled: function(name, enabled) {
        if (hooks[name]) hooks[name].enabled = enabled;
        sendMsg('hookUpdated', { name: name, enabled: enabled });
    },
    addTracker: function(name) {
        try {
            Java.perform(function() {
                var parts = name.split('.'); var m = parts.pop();
                var jcls  = Java.use(parts.join('.'));
                jcls[m].implementation = function() {
                    var args = Array.from(arguments).map(String);
                    var ret;
                    try { ret = this[m].apply(this, arguments); } catch(e) { ret = null; }
                    sendMsg('trackerEvent', { name: name, args: args, ret: String(ret) });
                    return ret;
                };
                trackers[name] = true;
                sendMsg('trackerAdded', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'addTracker', msg: e.message }); }
    },
    removeTracker: function(name) {
        try {
            Java.perform(function() {
                var parts = name.split('.'); var m = parts.pop();
                Java.use(parts.join('.'))[m].implementation = null;
                delete trackers[name];
                sendMsg('trackerRemoved', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'removeTracker', msg: e.message }); }
    },
    startRecorder: function() {
        recOn = true;
        Java.perform(function() {
            Java.enumerateLoadedClassesSync().forEach(function(cls) {
                try {
                    var jcls = Java.use(cls);
                    jcls.class.getDeclaredMethods().forEach(function(md) {
                        var mn = md.getName();
                        try {
                            jcls[mn].overloads.forEach(function(ovl) {
                                ovl.implementation = function() {
                                    if (!recOn) { ovl.implementation = null; return this[mn].apply(this, arguments); }
                                    sendMsg('recorded', { cls: cls, method: mn, args: Array.from(arguments).map(String) });
                                    return this[mn].apply(this, arguments);
                                };
                            });
                        } catch(ignored) {}
                    });
                } catch(ignored) {}
            });
        });
        sendMsg('recorderStarted', {});
    },
    stopRecorder: function() { recOn = false; sendMsg('recorderStopped', {}); },
    startMonitor: function() {
        monOn = true;
        Java.perform(function() {
            try {
                var SSLCtx = Java.use('javax.net.ssl.SSLContext');
                SSLCtx.init.overload(
                    '[Ljavax.net.ssl.KeyManager;',
                    '[Ljavax.net.ssl.TrustManager;',
                    'java.security.SecureRandom'
                ).implementation = function(km, tm, sr) {
                    sendMsg('sslInit', { note: 'SSLContext.init intercepted' });
                    return this.init(km, tm, sr);
                };
            } catch(e) {}
            try { Java.use('okhttp3.OkHttpClient'); sendMsg('monitorInfo', { note: 'OkHttp detected' }); } catch(e) {}
        });
        sendMsg('monitorStarted', {});
    },
    stopMonitor: function() { monOn = false; sendMsg('monitorStopped', {}); },
    evalScript: function(src) {
        try { sendMsg('scriptResult', { result: String(eval(src)) }); }
        catch(e) { sendMsg('scriptError', { msg: e.message }); }
    },
    openClass: function(className) {
        try {
            Java.perform(function() {
                var jcls = Java.use(className);
                sendMsg('classInfo', {
                    class:   className,
                    methods: jcls.class.getDeclaredMethods().map(function(m) { return m.getName(); }),
                    fields:  jcls.class.getDeclaredFields().map(function(f)  { return f.getName(); })
                });
            });
        } catch(e) { sendMsg('error', { fn: 'openClass', msg: e.message }); }
    },
    listInstances: function(className) {
        try {
            Java.perform(function() {
                var list = [];
                Java.choose(className, {
                    onMatch:    function(i) { list.push(String(i)); if (list.length >= 20) return 'stop'; },
                    onComplete: function()  { sendMsg('instanceList', { class: className, instances: list }); }
                });
            });
        } catch(e) { sendMsg('error', { fn: 'listInstances', msg: e.message }); }
    },
    hookMethod:  function(cls, m) { this.addHook(cls + '.' + m, 'noop', ''); },
    trackMethod: function(cls, m) { this.addTracker(cls + '.' + m); },
    patchField:  function(cls, f) { sendMsg('info', { msg: 'patchField: ' + cls + '.' + f }); }
};

// ── Message loop ───────────────────────────────────────────
function onMsg(msg) {
    if (!msg) return;
    if (msg.type === 'keyboard') {
        if (msg.show) showKeyboard(); else hideKeyboard();
    } else if (msg.type === 'rpc' && rpc.exports[msg.fn]) {
        var a = msg.args || {};
        try { rpc.exports[msg.fn].apply(rpc.exports, Object.values(a)); }
        catch(e) { sendMsg('error', { fn: msg.fn, msg: e.message }); }
    }
    recv(onMsg);
}
recv(onMsg);

sendMsg('ready', { msg: 'Dex-tool agent v1.1 loaded' });
