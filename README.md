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
commits the generated tree back. Hand edits under `src/` (except
`src/smali_runtime.{cpp,h}`, which CI does not generate since
r21) are overwritten by the next CI run - change the heredocs,
not the tree.

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

## Changes in this version (r34) - touch input + docking

- **Single-tap tabs**: the ImGui context now sets
  `ImGuiConfigFlags_IsTouchScreen`, `io.MouseDragThreshold = 15.0f`
  and `io.TouchExtraPadding = (2, 2)` at creation. Finger jitter
  beyond the old 6 px default was turning taps into drags, which
  canceled the click - tabs (and everything else) now need one
  tap. The injection path was verified while at it: touches go
  straight through `AddMousePosEvent` / `AddMouseButtonEvent` and
  IME text through `AddInputCharacter`, with no artificial
  delays.
- **Search field reset**: two causes fixed. `AutoSelectAll` is
  removed from every IME text field - on touch a re-tap selected
  the whole word so the next letter replaced it. And the agent now
  ignores the delayed watcher callback of its own programmatic
  EditText clear (a 400 ms echo window); that echo used to reach
  the menu as a burst of backspaces, wiping what you typed. The
  class search uses `InputTextWithHint` with a stable id,
  persistent buffer and continuous submission; there is no
  `SetKeyboardFocusHere` anywhere.
- **Class list count and phantom scroll**: the row filter now runs
  first, the counter shows the filtered count (`X of Y` where X is
  the number of rows actually rendered), and `ImGuiListClipper` is
  initialized with the filtered size - previously it clipped over
  the raw size, which reserved scroll height for rows that never
  rendered (the "748 of 748" / empty-void behavior). Empty names
  are never rendered.
- **Dockable inspector**: the build now uses the imgui
  `v1.90.4-docking` branch with `ImGuiConfigFlags_DockingEnable`.
  A `DockSpace` strip sits at the bottom of the main window (the
  tabs stay at the top and the class list stays visible); every
  new inspector is queued with `DockBuilderDockWindow` and docks
  there on its first frame. Dragging its tab out undocks it into
  a floating window and dragging it back re-docks - native ImGui
  docking, no manual attach code. A **Fill / Restore** button in
  the inspector toolbar toggles fullscreen and re-docks on
  restore. "Open in new window" opens floating without docking.
  All docking code is guarded with `#ifdef IMGUI_HAS_DOCK`, so the
  build still compiles if the docking headers are unavailable.


## Changes in this version (r33) - the UI round

User-reported issues; the class-browser flow is rebuilt around
independent, detachable windows.

- **Search box keyboard bug**: the class search field used
  ImGuiInputTextFlags_EnterReturnsTrue, which runs through the
  r24 "enter then release field" path - every letter after the
  first was dropped and the box cleared on re-tap. The flag is
  gone; the box now behaves like the family search box.
- **Dynamic search**: class rows filter as you type (client-side,
  like the family box); the Filter button re-queries the agent.
- **Independent windows**: results are routed by qid to the window
  that sent the search - searching in one window no longer changes
  the others. No window is opened by default; no "[id]" numbering
  (the id is a hidden suffix).
- **Method / field / string search**: modes 1 and 2 now scan via
  Java reflection - no baksmali dependency (the old path died
  silently wherever smali-fat.jar failed to load) - and support
  method-name + parameter-type + return-type filters (Options
  popup when Method is selected). Mode 3 (code text) still needs
  the disassembler and reports honestly when it is unavailable.
- **Class windows**: opening a class pops a window attached below
  the window it came from. Dragging or long-pressing the title
  detaches it; the title menu re-attaches it below the main menu
  or any open class-list window. Class rows have a long-press
  menu (open in new window / copy class name) and a copy button.
- **TreeNode layout**: the class window renders the class name as
  a root TreeNode (DefaultOpen | SpanAvailWidth | FramePadding)
  with Instances / Static Fields / Methods / Fields sub-nodes and
  indented children; the horizontal button row is gone. Buttons
  sit in a clean toolbar row separated by Separator; inputs use
  -FLT_MIN widths.
- **Find instances**: a floating popup with a filter box and a
  scrollable result list; picking a row closes the popup.
- **Create instance**: a popup lists the class's constructors
  with a search box; each parameter gets an input; @handle:
  tokens, numbers and booleans are coerced agent-side. The
  null-object shortcut, the manual text box and the Save buttons
  are removed.
- **Family window** closes after picking a family.
- **Main window**: a minimize button sits in the title bar next
  to the X; the title+tab strip is no longer a scroll region
  (single taps on tabs land immediately - the reported
  double-tap-to-open issue); the window no longer scrolls its top
  bar away; the Manual add inputs fit the width.
- **Detached class-list windows**: long-press a tab (0.6 s, then
  drag) to float it out of the tab bar; long-press a floating
  title to attach it back to the main menu.


## Fixes in this version (r32)

- **R1** dumpObject in FN_ARGS was keyed by the export's parameter
  name ('cls'), but the dispatcher resolves by JSON key and the
  menu sends "class" - the per-object dump has been dead since
  r30. Now ['class', 'handle'].
- **R2** r31 regression: the Settings "Menu visible" checkbox wrote
  s_visible while menu_draw read only the atomic mirror, so the
  checkbox did nothing. s_visible is now std::atomic<bool> itself
  and the mirror is gone - one variable, every writer updates it.
- **R3** the crash log names the faulting module again: the module
  map is snapshotted at install time (blocking is fine there) and
  the handler only READS the snapshot, which stays async-signal-
  safe - no dladdr, no file I/O in-handler.
- **R4** crash_ux() takes a capacity bound; the crash line buffer
  grew to 320 bytes so the restored module fields fit.
- **R5** imgui_park_without_gl() documents its backend-data leak
  and counts parks (Info panel: "GL context parks").
