'use strict';

var patches={}, hooks={}, trackers={};
var recOn=false, monOn=false;

function sendMsg(type, payload) {
    send(JSON.stringify({ type: type, payload: payload }));
}

// Touch offset correction
var gOffX=0, gOffY=0;

function updateOffset(activity) {
    try {
        var cv = activity.getWindow().getDecorView().findViewById(0x01020002);
        if (!cv) return;
        var loc = Java.array('int', [0,0]);
        cv.getLocationInWindow(loc);
        gOffX = loc[0]; gOffY = loc[1];
        sendMsg('info', { msg: 'Touch offset: ('+gOffX+','+gOffY+')' });
    } catch(e) {}
}

// Keyboard bridge
var gET=null, gIMM=null, gClearing=false;

function setupKeyboardBridge(activity) {
    Java.scheduleOnMainThread(function() {
        try {
  var ET     = Java.use('android.widget.EditText');
  var FL     = Java.use('android.widget.FrameLayout');
  var LP     = Java.use('android.widget.FrameLayout$LayoutParams');
  var TW     = Java.use('android.text.TextWatcher');
  var IMMCls = Java.use('android.view.inputmethod.InputMethodManager');
  var Editable= Java.use('android.text.Editable');
  var CS     = Java.use('java.lang.CharSequence');

  var et = ET.$new(activity);
  et.setAlpha(0.0);
  et.setFocusableInTouchMode(true);

  var params = LP.$new(1,1);
  Java.cast(activity.getWindow().getDecorView(), FL).addView(et, params);

  // TextWatcher: forward characters via send() (no NativeFunction)
  var WatcherCls = Java.registerClass({
      name: 'com.dextool.app.DxtoolKbd_'+Date.now(),
      implements: [TW],
      methods: {
          beforeTextChanged: function(s,st,c,af){},
          onTextChanged: function(s,st,before,count) {
              if (gClearing) return;
              try {
                  if (count > before) {
                      var txt = Java.cast(s, CS).toString();
                      var added = txt.substring(st+before, st+count);
                      for (var i=0; i<added.length; i++) {
                          // Send as JSON — C++ parse_int extracts 'cp'
                          send(JSON.stringify({
                              type: 'char',
                              cp:   added.charCodeAt(i)
                          }));
                      }
                  } else if (before > count) {
                      for (var i=0; i<before-count; i++) {
                          send(JSON.stringify({ type: 'backspace' }));
                      }
                  }
              } catch(e2) {}
          },
          afterTextChanged: function(e) {
              if (!gClearing) {
                  gClearing = true;
                  try { Java.cast(e, Editable).clear(); } catch(e2){}
                  gClearing = false;
              }
          }
      }
  });
  et.addTextChangedListener(WatcherCls.$new());
  gET  = et;
  gIMM = Java.cast(activity.getSystemService('input_method'), IMMCls);
  sendMsg('info', { msg: 'Keyboard bridge OK' });
        } catch(e) {
  sendMsg('warn', { msg: 'setupKeyboardBridge: '+e.message });
        }
    });
}

// Touch hook — sends JSON {type:'touch',x,y,action} via send()
// C++ on_message routes to overlay_on_touch() — NO NativeFunction
function installTouchHook() {
    try {
        Java.perform(function() {
  var Activity = Java.use('android.app.Activity');

  Activity.onWindowFocusChanged.implementation = function(has) {
      if (has) {
          updateOffset(this);
          if (!gET) setupKeyboardBridge(this);
      }
      return this.onWindowFocusChanged(has);
  };

  Activity.dispatchTouchEvent.implementation = function(ev) {
      try {
          send(JSON.stringify({
              type:   'touch',
              x:      ev.getX() - gOffX,
              y:      ev.getY() - gOffY,
              action: ev.getAction() & 0xff
          }));
      } catch(e2) {}
      return this.dispatchTouchEvent(ev);
  };
  sendMsg('info', { msg: 'Touch hook installed' });
        });
    } catch(e) {
        sendMsg('error', { fn: 'installTouchHook', msg: e.message });
    }
}

installTouchHook();

