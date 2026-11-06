#pragma once
void menu_init();            // one-time: applies theme
void menu_poll_messages();   // per-frame GL thread: drains frida msgs
void menu_draw();
