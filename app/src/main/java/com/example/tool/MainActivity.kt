package com.example.tool

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView

class MainActivity : Activity() {
    companion object {
        init { System.loadLibrary("tool") }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val tv = TextView(this).apply {
            text = "Test harness\n\nIf you see the ImGui menu overlay with\nMain and Classes tabs, the .so is working!"
            textSize = 20f
            gravity = Gravity.CENTER
        }
        setContentView(tv)
    }
}