rpc.exports = {
    addPatch: function(name,value) {
        try {
  Java.perform(function(){
      var parts=name.split('.'); var field=parts.pop();
      var jcls=Java.use(parts.join('.'));
      var f=jcls.class.getDeclaredField(field);
      f.setAccessible(true);
      var t=f.getType().getName();
      if(t==='int'||t==='long'||t==='short'||t==='byte')
          f.set(null,parseInt(value));
      else if(t==='boolean') f.set(null,value==='true');
      else if(t==='float'||t==='double') f.set(null,parseFloat(value));
      else f.set(null,value);
      patches[name]={value:value,enabled:true};
      sendMsg('patchApplied',{name:name});
  });
        } catch(e){ sendMsg('error',{fn:'addPatch',msg:e.message}); }
    },
    removePatch:    function(n){ delete patches[n]; sendMsg('patchRemoved',{name:n}); },
    setPatchEnabled:function(n,en){ if(patches[n])patches[n].enabled=en; sendMsg('patchUpdated',{name:n,enabled:en}); },

    addHook: function(name,mode,code) {
        try {
  Java.perform(function(){
      var parts=name.split('.'); var method=parts.pop();
      var jcls=Java.use(parts.join('.'));
      if(mode==='noop'){
          jcls[method].implementation=function(){
              sendMsg('hookFired',{name:name,mode:'noop'}); return null;
          };
      } else if(mode==='Java'){
          var fn=new Function('Java','send','args',code);
          jcls[method].implementation=function(){
              return fn(Java,send,Array.from(arguments));
          };
      } else if(mode==='Smali'){
          sendMsg('smaliRequest',{name:name,code:code});
      }
      hooks[name]={mode:mode,enabled:true};
      sendMsg('hookAdded',{name:name});
  });
        } catch(e){ sendMsg('error',{fn:'addHook',msg:e.message}); }
    },
    removeHook: function(name) {
        try {
  Java.perform(function(){
      var parts=name.split('.'); var m=parts.pop();
      Java.use(parts.join('.'))[m].implementation=null;
      delete hooks[name]; sendMsg('hookRemoved',{name:name});
  });
        } catch(e){ sendMsg('error',{fn:'removeHook',msg:e.message}); }
    },
    setHookEnabled: function(n,en){ if(hooks[n])hooks[n].enabled=en; sendMsg('hookUpdated',{name:n,enabled:en}); },

    addTracker: function(name) {
        try {
  Java.perform(function(){
      var parts=name.split('.'); var m=parts.pop();
      var jcls=Java.use(parts.join('.'));
      jcls[m].implementation=function(){
          var args=Array.from(arguments).map(String); var ret;
          try{ret=this[m].apply(this,arguments);}catch(e){ret=null;}
          sendMsg('trackerEvent',{name:name,args:args,ret:String(ret)});
          return ret;
      };
      trackers[name]=true; sendMsg('trackerAdded',{name:name});
  });
        } catch(e){ sendMsg('error',{fn:'addTracker',msg:e.message}); }
    },
    removeTracker: function(name) {
        try {
  Java.perform(function(){
      var parts=name.split('.'); var m=parts.pop();
      Java.use(parts.join('.'))[m].implementation=null;
      delete trackers[name]; sendMsg('trackerRemoved',{name:name});
  });
        } catch(e){ sendMsg('error',{fn:'removeTracker',msg:e.message}); }
    },

    startRecorder: function(){
        recOn=true;
        Java.perform(function(){
  Java.enumerateLoadedClassesSync().forEach(function(cls){
      try{
          var jcls=Java.use(cls);
          jcls.class.getDeclaredMethods().forEach(function(md){
              var mn=md.getName();
              try{
                  jcls[mn].overloads.forEach(function(ovl){
                      ovl.implementation=function(){
                          if(!recOn){ovl.implementation=null;return this[mn].apply(this,arguments);}
                          sendMsg('recorded',{cls:cls,method:mn,args:Array.from(arguments).map(String)});
                          return this[mn].apply(this,arguments);
                      };
                  });
              }catch(ig){}
          });
      }catch(ig){}
  });
        });
        sendMsg('recorderStarted',{});
    },
    stopRecorder: function(){ recOn=false; sendMsg('recorderStopped',{}); },

    startMonitor: function(){
        monOn=true;
        Java.perform(function(){
  try{
      var SSLCtx=Java.use('javax.net.ssl.SSLContext');
      SSLCtx.init.overload(
          '[Ljavax.net.ssl.KeyManager;',
          '[Ljavax.net.ssl.TrustManager;',
          'java.security.SecureRandom'
      ).implementation=function(km,tm,sr){
          sendMsg('sslInit',{note:'SSLContext.init intercepted'});
          return this.init(km,tm,sr);
      };
  }catch(e){}
  try{Java.use('okhttp3.OkHttpClient');sendMsg('monitorInfo',{note:'OkHttp detected'});}catch(e){}
        });
        sendMsg('monitorStarted',{});
    },
    stopMonitor: function(){ monOn=false; sendMsg('monitorStopped',{}); },

    evalScript: function(src){
        try{ sendMsg('scriptResult',{result:String(eval(src))}); }
        catch(e){ sendMsg('scriptError',{msg:e.message}); }
    },

    openClass: function(className){
        try{
  Java.perform(function(){
      var jcls=Java.use(className);
      sendMsg('classInfo',{
          class:className,
          methods:jcls.class.getDeclaredMethods().map(function(m){return m.getName();}),
          fields: jcls.class.getDeclaredFields().map(function(f){return f.getName();})
      });
  });
        }catch(e){ sendMsg('error',{fn:'openClass',msg:e.message}); }
    },

    listInstances: function(className){
        try{
  Java.perform(function(){
      var list=[];
      Java.choose(className,{
          onMatch:function(i){list.push(String(i));if(list.length>=20)return 'stop';},
          onComplete:function(){sendMsg('instanceList',{class:className,instances:list});}
      });
  });
        }catch(e){ sendMsg('error',{fn:'listInstances',msg:e.message}); }
    },

    hookMethod:  function(cls,m){ this.addHook(cls+'.'+m,'noop',''); },
    trackMethod: function(cls,m){ this.addTracker(cls+'.'+m); },
    patchField:  function(cls,f){ sendMsg('info',{msg:'patchField: '+cls+'.'+f}); }
};

recv(function onMsg(msg){
    if(!msg){ recv(onMsg); return; }
    if(msg.type==='rpc'&&rpc.exports[msg.fn]){
        var a=msg.args||{};
        rpc.exports[msg.fn].apply(rpc.exports,Object.values(a));
    } else if(msg.type==='showKeyboard'){
        Java.scheduleOnMainThread(function(){
  try{ if(gET&&gIMM){ gET.requestFocus(); gIMM.showSoftInput(gET,1); } }catch(e){}
        });
    } else if(msg.type==='hideKeyboard'){
        Java.scheduleOnMainThread(function(){
  try{
      if(gET&&gIMM){
          var b=gET.getWindowToken();
          if(b) gIMM.hideSoftInputFromWindow(b,0);
      }
  }catch(e){}
        });
    }
    recv(onMsg);
});

sendMsg('ready',{msg:'Dex-tool agent loaded'});