- **R6** the JSON helpers parse each message once: a mutex-guarded
  one-entry cache keyed on the payload reuses the tree for
  repeated lookups instead of re-parsing per lookup.
- **R7** the automatic class listing now sends "cs" like manual
  searches, so the Case-sensitive toggle is consistent.
- **R8** the object-dump popup preview shows the agent's real
  filename (class with dots replaced, hex handle).
- **R9** the CI hand-edit warning excludes src/smali_runtime.{cpp,h}
  (hand-maintained since r21); the README notes the exception.
- **R10** closing a class tab releases the handles its Instances
  list retained (skipping handles with open object tabs); removing
  a Saved Object releases its handle. The retain leak is closed on
  all three paths.
- The push back to dev is gated on the canonical repository, so a
  fork running this workflow cannot rewrite its own tree.
- The review's C3 cleanup note was re-checked and declined: the
  'nop' / 'noop' prefixes come from two different UI sources and
  are both needed; the code now carries a comment saying so.


## Fixes in this version (r31)

- **M4** the menu's substring JSON helpers (jstr, jnum, jhas, jarr,
  jtype, jobjarr, junesc) are replaced by a real minimal JSON tree
  parser: keys resolve to the SHALLOWEST match, so a payload like
  {"inner":{"class":"B"},"class":"A"} can no longer return "B";
  escapes are handled once, UTF-16 surrogate pairs encode as real
  emoji, and parse depth is capped at 64 against hostile input.
  Verified by a standalone harness covering the review's cases,
  emoji, depth bombs and malformed input.
- **M7** closing an object tab now posts releaseHandle - the agent
  Java.release()s / $dispose()s the retained instance instead of
  pinning it until the 200-entry LRU evicts it.
- **M8 (partial)** menu_frame_pump_level() no longer takes the menu
  mutex: s_visible is mirrored into a std::atomic<bool>, so the JS
  thread's frame pump can never stall behind a whole-frame render
  lock. Full per-container ownership remains future work.
- **Structural** the workflow now warns when a push modifies files
  under the generated paths (src/, agent/, sample-app/, README.md) -
  the heredocs overwrite those edits in the same run. It is a
  warning, not a failure: a yml-only update (the normal flow) passes
  silently, and the initial all-zeros push is skipped.


## Fixes in this version (r30)

Applied from the external code review (dex-tool-review.md); every
finding was verified against the tree before patching.

- **C1** the hit-flash overlay loop runs under the menu mutex now -
  flash_hit() pushes from the JS thread and the loop iterated and
  erased s_flash unlocked (use-after-free on realloc).
- **C2** s_classes, s_objtabs, s_explorers, s_trackers, s_rec, s_mon
  and s_cons moved to std::deque: opening a class tab from inside a
  method-arg pick used to push_back into s_classes while the whole
  call chain still held a ClassTab& (use-after-free). deque keeps
  references stable across push_back.
- **C3** the agent prefix-matches hook modes ("Nop (auto)" reached
  do_addHook un-mapped and fell through silently) and do_addHook
  reports hookFailed for unknown modes. The one-tap Nop itself
  already existed ("Patch NOP" button) and keeps working.
- **C4** dumpObject / dumpLoadedClasses are in FN_ARGS - the
  per-object "Dump to file" used to call dumpObject(undefined,
  undefined) and always report "handle gone (GC)".
- **H3** the stale OpenPopup("fmPopup") trigger and its dead twin are
  gone, together with the s_open_fmview flag; the context menu sets
  s_fm_open directly.
- **H4** a legacy font= value is honored again (the scale is derived
  from it) instead of being parsed and then discarded.
- **H5** touch-queue overflow clears the whole pending gesture (never
  a lone DOWN) and MOVE events no longer fabricate a button press, so
  the virtual mouse cannot get stuck down after a burst.
- **H6** on context loss the overlay parks ImGui without issuing any
  glDelete* - ForgetDeviceObjects against a foreign context was
  deleting the host app's GL objects.
- **M1** the tool no longer dup2()s the host app's stdout/stderr into
  the crash log; the app keeps its console.
- **M2** the crash signal handler is async-signal-safe: manual hex
  formatting + write(), no snprintf and no dladdr (both can deadlock
  on the stdio/loader locks during the very crash being reported).
- **M5** logger_init / logger_retarget create the full directory
  chain - a missing directory used to silence all logging.
- **M7** setInstanceField without a valid handle now reports
  "instance handle gone (GC)" instead of re-running Java.choose and
  patching whatever object the old scan index resolved to.
- **M9** apply_theme() runs only when the theme changes.
- junesc() pairs UTF-16 surrogate escapes before encoding, so emoji
  in agent output survive.
- **Not applied, with reasons:** H1/M3/M6 cite src/smali_runtime.cpp
  which does not exist in this project (the smali path is agent-side
  JS); H2's malformed-JSON mechanism does not reproduce
  (raw_json_string is escape-boundary correct and the raw form is
  valid JSON when re-embedded); M4a (real JSON parser), M7a (retain
  policy) and M8 (lock scope) are deferred as larger refactors; the
  CI-regeneration structural change would break this project's
  yml-heredoc delivery model and stays as documented above.


## Fixes in this version (r29)

The fix-spec round: a code review plus the six reported UI issues.
No new library was needed - ImGui + Frida cover everything.

- **Single tap works.** The MENU/HIDE toggle ignored taps that
  complete within one frame: Button returned pressed into an empty
  handler and the toggle ran only through the multi-frame drag path,
  so a second tap was needed. The press now toggles directly;
  dragging the button still only moves it.
- **Resize by dragging the edges.** Left/right/bottom edge strips
  (12 px, highlighted while touched) resize the menu; the top bar
  stays the move handle. The 420x300 minimum and the 95% screen
  maximum are kept.
- **Class Families is a window.** The family dropdown became a
  movable, resizable Class Families window (340x420 on first use)
  with the search box and an All shortcut inside.
