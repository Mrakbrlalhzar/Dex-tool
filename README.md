# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r7)

    1. Duplicate menus: only ONE EGL context now owns the
       overlay (elected by swap activity, migrated if the
       owner goes quiet). Both GL threads called
       eglSwapBuffers in r6, so the menu was drawn twice and
       only one copy received touches.

    2. Touch accuracy: touches are queued as real events
       (down/move/up) and replayed on the owner thread, so
       taps hit the buttons they aim at; menu taps are also
       swallowed when the overlay is hovered (want-mouse
       bridge) so the game does not react to them.

    3. Keyboard: showSoftInput flag fixed (0, not the
       ignorable SHOW_IMPLICIT=1) and every guard runs on
       the main thread. Characters reach ImGui via
       AddInputCharacter; backspace is a real key event.

    4. Close/crash: hiding the menu (X, Visible) no longer
       races the other GL thread; slots are never freed
       (graveyard) so a tick can never dereference a slot
       destroyed mid-frame. Element/array buffer bindings
       are saved/restored on ES2 as well (r6 clobbered them,
       which could abort HWUI).

    5. Classes tab: agent messages are sent as objects (not
       double-encoded strings), the payload is extracted on
       the C++ side, so classInfo arrives and the method and
       field lists populate. New "List loaded classes"
       button with a filter box; double-tap a class to open.

    6. Toggle button: a floating MENU button appears when
       the menu is hidden, so X / Visible-off can always be
       undone (r6 left you stranded with no way back).

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
