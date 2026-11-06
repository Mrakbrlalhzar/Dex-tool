#pragma once
#include <jni.h>
#include <string>
#include <vector>
void frida_bridge_start(JavaVM* jvm);
void frida_post(const std::string& json);
// Called from GL thread: swaps the pending-message vector into `out`.
int frida_drain_messages(std::vector<std::string>& out);
