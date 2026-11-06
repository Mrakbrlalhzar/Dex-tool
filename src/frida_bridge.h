#pragma once
#include <jni.h>
#include <string>
#include <functional>
void frida_bridge_start(JavaVM* jvm);
void frida_post(const std::string& json);
using MsgCallback = std::function<void(const std::string&)>;
void frida_set_message_callback(MsgCallback cb);
