'use strict';

var patches  = {};
var hooks    = {};
var trackers = {};
var recOn    = false;
var monOn    = false;

function sendMsg(type, payload) {
    send(JSON.stringify({ type: type, payload: payload }));
}

// ── FIX: Java touch hook ───────────────────────────────────────
// AInputQueue_getEvent in touch.cpp only fires for NativeActivity apps.
// For standard Java Activity apps (99% of apps), we hook at the Java level:
// Activity.dispatchTouchEvent is called once per touch event at the top
// of the view hierarchy, then calls dextool_on_touch in libtool.so.
function installTouchHook() {
    try {
        var sym = Module.findExportByName('libtool.so', 'dextool_on_touch');
        if (!sym) {
            sendMsg('warn', { msg: 'dextool_on_touch symbol not found in libtool.so' });
            return;
        }
        // void dextool_on_touch(float x, float y, int action)
        var nativeTouch = new NativeFunction(sym, 'void', ['float', 'float', 'int']);

        Java.perform(function() {
            // Primary: hook at Activity level (one call per gesture)
            try {
                var Activity = Java.use('android.app.Activity');
                Activity.dispatchTouchEvent.implementation = function(ev) {
                    try {
                        nativeTouch(ev.getX(), ev.getY(), ev.getAction() & 0xff);
                    } catch(e2) {}
                    return this.dispatchTouchEvent(ev);
                };
                sendMsg('info', { msg: 'Touch hook: Activity.dispatchTouchEvent OK' });
            } catch(e) {
                // Fallback: some apps subclass Dialog or use a custom Window
                try {
                    var Window = Java.use('android.view.Window');
                    Window.superDispatchTouchEvent.implementation = function(ev) {
                        try {
                            nativeTouch(ev.getX(), ev.getY(), ev.getAction() & 0xff);
                        } catch(e3) {}
                        return this.superDispatchTouchEvent(ev);
                    };
                    sendMsg('info', { msg: 'Touch hook: Window.superDispatchTouchEvent (fallback) OK' });
                } catch(e2) {
                    sendMsg('warn', { msg: 'Touch hook failed: ' + e.message });
                }
            }
        });
    } catch(e) {
        sendMsg('error', { fn: 'installTouchHook', msg: e.message });
    }
}

installTouchHook();

// ── RPC exports ───────────────────────────────────────────────
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
                if (t==='int'||t==='long'||t==='short'||t==='byte')
                    f.set(null, parseInt(value));
                else if (t==='boolean')
                    f.set(null, value === 'true');
                else if (t==='float'||t==='double')
                    f.set(null, parseFloat(value));
                else
                    f.set(null, value);
                patches[name] = { value: value, enabled: true };
                sendMsg('patchApplied', { name: name });
            });
        } catch(e) { sendMsg('error', { fn: 'addPatch', msg: e.message }); }
    },

    removePatch: function(name) {
        delete patches[name];
        sendMsg('patchRemoved', { name: name });
    },

    setPatchEnabled: function(name, enabled) {
        if (patches[name]) patches[name].enabled = enabled;
        sendMsg('patchUpdated', { name: name, enabled: enabled });
    },

    addHook: function(name, mode, code) {
        try {
            Java.perform(function() {
                var parts  = name.split('.');
                var method = parts.pop();
                var cls    = parts.join('.');
                var jcls   = Java.use(cls);
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
                } else if (mode === 'Smali') {
                    sendMsg('smaliRequest', { name: name, code: code });
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

recv(function onMsg(msg) {
    if (msg && msg.type === 'rpc' && rpc.exports[msg.fn]) {
        var a = msg.args || {};
        rpc.exports[msg.fn].apply(rpc.exports, Object.values(a));
    }
    recv(onMsg);
});

sendMsg('ready', { msg: 'Dex-tool agent loaded' });
