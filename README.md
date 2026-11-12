# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r27)

UI spec v2 (the runtime_tool_imgui_mock design) integrated into the
real menu. Same engine, RPCs and agent - menu.cpp carries the
changes; the object-parameter dropdowns reuse listInstances.

- **Family dropdown popup:** the FIRST widget inside the popup is a
  "Search families..." input that filters the PACKAGE list in real
  time as you type (package names only - it never touches the class
  search). "(All families)" sits right under it; the "All" checkbox
  stays next to the dropdown.
- **Class window title:** the full class name is the title line and
  WRAPS to the window width (long package paths stay readable when
  the menu is small).
- **Method long press:** pressing a method row for >= 0.55 s opens
  the same context menu as right-click / "...": Copy Method Name,
  Copy Class Name, Open in new window, Full Method View.
- **Method sub-tabs renamed:** Call / Patch / Trace / Hook (were
  Caller / Patcher / Tracer / Hooker).
- **Object parameters are dropdowns:** first open auto-scans the
  parameter's class (listInstances) and lists found instances in
  the popup; a tap fills the argument with the retained handle.
  "Find / Create Object..." opens the Object Finder (Java.choose,
  $new, manual reference) and the free-form value input stays.
- **Fields tree node:** the Fields list renders under a "Fields"
  tree node.
- **Labels per spec:** "Call Count" input, "Call Results:" log,
  "Manual Input" in the Patch tab; the Patch NOP hint spells out
  that void methods return immediately.
- The floating Toggle button and the whole r26 restructure are
  unchanged.

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
