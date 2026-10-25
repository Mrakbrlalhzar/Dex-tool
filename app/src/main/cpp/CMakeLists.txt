cmake_minimum_required(VERSION 3.22.1)
project("tool")

set(CMAKE_CXX_STANDARD 17)

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
    android
    log
    EGL
    GLESv3
)
