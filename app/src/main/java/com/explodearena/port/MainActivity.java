package com.explodearena.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;

/** Explode Arena - native Android port. Hosts the original game engine and shows its screen. */
public class MainActivity extends Activity implements GameView.Listener {
    private static final String PREFS = "explode_arena";
    private static final String PREF_ORIENTATION = "orientation";
    private static final int ORIENT_PORTRAIT = 0, ORIENT_LANDSCAPE = 1, ORIENT_AUTO = 2;

    private Engine engine;
    private AudioOut audio;
    private GameView view;
    private SharedPreferences prefs;
    private int orientationMode = ORIENT_PORTRAIT;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        orientationMode = prefs.getInt(PREF_ORIENTATION, ORIENT_PORTRAIT);
        applyOrientation();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new GameView(this);
        view.setListener(this);
        engine = new Engine(this, view);
        view.setEngine(engine);
        audio = new AudioOut(engine);
        setContentView(view);
        engine.start();
        audio.start();
    }

    private void applyOrientation() {
        switch (orientationMode) {
            case ORIENT_LANDSCAPE:
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
                break;
            case ORIENT_AUTO:
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
                break;
            default:
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
                break;
        }
    }

    /** Called by the gear button on the touch overlay. */
    @Override
    public void onSettingsRequested() {
        final String[] items = {"Portrait", "Landscape", "Auto-rotate"};
        engine.setPaused(true);
        new AlertDialog.Builder(this)
                .setTitle("Screen orientation")
                .setSingleChoiceItems(items, orientationMode, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        orientationMode = which;
                        prefs.edit().putInt(PREF_ORIENTATION, which).apply();
                        applyOrientation();
                        dialog.dismiss();
                    }
                })
                .setNegativeButton("Cancel", null)
                .setOnDismissListener(new DialogInterface.OnDismissListener() {
                    @Override
                    public void onDismiss(DialogInterface dialog) {
                        engine.setPaused(false);
                        hideSystemUi();
                    }
                })
                .show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (engine != null) {
            engine.setPaused(false);
        }
    }

    @Override
    protected void onPause() {
        view.releaseAll();
        engine.setPaused(true);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        audio.stop();
        engine.stop();
        super.onDestroy();
        if (isFinishing()) {
            // the engine keeps process-wide native state: start from a clean process next time
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        // the phone's right soft key is "Back" in the game's menus
        if (engine != null) {
            engine.postKey(Keys.RSK, true);
            engine.postKey(Keys.RSK, false);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private static int mapKey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT: return Keys.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Keys.RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: return Keys.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Keys.DOWN;
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_SPACE:
            case KeyEvent.KEYCODE_BUTTON_A: return Keys.FIRE;
            case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_BUTTON_R1: return Keys.DIGIT_8;      // detonator
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_BUTTON_X: return Keys.LSK;
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BUTTON_B: return Keys.RSK;
            case KeyEvent.KEYCODE_DEL: return Keys.CLEAR;
            default:
                if (code >= KeyEvent.KEYCODE_0 && code <= KeyEvent.KEYCODE_9) {
                    return Keys.DIGIT_0 + (code - KeyEvent.KEYCODE_0);
                }
                return -1;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (engine == null) {
            return super.dispatchKeyEvent(e);
        }
        int scan = mapKey(e.getKeyCode());
        if (scan < 0) {
            return super.dispatchKeyEvent(e);
        }
        if (e.getRepeatCount() == 0) {
            boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
            if (down || e.getAction() == KeyEvent.ACTION_UP) {
                engine.postKey(scan, down);
                if (scan == Keys.FIRE) {
                    engine.postKey(Keys.DIGIT_5, down);   // key 5 = bomb in the default keyboard layout
                }
            }
        }
        return true;
    }
}
