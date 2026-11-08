#pragma once
#include <android/log.h>
#define DTAG "DexTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  DTAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, DTAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, DTAG, __VA_ARGS__)
void logger_init(const char* base_dir);
void log_info(const char* msg);
void log_debug(const char* msg);
void log_crash_raw(const char* msg);
/* r11: base dir captured at init, for menu.cpp config IO */
const char* logger_base_dir();
void logger_install_abort_capture();
