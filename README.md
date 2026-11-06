# Dex-tool v1.3

Standalone runtime instrumentation framework for Android APKs.
Ships as one native shared library: libtool.so (armeabi-v7a + arm64-v8a).
No root. No frida-server. Android 8.0+ (API 26).

## v1.3 changes

- **Message queue** — the frida thread pushes raw JSON into a
  mutex-protected queue; the GL thread drains it each frame via
  `menu_poll_messages()`. **No more cross-thread access** to UI
  state (`s_classes`, `s_script_out`, …). This was the cause of
  the `addr=0x3` crash (use-after-free inside `std::vector`).
- **No `std::function` race** — `frida_set_message_callback` is
  gone. No torn read between the two threads.
- **EGL context change is cheap now** — only the GL backend is
  rebuilt (`ImGui_ImplOpenGL3_Shutdown/Init`); the ImGui context
  and UI state survive. Context switches no longer cause the
  tab/window state to reset, or the reinit storm we saw in v1.2.
- **Bounded reinit counter** — if the same set of EGL contexts
  oscillates, we stop reinit-ing after 8 tries.
- **`g_script` is now atomic** — `frida_post` sees a fully-set
  pointer or null, never a partially-written one.
- **`menu_draw` and `handle_message` guard `ImGui::GetCurrentContext()`**.
