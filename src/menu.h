#pragma once
#include <string>
void menu_init();
void menu_draw();
/* cross-link entry point used by every log list: opens (or focuses) the
   class tab and asks the agent for its methods/fields/instances */
void menu_open_class(const std::string& name);