- **Full Method View is a window.** Same conversion - movable,
  resizable, scrollbar kept.
- **Instance and Inspector merged.** The Object Finder popup and the
  Inspector button are gone; Find instances / Create instance /
  manual handle entry / Saved Objects all live in the Inspector's
  Instances sub-tab. Picking an instance for a method argument opens
  that class's Instances tab with a green "picking for argN" banner
  and a Cancel pick button.
- **Lists follow the menu size.** Recorder, Monitor, console, trace,
  source and Call-Results boxes now take a fraction of the remaining
  window height (fit_h) instead of fixed pixel heights, so nothing
  needs menu resizing to be seen.
- **Cleanup.** Duplicated forward declarations removed; the
  ObjFinder struct and the s_open_objfinder deferred-popup path
  deleted with the merge; instanceCreated now appends to the class
  tab's instance list directly.


## Fixes in this version (r28)

The menu was reshaped into the structure of the reference source the
user supplied (the IL2CPP Tool by mIsmanXP - Main.cpp, ClassesTab.cpp,
Tool.cpp), adapted to the Frida dex tool. Same engine, RPCs and
agent; menu.cpp carries the restructure, agent.js adds five small
things (hookHit, dumpLoadedClasses, dumpObject, the package name in
ready, the r28 banner).

- **One window, main tabs:** Tools / Tracer / Patches / Dumper /
Recorder / Monitor / Script / Settings - the reference's
Tools / Tracer / Dumper / Settings skeleton with the project's own
tabs kept after it.
- **Tools:** the reference "Fullscreen" checkbox + the inner tabber
(AutoSelectNewTabs | FittingPolicyScroll | TabListPopupButton) with
the LEADING "+" tab button, closeable Class Explorer tabs
("Classes" / "Classes [N]" / the filter text), one tab per opened
class and one "[0x..]" tab per inspected object (the reference
DrawTabMap). The r26 floating windows are gone - everything lives
in tabs and popups like the real tool.
- **Methods:** TREE NODES with the reference coloring - traced
methods green with a "Hit Count N | " prefix - and the three
reference sub-tabs Call / Patch / Trace (the r27 Hook editor merged
into Trace; the context menu, long-press and "..." button kept).
- **Tracer tab:** "Traced method count", the Quick Restore popup and
the traced list (trackers with their event log, hooks with their
enable/edit controls); the r27 Trackers + Hooks managers merged in
under "Manual add" so no feature was lost.
- **Hit flashes:** the agent counts every call through an explicit
hook/tracker layer (500 ms throttle) and the menu paints
"name (Ndx)" lines on the BACKGROUND draw list - green flash, fade
out - the reference HookerData::visited overlay.
- **Dumper tab:** DUMP writes every loaded class to
dextool-classes.txt in the app files dir (agent rpc
dumpLoadedClasses) with a Done + "Copy path" echo; object tabs dump
one instance as JSON (dumpObject) through the reference
"Dump to file" ProceedPopUp.
- **Object tabs:** click-to-edit field rows, the class's Methods
popup (tap opens the class tab with that method expanded) and the
reference Saved Objects section in the Object Finder popup.
- **Settings:** the reference "Display" section with the 7-step
Scale combo (Smallest..Largest) replacing the free slider (the
opacity / theme / scroll / y-bias knobs stay) and an "Info" section
leading with the package name (new ready payload) + Arch.
- **Popups:** the Object Finder and the Full Method View are popups
now (the reference keeps pickers in popups); the deferred-open
pattern mirrors the reference PopUpSelector.

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

## Fixes in this version (r25)

Field report: the menu worked on one app but HANGS, LAGS and FREEZES
on other apps ("when I touch the app, the touch I had previously
made on the menu works"), plus feature requests for the Hooks,
Monitor and Classes sections.

- **Frame pump (the freeze fix):** the menu is drawn inside the
  host's `eglSwapBuffers` hook - it only advances when the host
  renders a frame. Games render continuously, but most other apps
  render ON DEMAND and can go seconds without swapping; every
  queued menu touch/keystroke just waited for the next swap, which
  is exactly the reported "touch the app and the earlier menu touch
  works". The agent now polls `dextool_want_frames()` (new JNI
  export, 0/1/2) from a 30 ms timer and `postInvalidate()`s the
  captured activity's DecorView while frames are wanted: full rate
  with the menu visible, slow rate (FAB only) while hidden.
  `postInvalidate()` is the thread-safe variant, works on the JS
  thread, and continuous-rendering hosts coalesce the extra
  invalidations.
- **Method source view (Hooks tab + "Src" button):** the real body
  of the method, disassembled on-device by a bundled baksmali
  (org.smali 2.5.2 - baksmali is not on Google Maven for 3.x; the
  org.jf packages coexist with the com.android.tools.smali 3.0.7
  assembler). Smali mode gets a "Copy body -> editor" button. Java
  decompilation is NOT bundled - a decompiler is heavy and would
  run inside the host process, so both modes show the bytecode.
- **One-tap Nop:** the Classes > Methods rows have a Nop button that
  adds a noop hook immediately - it returns the default value for
  the method's return type (void -> plain return, numeric -> 0,
  boolean -> false, object/array -> null). The Hooks mode combo
  label is now "Nop (auto)".
- **Method input/output patching ("IO" button):** arms a one-shot
  probe; the method's next call reports its captured current
  argument values and return value (toString-safe rendering) into
  an editor. Only the checkbox-marked rows are patched from then
  on - arguments are replaced before the original call, the return
  value after. "Refresh values" re-probes, "Stop patching" unhooks.
- **Network monitor:** okhttp calls now log a REQUEST row AND a
  RESPONSE row - the response body is peeked (up to 4 KB, without
  consuming the original) and the request body is read through an
  okio Buffer when accessible; `HttpURLConnection.getInputStream`
  is hooked for the response side of plain-HTTP apps. Clicking any
  row opens a DETAIL window: type, http method, URL, the app
  method that sent/received it (clickable - opens the class), the
  note and the payload preview.
