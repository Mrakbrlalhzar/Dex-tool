cmake_minimum_required(VERSION 3.22.1)
project("tool")

set(CMAKE_CXX_STANDARD 17)

# Use Frida from /tmp
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(FRIDA_DIR /tmp/frida-gumjs/arm64-v8a)
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(FRIDA_DIR /tmp/frida-gumjs/armeabi-v7a)
else()
    message(FATAL_ERROR "Unsupported ABI: ${ANDROID_ABI}")
endif()

include_directories(${FRIDA_DIR}/include/frida-gum)
add_library(frida-gumjs STATIC IMPORTED)
set_target_properties(frida-gumjs PROPERTIES IMPORTED_LOCATION
    ${FRIDA_DIR}/lib/libfrida-gumjs-1.0.a)

include_directories(
    imgui
    imgui/backends
    tool
)

add_library(tool SHARED
    tool/native-lib.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_android.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

target_link_libraries(tool
    frida-gumjs
    android
    log
    EGL
    GLESv3
    dl
)
