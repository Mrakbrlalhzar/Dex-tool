package com.dextool.app;

import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class MainActivity extends Activity {
    private GLSurfaceView mGLView;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        mGLView = new GLSurfaceView(this);
        mGLView.setEGLContextClientVersion(3);
        mGLView.setRenderer(new GLSurfaceView.Renderer() {
  public void onSurfaceCreated(GL10 g, EGLConfig c) {}
  public void onSurfaceChanged(GL10 g, int w, int h) {}
  public void onDrawFrame(GL10 g) {}
        });
        setContentView(mGLView);
    }

    @Override protected void onResume() { super.onResume(); mGLView.onResume(); }
    @Override protected void onPause()  { super.onPause();  mGLView.onPause();  }
}