- **Classes tab:** the list is SORTED ALPHABETICALLY and android/
  androidx/com.android/com.google/java/javax/dalvik/kotlin classes
  are HIDDEN by default (a System checkbox brings them back).
  Search has 4 modes: class name, method name, field name and CODE
  TEXT - the last one scans every method's const-string
  instructions and every static String field value of the app's
  dex files (dexlib2, chunked 150 classDefs/15 ms so the agent
  never stalls, capped at 4000 hits, cancellable by a newer
  search). Classes a code-text search reports get their matched
  methods/fields painted GREEN when opened (modes 1/2 mark name
  matches the same way).
- **Known limitations, documented:** only classes inside the host
  APK (base + split APKs) are scannable for source/code-text;
  classes from dynamic `InMemoryDexClassLoader`s are outside the
  window. The monitor still cannot see inside TLS - payload
  previews come from the Java layer (okhttp/URLConnection).

## Fixes in this version (r24)

Patch #3 of the audits: severe performance bugs (the menu froze on
heavier apps - logcat "Skipped 40 frames! The application may be
doing too much work on its main thread") plus logic and CI
leftovers.

- **Field-tracker flooding (agent):** `field_layer_emit` sent a
  `trackerEvent` on EVERY method call of the tracked class, changed
  value or not - hot classes flooded the bridge and froze the menu.
  The field is now polled at most every 20 ms, events are emitted
  only when the value actually changed, and at most once per 100 ms
  per field.
- **`dt_is_loaded` race (agent):** the 3 s loaded-class cache could
  predate a class actually loading, and `loadClass` never fires
  again for a class that is already loaded - the queued install
  waited forever. A cache miss now forces ONE rebuild of the set
  (once per cache generation) before the class is declared
  unloaded.
- **Dispatcher overhead (agent):** the LAYERS key was substring'd
  on every hooked call; class/method names are parsed lazily now
  (only recorder rows need them), and the touch dispatcher keeps
  doing the minimum on the main thread.
- **Eaten gestures no longer leak UP (agent):** `gTouchEat` was
  cleared on UP/CANCEL before the consume check, so the app got an
  UP for a gesture whose DOWN the menu had consumed. The flag is
  captured first, cleared second, consumed last.
- **Enter searches now (menu.cpp + agent):** the class filter
  declares `EnterReturnsTrue`, but the phone IME's Done/Search only
  hid the keyboard. `onEditorAction` forwards an `imeEnter` message
  first; the menu injects a real `ImGuiKey_Enter` press (DOWN, then
  UP next frame) before the field is released.
- **Opacity is background-only (menu.cpp):** `style.Alpha` faded
  everything - text included - on top of the window background
  alpha. `style.Alpha` is gone; `SetNextWindowBgAlpha(s_opacity)`
  is the single opacity source.
- **agentStatus JSON escaping (frida_bridge.cpp):** the GumJS start
  failure text (`err->message`) was pasted into the JSON envelope
  unescaped; quotes inside it produced an envelope the menu could
  not parse. Escaped now.
- **IME ladder stops when the keyboard is up (agent):** every retry
  re-sent `showSoftInput` and logged "showSoftInput sent -> true"
  up to 4x per tap. Retries check `isAcceptingText()` (logged
  once), and each show request carries a session token - a newer
  request or Done/hide invalidates the pending ladder.
- **Console keeps rolling (menu.cpp):** the 400-line guard dropped
  every new line once full, so the console appeared to stop. It
  keeps the newest 400 lines now.
- **Auto-scroll targets the list (menu.cpp):** the Recorder/Script
  auto-scroll ran after `EndChild()` and scrolled the PARENT
  window; it now runs inside the child list.
- **CI:** the push trigger is restricted to `dev` (it fired from
  every branch and pushed to `dev`); the menu title says r24.
- **Known limitation, documented:** frida-java-bridge's `Java.use`
  ALWAYS forces `<clinit>` on a loaded-but-uninitialized class.
  The loadClass deferral only avoids classes that are not loaded
  YET - it cannot avoid initialization of loaded classes, because
  the bridge offers no non-initializing use(). Stated here and in
  the agent source instead of pretending it is fixed.

## Fixes in this version (r23)

Patch #2 of the 47-item audit: the 22 open items plus the 7 partial
ones finished.

- **Message routing (menu.cpp):** every agent message is dispatched on
  the EXACT `"type"` value (`jtype()`); `jhas()` is colon-anchored, so
  a payload that merely contains a word can no longer hijack another
  branch.
- **Click-through (overlay.cpp + agent):** menu.cpp/overlay publish the
  rects of every live ImGui window each frame; the Java touch hook asks
  `dextool_hit_test(rawX, rawY)` BEFORE the app sees the gesture and
  consumes the whole gesture when it starts on the menu - taps no
  longer leak to the host app.
- **Touch map:** the view probe scores SurfaceView/TextureView
  candidates against the overlay's REAL GL surface size
  (`dextool_display_size`); overlay.cpp dropped its clamping, so taps
  outside the view no longer snap onto edge widgets.
- **Hook layering (agent):** one registry (`LAYERS`) per
  class+method - Hook, Tracker, field-Tracker, Recorder and Monitor
  stack on the same method through a single dispatcher instead of
  overwriting each other's `ov.implementation`.
- **Recorder/Monitor Stop really unhooks:** stopping removes the
  layer and restores the original implementation.
- **Instance-method Call:** `listInstances` retains every instance
  (`Java.retain`) and returns `{str, handle, fields:[{n,t,v}]}`;
  `callMethod`/`setInstanceField` address instances by handle - real
  receivers. Static-only calls without a handle are refused with a
  hint. The old `"##"/";"` value protocol (which corrupted values
  containing `|`, `#`, `;`) is gone.
