# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r26)

Field spec: a full menu RESTRUCTURE. Same engine and RPCs - the
UI is reorganized around windows and the exact tab set the spec
names.

- **Main tabs, exact order:** Classes, Patches, Trackers, Hooks,
  Recorder, Monitor, Script, Settings (was Settings-first with
  "Tracker").
- **Secondary bar:** the opened classes are tabs under the main
  bar; the far-right **+** button opens a NEW Class Explorer
  window. Tapping a class tab raises that class's window.
- **Class Explorer windows:** package/family dropdown (com.game,
  com.android, ... derived from the loaded list) with an **All**
  checkbox, the search field + **Filter** button, and a pink
  **Options** button whose popup holds **Case sensitive**,
  **Filter by Class / Method / Field**, the advanced **code-text**
  search (same baksmali const-string scan as r25) and the System
  classes gate. The filtered class list fills the window; tapping
  a class opens its Inspector. Case sensitivity now reaches the
  agent (listLoadedClasses gained a `cs` argument - mode 0 AND
  the dex scan honor it).
- **Class Inspector windows** (one per opened class, floating,
  movable): **Inspector** button (opens the Object Finder for the
  class), **Static Fields** section (values read on demand), the
  Methods list and the full Fields list. **-** collapses the
  window, **X** closes it.
- **Method expansion:** tapping a method opens the 4 spec
  sub-tabs in place:
  - **Caller** - one input per parameter (primitives int32/
    int64/float/double/string/boolean/byte are plain text;
    OBJECT parameters get a **Find...** button that opens the
    Object Finder for the parameter's class), the **Call count**
    input, **Call** and **Call in Main Thread** buttons (the
    agent loops and runs each call via
    Java.scheduleOnMainThread), and a per-call results log
    ("call 3/10: ..."). Object args travel as {"handle":N} and
    callMethod passes the retained instance as the REAL object
    - text-only args could never do that.
  - **Patcher** - manual value, **Patch NOP** (auto return by
    method logic) and **Redirect** (forces the return value on
    every subsequent call), plus the r25 live IO-probe editor.
  - **Tracer** - track this method + live event log.
  - **Hooker** - Java/Smali/Nop hook editor for the method.
- **Method context menu:** right-click (or the "..." button on
  touch) offers Open in new window (duplicate Inspector), Full
  Method View (real smali in its own window), Copy Method Name
  and Copy Class Name (clipboard + an on-screen echo - the touch
  bridge has no system clipboard backend).
- **Object Finder window:** **Find Objects** (Java.choose),
  **Create Object** ($new() through the no-arg constructor,
  retained like a scan match), a manual reference field and a
  results list in the spec's "Name (i/N)" shape. A click fills
  the caller parameter that opened it or selects the object for
  the Inspector/Instances view.
- r25 kept: frame pump, method source view, one-tap Nop, IO
  patching, network monitor detail window, green code-text marks,
  sorted filtered class list.

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
