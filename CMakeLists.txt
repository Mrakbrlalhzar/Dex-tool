cmake_minimum_required(VERSION 3.22)
project(dextool CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

set(FRIDA_GUM_DIR "${CMAKE_SOURCE_DIR}/deps/frida-gum/${ANDROID_ABI}")
add_library(frida-gumjs STATIC IMPORTED)
set_target_properties(frida-gumjs PROPERTIES
    IMPORTED_LOCATION "${FRIDA_GUM_DIR}/libfrida-gumjs.a")

set(DOBBY_DIR "${CMAKE_SOURCE_DIR}/deps/dobby")
add_library(dobby_static STATIC IMPORTED)
set_target_properties(dobby_static PROPERTIES
    IMPORTED_LOCATION "${DOBBY_DIR}/${ANDROID_ABI}/libdobby.a")

find_library(lib_egl     EGL)
find_library(lib_gles2   GLESv2)
find_library(lib_log     log)
find_library(lib_dl      dl)
find_library(lib_android android)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui")
set(IMGUI_SRC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

add_library(tool SHARED
    src/main.cpp
    src/logger.cpp
    src/overlay.cpp
    src/touch.cpp
    src/frida_bridge.cpp
    src/smali_runtime.cpp
    src/menu.cpp
    ${IMGUI_SRC})

target_include_directories(tool PRIVATE
    src/
    ${FRIDA_GUM_DIR}/
    ${DOBBY_DIR}/include/
    ${IMGUI_DIR}/
    ${IMGUI_DIR}/backends/)

target_compile_definitions(tool PRIVATE IMGUI_IMPL_OPENGL_ES2=1)

target_compile_options(tool PRIVATE
    -fvisibility=hidden -ffunction-sections -fdata-sections -O2
    -Wno-unused-variable -Wno-unused-function)

target_link_libraries(tool
    frida-gumjs dobby_static
    ${lib_egl} ${lib_gles2} ${lib_log} ${lib_dl} ${lib_android}
    -lm -Wl,--gc-sections -Wl,--exclude-libs,ALL)
