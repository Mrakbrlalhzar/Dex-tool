package com.example.tool

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.PixelFormat
import android.opengl.GLSurfaceView
import android.view.MotionEvent
import android.view.Surface
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class OverlayView(context: Context) : GLSurfaceView(context) {

    companion object {
        init {
            System.loadLibrary("tool")
        }
    }

    @Volatile private var nativeReady = false

    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 16, 0)
        setZOrderOnTop(true)
        holder.setFormat(PixelFormat.TRANSLUCENT)
        setRenderer(Renderer())
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    private inner class Renderer : GLSurfaceView.Renderer {
        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            val s: Surface = holder.surface ?: return
            nativeInit(s)
            nativeReady = true
        }
        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            nativeResize(width, height)
        }
        override fun onDrawFrame(gl: GL10?) {
            if (nativeReady) nativeRender()
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!nativeReady) return false
        return when (event.actionMasked) {
            MotionEvent.ACTION_DOWN        -> nativeTouch(0, event.x, event.y)
            MotionEvent.ACTION_MOVE        -> nativeTouch(2, event.x, event.y)
            MotionEvent.ACTION_UP          -> nativeTouch(1, event.x, event.y)
            MotionEvent.ACTION_CANCEL      -> nativeTouch(3, event.x, event.y)
            else -> false
        }
    }

    private external fun nativeInit(surface: Surface)
    private external fun nativeResize(width: Int, height: Int)
    private external fun nativeRender()
    private external fun nativeTouch(action: Int, x: Float, y: Float): Boolean
}
