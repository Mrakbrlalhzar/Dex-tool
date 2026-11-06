# Dex-tool v1.2

Standalone runtime instrumentation framework for Android APKs.
Ships as one native shared library: libtool.so (armeabi-v7a + arm64-v8a).
No root. No frida-server. Android 8.0+ (API 26).

## v1.2 changes

- **Crash handler** rewritten with `sigaction` + sigaltstack so it
  still runs on stack overflow, and logs fault addr + PC.
- **GumJS startup**: frida pthread now gets 16 MB stack; ImGui is
  fully initialized *before* the frida thread is spawned, removing
  a GLib-state race.
- **Fine-grained GumJS checkpoints** in `dextool.log`:
  A … B … C … D … E … F … G … H — the last one before the crash
  tells us exactly which gum_* call fails.
- **Agent** top-level code runs inside a try/catch IIFE and the
  Java hooks are deferred with `setImmediate` so they don’t fire
  synchronously inside `gum_script_load_sync`.