- **Instance editor:** buffers are keyed by class + list generation +
  index - no more stale text across classes.
- **Class initialization:** hooks/trackers only `Java.use` classes
  that are ALREADY loaded; anything else is deferred and installed by
  a `loadClass` watcher when the app loads the class. Known
  limitation (see r24 below): `Java.use` still forces `<clinit>` on
  loaded-but-uninitialized classes - the bridge offers no
  non-initializing use().
- **Tracker side effects:** field/tracker values go through
  `field_repr()` - primitives and boxes only, everything else is
  `@class#identityHashCode`; the app's `toString()` is never called
  inside hooks (no more recursion/crash).
- **Search:** class filter searches on Enter/button only (no more
  per-keystroke `enumerateLoadedClasses`); stale pages of an older
  query are dropped via the qid generation counter.
- **IME focus order:** the OLD field's deferred deactivation no longer
  closes the keyboard of the NEW field (owner-ID check).
- **IME focus restore:** the app's previously focused view is
  remembered and gets focus back when the keyboard closes (Done/back/
  hide) - the hidden EditText no longer keeps focus forever.
- **UI polish:** window opacity applied once (was squared); the X
  button force-returns the toggle FAB when it was disabled.
- **Agent status:** failed GumJS starts are delivered to the menu
  (`agentStatus`) and shown in Settings instead of "waiting" forever.
- **Main thread:** the 500 ms `Thread.sleep` in DexToolApp.onCreate is
  gone.
- **GL state:** vertex-attrib enable flags (0..7) are saved and
  restored around the overlay draw alongside the VAO binding.
- **`my_terminate`:** retires only the slots belonging to the
  terminated display (slots record their `dpy`).
- **Stale GL names:** parked worlds "forget" their backend GL handles
  (`ImGui_ImplOpenGL3_ForgetDeviceObjects`, appended to the backend by
  CI) so adoption can never delete a name the new context recycled;
  owner migration now PARKS the old world instead of leaking it.
- **Build:** Dobby is pinned to a specific commit; `.so`/`.jar`
  binaries and the generated embed header are no longer committed.

## Fixes in this version (r22)

Third-party audit (47 items): the 18 patch-fixed items are ported
INTO the generated sources and merged with the r19-r21 IME fixes
(the audit's raw patch targeted a pre-r19 snapshot - its
ime_on_text hunk would not apply over the r21 code). The audit's
"build committed sources only" workflow was NOT adopted: this
workflow stays self-contained and regenerates the sources, so the
fixes cannot be reverted by a push.

1. Overlay input: ONE ordered queue - deletions keep their order
   before insertions (autocorrect no longer scrambles text), and
   UTF-16 surrogate halves are recombined via
   AddInputCharacterUTF16, so astral characters survive. The agent
   now counts IME deletions in code points (ImGui deletes one
   character per backspace - unit counting over-deleted fields).

2. Overlay GL: the double-draw guard spans the real eglSwapBuffers
   call (libEGL forwards to eglSwapBuffersWithDamageKHR, also
   hooked - the menu was drawn twice per frame); texture binding
   restored on unit 0; owner-migration stale test fixed; no second
   renderer Shutdown when adopting a parked world; dead
   ImFontConfig removed.

3. Logger: FILE* checked under the lock (retarget race), private
   crash log rotated on retarget (dextool-crash.prev.log is real
   now), stdout/stderr follow the new crash fd, the runtime's
   existing sigaltstack is preserved, terminate handler survives
   "no active exception".

4. frida_bridge: early agent messages (the one-shot "ready") are
   queued and replayed when the menu registers; uncaught GumJS
   errors are logged to the crash log and forwarded to the Script
   console; dropped frida_post calls are logged.

5. Agent: Script tab code actually runs (new Function body);
   Java(JS) hooks install (mode strings normalised); orig() keeps
   receiver + args; Patch OFF/delete restores the snapshotted
   original; recorder pre-filters loaded classes; tracker callers
   exclude the tracked method; OkHttp enqueue also monitored;
   readable script console (cstr); smali jar extracted atomically
   + read-only (Android 14) + assembled via ClassFactory with both
   Main class names; single-ByteBuffer InMemoryDexClassLoader
   (minSdk 26); dead code removed (DOTS/DMAX, parse_rules,
   lit_to_java, CONS_Dummy, gDispSize).

6. DexToolApp: crash.prev.log exported, copyLogs synchronized,
   logs also exported to the app-specific external dir (no
   permission needed on Android 11+).

7. CI: the smali fat jar is DEXED with d8 (+ antlr-runtime) - ART
   loads classes.dex, never .class files, and the ANTLR runtime
   was missing, so the Smali mode could not work on-device; 16 KB
   page alignment (-Wl,-z,max-page-size=16384) for Android 15+;
   debug keystore cached so APK updates keep one signature; dead
   smali_runtime.cpp removed from the build.

## Fixes in this version (r21)

1. Field evidence (r20 build, device logs, both failures pinned by
   the r20 stage tags): tap 1 dies with "stage decor.addView:
   decor.addView typeof undefined" on every ladder attempt - the
   wrapper returned by win.getDecorView() does not resolve
   ViewGroup methods on this bridge, so the hidden EditText could
   never be attached. After the addContentView patch the keyboard
   opens and the ladder completes ("bridge up after ladder").

2. Second symptom: the first keystroke typed the literal
   "[object Object]" into the menu, then nothing. The Editable
   handed to afterTextChanged() had no bridged toString either,
   so ed.toString() fell through to QuickJS's
   Object.prototype.toString - it does NOT throw, it silently
   returns that constant, the watcher stores it as IME.prev and
   every later diff comes out empty.

3. Fix 1 (per field fix-spec): the hidden EditText is attached
   via act.addContentView(edit, lp) - a plain public Activity
   method on the same wrapper whose hashCode/getWindow/
   getSystemService all work. The win/decor stages are gone;
   the r19 early guard now probes BOTH act.getWindow and
   act.addContentView before any state mutation.

