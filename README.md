# Dex-tool


## What this is

Dex-tool is a runtime inspector for Android apps: a Frida-GumJS agent
plus an ImGui overlay injected into the host process. It browses
loaded classes, inspects and patches static/instance fields, traces
method calls, calls methods, creates instances, hooks methods (Java
source, smali skeleton or the one-tap Patch NOP), dumps classes and
objects to file, and records/monitors network + Activity traffic.
Touch-first menu: single-tap toggle button, drag-to-move, edge-drag
resize, IME-aware text fields.

## How to build

The workflow `.github/workflows/build.yml` IS the source of truth: it
writes `src/`, `agent/agent.js`, `sample-app/` and this README from
inline heredocs, builds `libtool.so` + the sample APK with the NDK and
commits the generated tree back. Hand edits under `src/` are
overwritten by the next CI run - change the heredocs, not the tree.
(The r36 review removed the dead `src/smali_runtime.{cpp,h}` pair -
they were referenced by nothing since r22's "dead code removed"
entry but the files themselves survived every prune.)

## Known limitations

- One recursive mutex guards most shared UI state; the JS thread can
  still stall for the duration of a frame (r31 removed the frame-pump
  stall via an atomic mirror; full per-container ownership remains
  future work).
- The overlay needs a GL context on the screen it draws on; on
  context loss it parks itself without GL calls and re-inits on the
  next one (r30).



Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Changes in this version (r36) - review fixes

Logic and UI fixes from the external review; every one verified
against the tree before patching.

- **L1 (menu.cpp)** five render-thread writes to `s_trackers` /
  `s_hooks` (`Track this method`, `Add Hook`, `Remove hook`, the two
  field-`Track` buttons, the object-tab `Track`) ran WITHOUT the menu
  mutex while the JS thread mutates the same containers under `g_mtx`
  - a `trackerEvent` arriving mid-render could invalidate the
  `Tracker&`/`HookRow&` the frame was holding. All wrapped in
  `MLK()/MULK()` now.
- **L2 (menu.cpp)** the FAB double-toggle: on a fast tap the ImGui
  click (`Button` returns true) AND the release-branch both toggled
  `s_visible` in the same frame, netting zero - the menu looked-like-a-
  no-op. The release branch now only fires when the button was never
  pressed this gesture.
- **L3 (menu.cpp)** the instance-field editor used ONE static
  `nv[32][128]` shared by every open class window (keyed by
  class+gen+index, so class B's editor silently adopted class A's
  text on selection switches). The buffers moved into `ClassTab`
  (per window), synced back every frame.
- **L4 (menu.cpp)** `s_hits` (hook hit counters) grew without bound -
  capped at 512 entries now.
- **U1 (menu.cpp)** the resize handles enforced a raw `420 px` left
  minimum and no right/bottom minimum at all, while r35 moved every
  other minimum to dp - on a 1440p phone the window could be dragged
  3x smaller than the layout supports. All three edges clamp to the
  same dp minimums as the size constraints.
- **U2 (menu.cpp)** the minimize button sat at a raw `-56/+3 px`
  corner offset - 3x misplaced on density-3 phones (overlapping the
  collapse arrow). dp-sized and measured from its own text width now.
- **L5 (agent.js)** the frame pump hard-required `IME.act`; a host
  whose touches never pass `Activity.dispatchTouchEvent`
  (native-activity games) never captured an activity, so the pump
  stalled forever and the FAB/menu never rendered. The pump resolves
  the activity on demand now.
- **L6 (agent.js)** `listInstances` retained EVERY heap match before
  the 20-row cap check - a class with thousands of instances pinned
  that many JNI global refs and only then stopped. The cap is
  checked first; instances past 20 are never retained.
- **L7 (overlay.cpp)** `g_font_size`/`g_font_dirty` were plain
  globals written by the JS thread (density arrival) and read by the
  render thread - a torn bool/float pair. Both are `std::atomic` now.
- **L8 (touch.cpp)** the NativeActivity touch path forwarded
  view-LOCAL `getX/getY`, but the touch map and the Java-path hook
  both work in SCREEN coords, so taps were offset by the view's
  on-screen position. The raw screen axes
  (`AMOTION_EVENT_AXIS_RAWX/RAWY`) are used when the NDK defines
  them (r19+); older NDKs keep the view-local values, now
  documented.
- **Files removed:** `src/smali_runtime.{cpp,h}` - dead since r22
  (never compiled, never generated, only referenced by comments).
- **Not applied, with reasons:** `callMethod`'s first-matching
  overload selection (same arity picks overload #1) is a known
  approximation - the UI sends no type tags, and a full
  typed-argument editor is a feature, not a fix; `mon_origin`'s
  per-URL origin cache is a documented approximation too.

## Usage

1. Drop libtool.so into lib/arm64-v8a/ and lib/armeabi-v7a/.
2. Inject System.loadLibrary("tool") into Application.onCreate()
   or the launcher Activity.
3. Copy smali-fat.jar into assets/.
4. Re-sign and install.

## Components

    frida-gum 16.7.19   QuickJS / GumJS scripting runtime
    Dobby (LSPosed)     inline function hook framework
    ImGui 1.93.0-docking  immediate-mode overlay GUI (ES2 mode)
    smali 3.0.7         runtime .smali to DEX assembler (bundled JAR)
