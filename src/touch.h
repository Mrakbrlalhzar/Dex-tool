#pragma once
// NativeActivity fallback touch hook.
// Java-based apps get touch via GumJS Activity.dispatchTouchEvent hook.
void touch_install_hooks();
