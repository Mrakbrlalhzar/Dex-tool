cmake_minimum_required(VERSION 3.22.1)
project("tool")
set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden")
set(CMAKE_CXX_FLAGS_RELEASE "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden")

if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(FRIDA_DIR /tmp/frida-built/arm64-v8a)
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(FRIDA_DIR /tmp/frida-built/armeabi-v7a)
else()
    message(FATAL_ERROR "Unsupported ABI: ${ANDROID_ABI}")
endif()

if(NOT EXISTS "${FRIDA_DIR}/lib/libfrida-gumjs.a")
    message(FATAL_ERROR "Frida lib not found: ${FRIDA_DIR}/lib/libfrida-gumjs.a")
endif()

# Include the root + gum subdir for good measure
include_directories(${FRIDA_DIR}/include)
include_directories(${FRIDA_DIR}/include/gum)

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

target_link_options(tool PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,--strip-all)

target_link_libraries(tool frida-gumjs android log EGL GLESv3 dl)