4. Fix 2 (per field fix-spec, hardened): ime_read_text() re-wraps
   the Editable through Java.cast(ed, CharSequence).toString()
   (the same cast machinery as the proven InputMethodManager
   cast), falls back to a java.lang.Object cast, then to
   CharSequence.toString.call(ed). Every path rejects the
   "[object Object]" poison literal, and a total read failure
   returns null so ime_on_text() SKIPS the delta - the naive
   fallback (cur = '') would have fired backspaces for the whole
   field.

## Fixes in this version (r20)

1. Field evidence (r19 build, device logs): capture and retention
   work ("activity captured & retained via touch hook"), every
   r19 typeof guard passed silently, and each ladder attempt
   dies inside the hidden-EditText factory with
   {"fn":"ime_create_edit","msg":"not a function"}.

2. Why the exact call was still unknown: the GumJS backend is
   QuickJS, and QuickJS reports any broken call as a bare
   "not a function" WITHOUT the callee name (unlike V8's
   "x.y is not a function"). A bare message cannot distinguish
   window.getDecorView() from EditText.$new(),
   FrameLayout$LayoutParams.$new(), decor.addView(),
   Java.registerClass() or the listener installs.

3. Fix: ime_stage(name, fn) wraps every call in the IME path
   (factory + ime_try_show). Each callee is typeof-verified
   before the call and every failure rethrows as
   "stage <name>: <original>", so the next [agent error]
   payload names the exact call (e.g. "stage decor.addView:
   not a function"). Zero behavior change on success.

4. One-shot view-map probe: after the first activity capture the
   touch hook measures the overlay view once and logs
   "ime: view map probe ok" or the exact failure - report_map()
   and find_gl_view() swallow their own errors, so the same
   failure class used to hide there.

## Fixes in this version (r19)

1. Symptom (field report on r18): tapping a text field logged
   `[agent error] ... ime_ensure ... not a function` and the
   keyboard never opened.

2. What the truncated log could not say: the verbatim payload is
   {"type":"error","fn":"<name>","msg":"<message>"}, so it is
   ambiguous whether the TypeError was thrown AT the ime_ensure()
   call site or INSIDE ime_ensure() and reported through its own
   {fn:'ime_ensure'} channel. r19 makes both shapes impossible
   and turns every remaining failure into a named-member report.

3. Factory rename + legacy alias: the hidden-EditText factory is
   now ime_create_edit() (canonical name); ime_ensure() stays
   defined as a DELEGATING alias to it, so any stale ime_ensure()
   reference in any build resolves to the REAL factory. Note:
   an EMPTY stub (function ime_ensure() {}) would make
   ime_try_show() bail on edit === null and the keyboard would
   never open - the alias must delegate.

4. Defensive invocation (fix-spec step 2): ime_try_show() no
   longer calls ime_ensure() directly - it resolves the factory
   through typeof checks (ime_create_edit, falling back to the
   alias) and, if both bindings were ever lost, logs BOTH
   typeofs through the {fn:'ime_try_show'} channel instead of
   throwing a bare "ime_ensure is not a function".

5. Retained-wrapper verification: ime_set_act() probes the
   Java.retain() result for a real Activity method and re-wraps
   it through Java.cast() (global reference + full method
   dispatch) if a bridge ever returned a bare handle object.
   Verified against the frida-java-bridge 5.x sources:
   retain() returns obj.$clone(env) -> new C($h, $t, env) with
   owned=true -> newGlobalRef + method-capable Proxy wrapper,
   so current builds are unaffected by the probe (one typeof).

6. Exact-member diagnostics: ime_create_edit() typeof-guards
   act.getWindow, setBackgroundDrawable/setBackground and
   setAlpha. If a member is missing, the error payload names it
   (e.g. "retained activity lacks act.getWindow (typeof
   undefined)") instead of a bare "X is not a function".

## Fixes in this version (r18)

1. JNI crash / keyboard dead (field report on r17): "JNI DETECTED
   ERROR IN APPLICATION: use of invalid jobject" -> SIGABRT.
   Root cause: the r17 touch-hook capture stored `this` RAW in
   the long-lived IME.act. A Java wrapper handed to a hook
   implementation is only valid for the duration of that
   callback - its JNI reference is a LOCAL reference that dies
   when the callback returns (the frida-java-bridge docs say
   exactly this and provide Java.retain() for it: "Duplicates a
   JavaScript wrapper for later use outside replacement
   method"). The IME logic later calling
   getSystemService()/showSoftInput() on the stale object aborts
   the process: the keyboard never opens and the app dies.

2. Fix: every assignment into IME.act now goes through
   ime_set_act(), which stores a Java.retain()-ed wrapper (a
   GLOBAL JNI reference - valid after the callback returned,
   from any thread) and releases the previous retained activity
   when the tracked one changes, so activity switches never
   leak global references. The bridge does not export
   Java.release(), so cleanup calls the wrapper's $dispose()
   ("eagerly deletes the underlying JNI global reference"),
   preferring Java.release() if a future bridge provides it.

3. Retention applied to ALL capture sources, not just the touch
   hook: the onResume hook, the Application lifecycle callback,
   the ActivityThread.mActivities walk and the Java.choose heap
   scan all hand out equally callback-scoped wrappers - patching
   only the touch hook would have left the same SIGABRT one
   resume away (onResume can even fire before the first touch).

4. Capture logging rebuilt: "ime: activity captured & retained
   via <source>" fires per NEW activity (identity-checked with
   .equals - cheap, once per switch, never per touch event), so
   it covers first capture AND switches. The r17
   once-per-agent flag is retired, and the "IME host activity"
   info line moved into ime_set_act so every source reports
   identically and exactly once.

## Fixes in this version (r17)

1. IME failure, root cause: missing Activity capture in the touch
   hook (field report on r16). The dispatchTouchEvent hook
   intercepted taps, forwarded them to native and could even
   CONSUME them - but it never assigned IME.act. Only the
   onResume hook did, and that hook is lost when the agent
   attaches after the first resume (hot start: no further resume
   fires until the user leaves and returns). ime_show() was then
   thrown back to the fragile ActivityThread.mActivities walk /
   Java.choose heap scan, which on many hosts fails to find the
   resumed activity, so showSoftInput was never called. The
   touch hook now assigns IME.act = this on every event: the tap
   that opens the menu is itself proof of a live, resumed
   activity. Logged once per agent ("ime: activity captured via
   touch hook"), not at event rate.

2. Stale activity selection: find_current_activity_sync() only
   checked the paused/stopped record flags - a finishing or
   destroyed Activity passed, and getSystemService() then
   succeeded while showSoftInput silently no-opped. The walk
   filters dead activities out now (isFinishing()/isDestroyed(),
   not even eligible as the last-resort fallback), and the
   Java.choose heap scan does the same: destroyed instances
   linger until GC, so onMatch now keeps scanning past them and
   only stops at a live one.

3. Weak diagnostics fixed: the "no activity yet" warning
   carries its payload now (walk threw <exception> vs. walk
   empty vs. only dead/paused records), an empty walk logs what
   it saw, ime_try_show() logs a skipped attempt instead of
   returning false silently, showSoftInput's real return value
   is logged, and a final stage line 2.2 s after the ladder
   reports whether the bridge is up - with the reason when it
   is not (no activity captured vs. show kept failing).

4. Kept from r16: the async resolver callback performs the show
   itself (ime_act_resolved -> ime_try_show(false),
   main-thread scheduled, no text reset), and the retry ladder
   resolves through BOTH layers (sync walk first, heap scan
   second).

## Fixes in this version (r16)

1. The keyboard STILL failed to open on some hosts (field report
   on r15). Three residual timing defects, all fixed:
   a) the async resolve_current_activity() callback stored
      IME.act but never attempted the show - when every sync
      retry missed and the heap scan landed after the retry
      ladder was done, showSoftInput never ran. The callback
      performs the show itself now (ime_act_resolved),
      scheduled on the main thread, without resetting text.
   b) the retry ladder ran the SYNCHRONOUS ActivityThread walk
      only, so an activity found only by the async heap scan
      was never picked up. The ladder calls
      resolve_current_activity() now (sync walk first, heap
      scan second).
   c) ime_try_show() called ime_reset_text() on EVERY attempt;
      a retry landing while the user was already typing wiped
      the hidden EditText (and the TextWatcher diff then
      re-sent the surviving text as fresh insertions). Only
      the first attempt of a focus session resets now.

