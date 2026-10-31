package com.example.tool;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            Intent i = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:" + getPackageName()));
            startActivity(i);
        }
        System.loadLibrary("tool");
        TextView tv = new TextView(this);
        tv.setText("DexTool Test\n\nIf overlay not shown:\n1. Grant 'Display over other apps'\n2. Restart this app\n3. Check /sdcard/Download/DexTool.log");
        tv.setTextSize(18);
        tv.setPadding(40, 200, 40, 40);
        setContentView(tv);
    }
}
