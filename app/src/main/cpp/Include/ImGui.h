#pragma once
#include <EGL/egl.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"
#include "Dobby/include/dobby.h"
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "DexTool", __VA_ARGS__)
#endif
using swapbuffers_orig = EGLBoolean (*)(EGLDisplay, EGLSurface);
static swapbuffers_orig o_swapbuffers = nullptr;
static bool imgui_sw_initialized = false;
static void (*menuAddress)() = nullptr;
static bool isLibraryLoaded(const char *name) {
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (!fp) return false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) { if (strstr(line, name)) { fclose(fp); return true; } }
    fclose(fp); return false;
}
static EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (!imgui_sw_initialized) {
        ImGui::CreateContext();
        ImGuiIO& io0 = ImGui::GetIO();
        io0.DisplaySize = ImVec2((float)w, (float)h);
        io0.IniFilename = nullptr;
        ImGui_ImplAndroid_Init(nullptr);
        ImGui_ImplOpenGL3_Init("#version 300 es");
        imgui_sw_initialized = true;
        LOGI("ImGui initialized %dx%d", w, h);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (menuAddress) menuAddress();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return o_swapbuffers(dpy, surf);
}
void *initModMenu(void *menu_addr) {
    menuAddress = (void (*)())menu_addr;
    while (!isLibraryLoaded("libEGL.so")) sleep(1);
    auto swapBuffers = (uintptr_t)DobbySymbolResolver("libEGL.so", "eglSwapBuffers");
    DobbyHook((void*)swapBuffers, (void*)swapbuffers_hook, (void**)&o_swapbuffers);
    LOGI("eglSwapBuffers hooked");
    return nullptr;
}
