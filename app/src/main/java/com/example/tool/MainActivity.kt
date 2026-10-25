package com.example.tool

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView

class MainActivity : Activity() {
    companion object {
        init {
            try {
                System.loadLibrary("tool")
            } catch (t: Throwable) {
                try {
                    val f = java.io.File("/sdcard/Download/ImGuiMenu_crash.log")
                    f.parentFile?.mkdirs()
                    f.appendText("\n=== LoadLibrary failed ===\n${t.stackTraceToString()}\n")
                } catch (_: Throwable) {
                    try {
                        val f = java.io.File("/sdcard/Android/data/com.example.tool/crash.log")
                        f.parentFile?.mkdirs()
                        f.appendText("\n=== LoadLibrary failed ===\n${t.stackTraceToString()}\n")
                    } catch (_: Throwable) {}
                }
            }
        }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val tv = TextView(this).apply {
            text = "Test harness\n\nIf you see the ImGui menu, the .so works.\nIf not, check the crash log in Downloads or Android/data/com.example.tool/"
            textSize = 18f
            gravity = Gravity.CENTER
        }
        setContentView(tv)
    }
}
