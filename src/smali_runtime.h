#pragma once
#include <cstdint>
#include <string>
#include <vector>
std::string smali_compile(const std::string& src,
                          const std::string& class_name,
                          std::vector<uint8_t>& dex_out);
