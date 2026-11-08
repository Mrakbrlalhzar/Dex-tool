# Dex-tool

Standalone runtime instrumentation for Android APKs.
libtool.so for armeabi-v7a + arm64-v8a. No root. API 26+.

## Fixes in this version (r10)

    1. All tabs drew empty - the tab loop created the tab items but
       never called their body functions (Settings/Patches/Hooks/
       Tracker/Recorder/Monitor/Script/Classes are ~500 lines of UI
       that simply never ran). The loop now calls each tab's
       function between BeginTabItem/EndTabItem.

    2. App crash right after boot (JNI 'invalid jobject' abort in
       nativePollOnce). The keyboard bridge built a TextWatcher with
       Java.registerClass and posted it via Java.scheduleOnMainThread
       - both are fatal on 32-bit ART with JNI checks. Rewritten -
       the hidden EditText and the IME show/hide now happen inside
       the main-thread dispatchTouchEvent hook (plain reflection
       only), and the frida thread polls the EditText text and feeds
       the delta to ImGui (chars + backspace).

    3. Touch offset still present when the agent died before
       measuring. The overlay now derives a native fallback map from
       the largest surface ever seen (the HWUI surface spans the full
       window), so input is scaled correctly from the first frame;
       the agent's real measurement replaces it when it arrives.

    4. Park/adopt confirmed in the field - the r9 log shows the menu
       surviving an app-switch context destroy ('ImGui parked',
       'adopted after 8 swaps'). That path is kept as-is.

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
