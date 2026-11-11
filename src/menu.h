#pragma once
#include <string>
void menu_init();
void menu_draw();
/* r25: 0 = no frames wanted, 1 = slow (menu hidden, FAB alive),
   2 = full rate (menu visible). Read by the dextool_want_frames
   export; the agent drives DecorView.postInvalidate() from a timer
   so render-on-demand hosts keep producing frames while the menu
   is up (freeze/lag fix). */
int menu_frame_pump_level();
/* cross-link entry point used by every log list: opens (or focuses) the
   class tab and asks the agent for its methods/fields/instances */
void menu_open_class(const std::string& name);
/* r12: text input comes from the PHONE's own keyboard (system IME).
   A focused field posts showIme to the agent; the agent's hidden
   EditText + TextWatcher turns IME edits into dextool_add_char()/
   dextool_backspace(), which overlay.cpp feeds to ImGui as normal
   character events. No keyboard is drawn natively anymore. */
