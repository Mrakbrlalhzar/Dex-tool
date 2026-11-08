#pragma once
#include <string>
void menu_init();
void menu_draw();
/* cross-link entry point used by every log list: opens (or focuses) the
   class tab and asks the agent for its methods/fields/instances */
void menu_open_class(const std::string& name);
/* r12: text input comes from the PHONE's own keyboard (system IME).
   A focused field posts showIme to the agent; the agent's hidden
   EditText + TextWatcher turns IME edits into dextool_add_char()/
   dextool_backspace(), which overlay.cpp feeds to ImGui as normal
   character events. No keyboard is drawn natively anymore. */
