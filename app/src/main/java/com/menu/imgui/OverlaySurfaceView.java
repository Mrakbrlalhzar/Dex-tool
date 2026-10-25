package com.menu.imgui;

import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class OverlaySurfaceView extends GLSurfaceView {
    private volatile boolean ready = false;
    private volatile boolean keyboardShown = false;
    private volatile long lastActionMs = 0;
    private final InputMethodManager imm;

    public OverlaySurfaceView(Context ctx) {
        super(ctx);
        imm = (InputMethodManager) ctx.getSystemService(Context.INPUT_METHOD_SERVICE);
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        setZOrderOnTop(true);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
        setRenderer(new MyRenderer());
        setRenderMode(RENDERMODE_CONTINUOUSLY);
    }
    public boolean isReady() { return ready; }
    private void showKeyboard() {
        try { requestFocus(); imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT); } catch (Throwable ignored) { }
    }
    private void hideKeyboard() {
        try { imm.hideSoftInputFromWindow(getWindowToken(), 0); } catch (Throwable ignored) { }
    }
    private class MyRenderer implements GLSurfaceView.Renderer {
        @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            Surface s = getHolder().getSurface();
            if (s != null) { Helper.nativeInit(s); ready = true; }
        }
        @Override public void onSurfaceChanged(GL10 gl, int w, int h) { Helper.nativeResize(w, h); }
        @Override public void onDrawFrame(GL10 gl) {
            if (!ready) return;
            Helper.nativeRender();
            final boolean want = Helper.nativeWantKeyboard();
            long now = System.currentTimeMillis();
            if (want && !keyboardShown) {
                keyboardShown = true; lastActionMs = now;
                post(() -> showKeyboard());
            } else if (!want && keyboardShown) {
                keyboardShown = false; lastActionMs = now;
                post(() -> hideKeyboard());
            } else if (want && keyboardShown && !imm.isAcceptingText() && now - lastActionMs > 400) {
                lastActionMs = now; post(() -> showKeyboard());
            }
        }
    }
    @Override public boolean onCheckIsTextEditor() { return true; }
    @Override public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        return new BaseInputConnection(this, false) {
            @Override public boolean commitText(CharSequence text, int pos) {
                if (text != null) for (int i = 0; i < text.length(); i++) Helper.nativeAddInputChar(text.charAt(i));
                return true;
            }
            @Override public boolean setComposingText(CharSequence text, int pos) { return commitText(text, pos); }
            @Override public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < before; i++) { Helper.nativeAddKeyEvent(67, true); Helper.nativeAddKeyEvent(67, false); }
                return true;
            }
            @Override public boolean sendKeyEvent(KeyEvent ev) {
                if (ev.getKeyCode() == KeyEvent.KEYCODE_DEL) {
                    if (ev.getAction() == KeyEvent.ACTION_DOWN) { Helper.nativeAddKeyEvent(67, true); Helper.nativeAddKeyEvent(67, false); }
                    return true;
                }
                if (ev.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
                    if (ev.getAction() == KeyEvent.ACTION_DOWN) Helper.nativeAddInputChar('\n');
                    return true;
                }
                return super.sendKeyEvent(ev);
            }
        };
    }
}
