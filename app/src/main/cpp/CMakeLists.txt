cmake_minimum_required(VERSION 3.22.1)
project("tool")
set(CMAKE_CXX_STANDARD 17)

set(FRIDA_DIR /tmp/frida-gumjs/${ANDROID_ABI})
if(NOT EXISTS "${FRIDA_DIR}/lib/libfrida-gumjs.a")
    message(FATAL_ERROR "Frida lib missing for ${ANDROID_ABI}")
endif()

include_directories(${FRIDA_DIR}/include)
add_library(frida-gumjs STATIC IMPORTED)
set_target_properties(frida-gumjs PROPERTIES
    IMPORTED_LOCATION ${FRIDA_DIR}/lib/libfrida-gumjs.a)

include_directories(imgui imgui/backends tool)

add_library(tool SHARED
    tool/native-lib.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_android.cpp
    imgui/backends/imgui_impl_opengl3.cpp)

target_link_libraries(tool frida-gumjs android log EGL GLESv3 dl)
