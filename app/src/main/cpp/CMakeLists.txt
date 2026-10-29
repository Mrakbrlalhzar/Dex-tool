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
    message(FATAL_ERROR "Unsupported ABI")
endif()

include_directories(
    ${FRIDA_DIR}/include
    ${FRIDA_DIR}/include/glib-2.0
    ${FRIDA_DIR}/include/glib-2.0-include
)

# Import همه کتابخانه‌های استاتیک Frida
file(GLOB FRIDA_LIBS "${FRIDA_DIR}/lib/*.a")
add_library(frida-bundle STATIC IMPORTED)
set_target_properties(frida-bundle PROPERTIES IMPORTED_LOCATION "${FRIDA_DIR}/lib/libfrida-gumjs.a")

include_directories(imgui imgui/backends tool)

add_library(tool SHARED
    tool/native-lib.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_android.cpp
    imgui/backends/imgui_impl_opengl3.cpp)

# لینک کردن تمام کتابخانه‌های استاتیک همراه با group برای حل وابستگی دایره‌ای
target_link_libraries(tool
    -Wl,--start-group
    ${FRIDA_LIBS}
    -Wl,--end-group
    android log EGL GLESv3 dl)

target_link_options(tool PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--strip-all)
