package com.dextool.app;
import android.app.Activity;
import android.opengl.GLSurfaceView;
import static android.opengl.GLES20.glClearColor;
import static android.opengl.GLES20.glViewport;
import static android.opengl.GLES20.glClear;
import static android.opengl.GLES20.GL_COLOR_BUFFER_BIT;
import android.os.Bundle;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
public class MainActivity extends Activity {
    private GLSurfaceView mGLView;
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        mGLView = new GLSurfaceView(this);
        mGLView.setEGLContextClientVersion(2);
        mGLView.setRenderer(new GLSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 g, EGLConfig c) {
              glClearColor(0.08f, 0.10f, 0.14f, 1f);
            }
            public void onSurfaceChanged(GL10 g, int w, int h) {
              glViewport(0, 0, w, h);
            }
            public void onDrawFrame(GL10 g) {
              glClear(GL_COLOR_BUFFER_BIT);
            }
        });
        setContentView(mGLView);
    }
    @Override protected void onResume() { super.onResume(); mGLView.onResume(); }
    @Override protected void onPause()  { super.onPause();  mGLView.onPause();  }
}