2. Crash-log plumbing (duplicated blocks / wrong directory):
   a) the Java copier appended the WHOLE source file every 5s
      (FileOutputStream(d, true)) - every pulled log held one
      copy per 5s of uptime. It now writes a temp file and
      rename(2)s it over the target: atomic, no duplication,
      no torn reads; files unchanged since the last pass are
      skipped.
   b) JNI_OnLoad runs inside System.loadLibrary, BEFORE
      Application.onCreate finishes - currentApplication() was
      null there, so native logs landed in /sdcard/Download
      (unwritable under scoped storage on modern devices) while
      the copier read filesDir/dextool-logs. DexToolApp now
      hands getFilesDir()/dextool-logs to the native logger
      right after loadLibrary (nativeSetLogDir ->
      logger_retarget; files reopened append-only, so nothing
      already written is lost).
   c) the uncaught-exception handler appends JAVA_CRASH to the
      SOURCE log now (an entry written straight to the
      Download copy was clobbered by the next copier pass) and
      pushes an immediate copy pass.
   d) the native crash fd opens with O_APPEND - concurrent
      writes (native signal handler, Java handler, copier)
      cannot clobber each other's offsets. Fatal-signal capture
      (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE: signal, si_code,
      fault addr, PC, module+offset) and the streamed logcat
      tombstone backtrace were already in place from r15.

## Fixes in this version (r15)

1. The keyboard STILL failed to open on some hosts (code-audit
   follow-up on r14). Three causes, all fixed:
   a) the Java.choose heap-scan fallback consumed its result
      synchronously - Java.choose is ASYNC, onMatch ran after
      `return found`, so the fallback always returned null.
      Activity resolution is now callback-based
      (find_current_activity_sync + resolve_current_activity(cb)).
   b) ime_show() hit `if (!IME.act) return;` BEFORE the retry
      ladder, so the hidden EditText + TextWatcher were never
      created; the showSoftInput retries are armed
      unconditionally now and each retry re-resolves the
      activity and retries the keyboard.
   c) the instance-field editor and the method-call argument
      fields used raw ImGui::InputText and never asked for the
      keyboard; every editable field goes through the
      ime_textfield wrapper now.

2. Touch mapping rebuilt on ONE coordinate space (the -80 px Y
   bias is gone): the hook forwards getRawX()/getRawY() (screen
   coords), the agent measures the GL view (largest
   SurfaceView/GLSurfaceView/TextureView, content-view fallback)
   with getLocationOnScreen(), and map_touch() scales BOTH axes
   proportionally: imgui = (raw - viewOrigin) * (DisplaySize /
   viewSize). The old scheme mixed view-local getX()/getY() with
   a window-relative content-view rect, hardcoded the Y scale to
   1.0 and patched the residue with a device-specific -80 px bias
   (the logged surface delta was 128 px, so the bias was wrong
   even on its own terms). Bias default is 0; the Settings slider
   stays for exotic hosts; the saved-config key was renamed
   (yoff -> ybias) so a stale -80 cannot fight the new mapping.

