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
#include "Log.h"

using swapbuffers_orig = EGLBoolean (*)(EGLDisplay, EGLSurface);
static swapbuffers_orig o_swapbuffers = nullptr;
static bool imgui_sw_initialized = false;
static void (*menuAddress)() = nullptr;
static int g_frame_count = 0;

static bool isLibraryLoaded(const char *name) {
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (!fp) return false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name)) { fclose(fp); return true; }
    }
    fclose(fp);
    return false;
}

static EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf) {
    g_frame_count++;
    bool log_frame = (g_frame_count <= 10) || (g_frame_count % 60 == 0);
    if (log_frame) flog("[swap] frame %d enter", g_frame_count);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (!imgui_sw_initialized) {
        flog("[swap] FIRST FRAME - init ImGui (w=%d h=%d)", w, h);
        ImGui::CreateContext();
        flog("[swap] CreateContext OK");
        ImGuiIO& io0 = ImGui::GetIO();
        io0.DisplaySize = ImVec2((float)w, (float)h);
        io0.IniFilename = nullptr;
        ImGui_ImplAndroid_Init(nullptr);
        flog("[swap] ImGui_ImplAndroid_Init OK");
        ImGui_ImplOpenGL3_Init("#version 300 es");
        flog("[swap] ImGui_ImplOpenGL3_Init OK");
        imgui_sw_initialized = true;
        flog("[swap] ImGui init done");
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplOpenGL3_NewFrame();
    if (log_frame) flog("[swap] OpenGL3_NewFrame OK");
    ImGui::NewFrame();
    if (log_frame) flog("[swap] NewFrame OK");
    if (menuAddress) {
        menuAddress();
        if (log_frame) flog("[swap] DrawMenu OK");
    }
    ImGui::Render();
    if (log_frame) flog("[swap] Render OK");
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (log_frame) flog("[swap] RenderDrawData OK");
    if (!o_swapbuffers) {
        flog("[swap] ERROR: o_swapbuffers is NULL!");
        return EGL_FALSE;
    }
    return o_swapbuffers(dpy, surf);
}

void *initModMenu(void *menu_addr) {
    menuAddress = (void (*)())menu_addr;
    flog("[init] waiting for libEGL.so");
    while (!isLibraryLoaded("libEGL.so")) sleep(1);
    flog("[init] libEGL.so loaded, resolving eglSwapBuffers");
    auto swapBuffers = (uintptr_t)DobbySymbolResolver("libEGL.so", "eglSwapBuffers");
    flog("[init] eglSwapBuffers addr=%p", (void*)swapBuffers);
    if (!swapBuffers) {
        flog("[init] ERROR: could not resolve eglSwapBuffers");
        return nullptr;
    }
    int rc = DobbyHook((void*)swapBuffers, (void*)swapbuffers_hook, (void**)&o_swapbuffers);
    flog("[init] DobbyHook rc=%d orig=%p", rc, (void*)o_swapbuffers);
    if (rc != 0 || !o_swapbuffers) {
        flog("[init] ERROR: DobbyHook FAILED rc=%d orig=%p", rc, (void*)o_swapbuffers);
        return nullptr;
    }
    flog("[init] eglSwapBuffers hooked OK");
    return nullptr;
}
