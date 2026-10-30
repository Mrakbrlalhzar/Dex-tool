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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "DexTool", __VA_ARGS__)
using swapbuffers_orig = EGLBoolean (*)(EGLDisplay, EGLSurface);
static swapbuffers_orig o_swapbuffers = nullptr;
static bool isInitialized = false;
static void (*menuAddress)() = nullptr;
static bool isLibraryLoaded(const char *name) {
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (!fp) return false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) { if (strstr(line, name)) { fclose(fp); return true; } }
    fclose(fp); return false;
}
static EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf) {
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (!isInitialized) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.IniFilename = nullptr;
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        isInitialized = true;
        LOGI("ImGui initialized %dx%d", w, h);
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w, h);
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