3. Backspace race (overlay.cpp): dextool_backspace() set a single
   bool, so N backspaces arriving within one frame collapsed into
   ONE deleted character (characters were queued, backspaces were
   not). It is a counter now.

4. smali_runtime.cpp extract_jar(): stored the java.io.File from
   getFilesDir() in a jstring and fed it to GetStringUTFChars - a
   JNI type mismatch that would crash the process the moment the
   native Smali path ran. Calls getAbsolutePath() properly now
   (with exception checks).

5. Log paths now MATCH: the native side logs to
   getFilesDir()/dextool-logs/ - exactly what the DexToolApp
   copier reads (before: native wrote /sdcard/Download while the
   copier read the private dir, so neither saw the other; under
   scoped storage the /sdcard writes failed silently). The JNI
   markers live in the same folder; /sdcard/Download stays as a
   fallback when the JVM is not reachable.

6. Build/git hygiene: the nolib APK is built from a scratch copy,
   so the committed DexToolApp.java keeps its System.loadLibrary()
   call (the old sed -i stripped it from the working tree, leaving
   a misleading "libtool.so loaded" log); the nolib variant also
   drops that log line. .gitignore covers .gradle/, build/,
   local.properties, .idea/, captures/ and *.iml, and the commit
   step untracks them.

## Fixes in r14 (still in)

1. The keyboard never opened. ime_show() dereferenced `act` - a
   variable that does not exist in that scope - so EVERY attempt to
   show the system IME died with a ReferenceError before
   showSoftInput was ever called, and the single 150 ms retry was
   skipped too (it lives after the throw). Fixed: the IME call uses
   IME.act; the activity is resolved from four sources (lifecycle
   callbacks, the onResume hook, the ActivityThread.mActivities
   walk, and a Java.choose heap-scan fallback); showSoftInput is
   retried at 150/450/900 ms with a fresh requestFocus; every IME
   step is written to the logs, so the next field report shows
   exactly which step failed and on which activity.

2. SIGABRT during agent load ("opened and run but no menu", crash
   log: CRASH signal=6 right after "script created, loading agent",
   with zero JS messages). All Java-touching init (lifecycle
   tracker, Activity hooks) is deferred out of gum_script_load_sync
   into the JS job queue, and each step writes a phase marker
   through the new dextool_agent_log() export into
   dextool-crash.log + dextool-debug.log. A host that kills itself
   (anti-tamper) now shows the exact phase that triggered it - and
   the load itself can no longer abort.

3. "Tapping the app's own menu spawns a small clone of our menu" +
   the clone being untouchable. When the owner context died (app
   menu, dialog, keyboard relayout), the parked ImGui world was
   ADOPTED by whatever surface reached 8 swaps next - including
   small popup/dialog surfaces, which rendered the whole menu
   inside their tiny window (and the clone ignored taps because the
   touch map belongs to the main surface). ensure_ownership() now
   refuses ownership for surfaces far smaller than the parked world
   (area < 50% or a dimension < 45%); tiny surfaces (< 220x220)
   never win election at all. Rejections are logged
   ("skip small surface").

4. Diagnostics: logger_init records VmRSS + MemAvailable (the
   "crash when I open any app" class - lmkd kills are invisible to
   signal handlers) and the agent phase markers above.

## Fixes in r12-r13 (still in)

1. "Open a class and the app freezes" - the field log shows the tap on
   a class is the last event the UI ever processed. Two independent
   causes, both fixed:
   a) menu.cpp locked g_mtx a SECOND time in tab_classes() (MLK inside
      MLK), and class_tab_content() nested again - with the plain
      mutex the render thread self-deadlocked, sitting inside the
      eglSwapBuffers hook, which froze the host's GL thread: the
      whole app hung. g_mtx is a RECURSIVE mutex now and the stray
      lock is gone.
   b) openClass() read every STATIC field value (f.get(null)); a
      static read can run the class's <clinit>, which may block
      forever inside app code. openClass() now lists names/types
      only; a "Read static values" button in the Fields sub-tab
      loads the values on demand for the open class.
   Tapping a recorded row / tracker caller froze the app through the
   same path (it opens the class tab too).

2. Y-axis scaling mismatch ("must tap above top items / below bottom
   items, only the center is right"). map_touch() scaled Y by
   ds.y / g_map.h - the view rect is TALLER than the GL surface
   (960 vs 832), so a false 0.867 factor pulled taps toward the
   center. Y is now a pure translation (sy = 1.0); the static
   status-bar offset is handled by the existing "Touch Y offset"
   slider (g_bias_y).

3. The built-in on-screen keyboard is REMOVED. Text fields use the
   PHONE's own keyboard: focusing a field posts showIme to the agent,
   which focuses a hidden 1x1 EditText on the resumed activity and
   opens the system IME. A TextWatcher diffs each edit against the
   previous text (.toString() - the method; the r10 bug was
   String(editable) -> "[object Object]") and forwards the delta to
   libtool.so as dextool_add_char()/dextool_backspace(). Done on the
   IME releases the ImGui field, so the next tap re-opens the
   keyboard; tapping the active field again re-opens it too.

4. The Classes tab lists ALL loaded classes BY DEFAULT - no
   "+ search" gate. The agent streams the full list in pages of 400
   with a query id (r11 mechanism, now triggered automatically when
   the tab opens); the filter row re-queries with a substring.

5. Recorder: Stop now also ABORTS a still-running installation (r11
   kept hooking classes after Stop, so the tab looked stuck on
   "installing ..."). Progress messages are throttled to one per
   250 ms (the r11 log flooded ~200 recorderProgress lines per
   install) and install chunks are 12 classes instead of 25, so the
   JS thread never stalls for seconds.

Also (r11, still in): scoped recorder (package filter + method cap),
eglTerminate slot retirement against the GL_INVALID_VALUE abort
class, park/adopt of the ImGui world across context death, UI state
persistence to <logdir>/dextool-ui.conf.


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
