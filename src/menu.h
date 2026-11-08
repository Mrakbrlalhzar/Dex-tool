#pragma once
#include <string>
void menu_init();
void menu_draw();
/* cross-link entry point used by every log list: opens (or focuses) the
   class tab and asks the agent for its methods/fields/instances */
void menu_open_class(const std::string& name);
/* r11 on-screen keyboard: overlay calls this for every touch event
   (display space) BEFORE feeding the event to ImGui; returns true when
   the keyboard took the event (tap inside it, or a drag that started
   on it). */
bool menu_kbd_feed(int phase, float x, float y);
