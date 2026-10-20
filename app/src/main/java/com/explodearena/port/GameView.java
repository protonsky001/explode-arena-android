package com.explodearena.port;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * Draws the 176x208 game screen and the on-screen controls, in portrait or landscape, and turns
 * touches into the Symbian key events the original game expects.
 *
 * Controls: d-pad, BOMB (joystick press + key 5), DET (detonator, key 8), the two soft keys,
 * and a small gear button that opens the settings dialog.
 */
final class GameView extends SurfaceView implements SurfaceHolder.Callback, Engine.Host {
    interface Listener {
        void onSettingsRequested();
    }

    private static final int C_LEFT = 0, C_RIGHT = 1, C_UP = 2, C_DOWN = 3, C_BOMB = 4, C_DET = 5, C_LSK = 6, C_RSK = 7;
    private static final int C_COUNT = 8;
    private static final float DPAD_HIT = 1.2f;
    private static final float ACTION_HIT = 1.08f;   // touch area of BOMB / DET relative to the drawn circle

    /** Scan codes sent for each on-screen control. */
    private static final int[][] SCAN = {
            {Keys.LEFT}, {Keys.RIGHT}, {Keys.UP}, {Keys.DOWN},
            {Keys.FIRE, Keys.DIGIT_5},   // joystick press (menus) + key 5 (bomb in the default keyboard layout)
            {Keys.DIGIT_8},              // detonator
            {Keys.LSK}, {Keys.RSK}};

    private final SurfaceHolder holder;
    private final Object drawLock = new Object();
    private final Bitmap frame = Bitmap.createBitmap(176, 208, Bitmap.Config.ARGB_8888);
    private final Rect src = new Rect(0, 0, 176, 208);
    private final RectF gameRect = new RectF();
    private final RectF lskRect = new RectF();
    private final RectF rskRect = new RectF();
    private float dpadX, dpadY, dpadR;
    private float bombX, bombY, bombR;
    private float detX, detY, detR;
    private float gearX, gearY, gearR;

    private final Paint bitmapPaint = new Paint();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();

    private final boolean[] down = new boolean[C_COUNT];
    private boolean surfaceReady = false;
    private Engine engine;
    private Listener listener;

    GameView(Context context) {
        super(context);
        holder = getHolder();
        holder.addCallback(this);
        bitmapPaint.setFilterBitmap(true);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3f);
        stroke.setColor(Color.argb(150, 255, 255, 255));
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        setFocusable(true);
    }

    void setEngine(Engine e) {
        engine = e;
    }

    void setListener(Listener l) {
        listener = l;
    }

    // ---------------------------------------------------------------- Engine.Host
    @Override
    public void showStatus(String s) {
        synchronized (drawLock) {
            if (!surfaceReady) {
                return;
            }
            Canvas c = holder.lockCanvas();
            if (c == null) {
                return;
            }
            try {
                c.drawColor(Color.BLACK);
                text.setTextSize(Math.min(getWidth(), getHeight()) / 16f);
                c.drawText(s, getWidth() / 2f, getHeight() / 2f, text);
            } finally {
                holder.unlockCanvasAndPost(c);
            }
        }
    }

    @Override
    public void showFrame(int[] argb) {
        synchronized (drawLock) {
            if (!surfaceReady) {
                return;
            }
            Canvas c = holder.lockCanvas();
            if (c == null) {
                return;
            }
            try {
                frame.setPixels(argb, 0, 176, 0, 0, 176, 208);
                c.drawColor(Color.BLACK);
                c.drawBitmap(frame, src, gameRect, bitmapPaint);
                drawControls(c);
            } finally {
                holder.unlockCanvasAndPost(c);
            }
        }
    }

    @Override
    public void onQuit() {
        final android.app.Activity a = (android.app.Activity) getContext();
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                a.finish();
            }
        });
    }

    // ---------------------------------------------------------------- surface
    @Override
    public void surfaceCreated(SurfaceHolder h) {
        synchronized (drawLock) {
            surfaceReady = true;
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
        synchronized (drawLock) {
            layoutControls(w, hgt);
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder h) {
        synchronized (drawLock) {
            surfaceReady = false;
        }
        releaseAll();
    }

    // ---------------------------------------------------------------- layout
    private void layoutControls(int w, int h) {
        if (w > h) {
            layoutLandscape(w, h);
        } else {
            layoutPortrait(w, h);
        }
    }

    /** Game on top, controls underneath: soft keys + settings row, then d-pad (left) and DET/BOMB (right). */
    private void layoutPortrait(int w, int h) {
        float gw = w;
        float gh = gw * 208f / 176f;
        float maxH = h * 0.66f;
        if (gh > maxH) {
            gh = maxH;
            gw = gh * 176f / 208f;
        }
        float gx = (w - gw) / 2f;
        gameRect.set(gx, 0, gx + gw, gh);

        float top = gh;
        float ah = h - gh;
        float margin = Math.max(14f, ah * 0.025f);

        float skH = Math.min(Math.max(ah * 0.13f, 48f), 140f);
        float skW = w * 0.28f;
        float skY = top + margin;
        lskRect.set(w * 0.05f, skY, w * 0.05f + skW, skY + skH);
        rskRect.set(w * 0.95f - skW, skY, w * 0.95f, skY + skH);

        gearR = Math.min(skH * 0.5f, w * 0.05f);
        gearX = w / 2f;
        gearY = skY + skH / 2f;

        float mainTop = skY + skH + margin;
        float mainBottom = h - margin;
        float mainH = mainBottom - mainTop;
        float cy = mainTop + mainH / 2f;

        dpadR = Math.min(w * 0.205f, mainH * 0.41f);
        dpadX = w * 0.25f;
        dpadY = cy;

        layoutActionStack(w * 0.74f, cy, Math.min(w * 0.17f, mainH * 0.34f), mainH);
    }

    /** Game centred and as tall as possible; d-pad in the left zone, DET/BOMB in the right zone. */
    private void layoutLandscape(int w, int h) {
        float minZone = w * 0.20f;
        float gw = Math.min(h * 176f / 208f, w - 2f * minZone);
        float gh = gw * 208f / 176f;
        float gx = (w - gw) / 2f;
        float gy = (h - gh) / 2f;
        gameRect.set(gx, gy, gx + gw, gy + gh);
        float zone = gx;
        float margin = Math.max(14f, h * 0.03f);

        float skH = Math.min(h * 0.11f, 120f);
        float skW = zone * 0.78f;
        float skY = margin;
        lskRect.set(zone * 0.11f, skY, zone * 0.11f + skW, skY + skH);
        rskRect.set(w - zone * 0.11f - skW, skY, w - zone * 0.11f, skY + skH);

        gearR = Math.min(zone * 0.10f, h * 0.045f);
        gearX = w - margin - gearR - zone * 0.04f;
        gearY = h - margin - gearR;

        float mainTop = skY + skH + margin;
        float leftH = (h - margin) - mainTop;
        dpadR = Math.min(Math.min(zone * 0.40f, leftH * 0.41f), h * 0.25f);
        dpadX = zone / 2f;
        dpadY = mainTop + leftH / 2f;

        float rightBottom = gearY - gearR - margin;
        float rightH = rightBottom - mainTop;
        layoutActionStack(w - zone / 2f, mainTop + rightH / 2f, Math.min(Math.min(zone * 0.30f, rightH * 0.30f), h * 0.19f), rightH);
    }

    /** DET above BOMB, centred on (cx, cy); shrunk if needed so that their touch areas fit in availH. */
    private void layoutActionStack(float cx, float cy, float rb0, float availH) {
        float rd0 = rb0 * 0.62f;
        float gap = Math.max(8f, rb0 * 0.2f);
        float need = (1f + ACTION_HIT) * (rb0 + rd0) + gap;   // vertical extent of both touch areas
        float s = Math.min(1f, availH / need);
        bombR = rb0 * s;
        detR = rd0 * s;
        float g = gap * s;
        float stackTop = cy - (2f * (bombR + detR) + g) / 2f;
        detX = cx;
        detY = stackTop + detR;
        bombX = cx;
        bombY = stackTop + 2f * detR + g + bombR;
    }

    // ---------------------------------------------------------------- drawing
    private void drawControls(Canvas c) {
        fill.setStyle(Paint.Style.FILL);
        drawSoft(c, lskRect, "LEFT", down[C_LSK]);
        drawSoft(c, rskRect, "RIGHT", down[C_RSK]);
        drawGear(c);

        fill.setColor(Color.argb(60, 255, 255, 255));
        c.drawCircle(dpadX, dpadY, dpadR, fill);
        c.drawCircle(dpadX, dpadY, dpadR, stroke);
        float a = dpadR * 0.30f;
        drawArrow(c, dpadX, dpadY - dpadR * 0.62f, a, 0, down[C_UP]);
        drawArrow(c, dpadX, dpadY + dpadR * 0.62f, a, 180, down[C_DOWN]);
        drawArrow(c, dpadX - dpadR * 0.62f, dpadY, a, 270, down[C_LEFT]);
        drawArrow(c, dpadX + dpadR * 0.62f, dpadY, a, 90, down[C_RIGHT]);

        drawRound(c, detX, detY, detR, "DET", down[C_DET], Color.argb(90, 230, 70, 70), Color.argb(190, 240, 90, 90), 0.40f);
        drawRound(c, bombX, bombY, bombR, "BOMB", down[C_BOMB], Color.argb(90, 255, 140, 40), Color.argb(190, 255, 130, 40), 0.34f);
    }

    private void drawRound(Canvas c, float x, float y, float r, String label, boolean pressed, int idle, int active, float textScale) {
        fill.setColor(pressed ? active : idle);
        c.drawCircle(x, y, r, fill);
        c.drawCircle(x, y, r, stroke);
        text.setTextSize(r * textScale);
        c.drawText(label, x, y + r * textScale * 0.35f, text);
    }

    private void drawSoft(Canvas c, RectF r, String label, boolean pressed) {
        fill.setColor(pressed ? Color.argb(170, 255, 255, 255) : Color.argb(70, 255, 255, 255));
        c.drawRoundRect(r, r.height() / 3f, r.height() / 3f, fill);
        c.drawRoundRect(r, r.height() / 3f, r.height() / 3f, stroke);
        text.setTextSize(r.height() * 0.42f);
        text.setColor(pressed ? Color.BLACK : Color.WHITE);
        c.drawText(label, r.centerX(), r.centerY() + r.height() * 0.15f, text);
        text.setColor(Color.WHITE);
    }

    private void drawGear(Canvas c) {
        fill.setColor(Color.argb(70, 255, 255, 255));
        c.drawCircle(gearX, gearY, gearR, fill);
        c.drawCircle(gearX, gearY, gearR, stroke);
        Paint p = stroke;
        float inner = gearR * 0.34f, outer = gearR * 0.62f;
        c.drawCircle(gearX, gearY, inner, p);
        for (int k = 0; k < 8; k++) {
            double ang = k * Math.PI / 4.0;
            float cs = (float) Math.cos(ang), sn = (float) Math.sin(ang);
            c.drawLine(gearX + cs * inner, gearY + sn * inner, gearX + cs * outer, gearY + sn * outer, p);
        }
    }

    private void drawArrow(Canvas c, float cx, float cy, float s, float angle, boolean pressed) {
        arrow.reset();
        arrow.moveTo(0, -s);
        arrow.lineTo(s, s * 0.7f);
        arrow.lineTo(-s, s * 0.7f);
        arrow.close();
        c.save();
        c.translate(cx, cy);
        c.rotate(angle);
        fill.setColor(pressed ? Color.argb(230, 255, 255, 255) : Color.argb(120, 255, 255, 255));
        c.drawPath(arrow, fill);
        c.restore();
    }

    // ---------------------------------------------------------------- touch
    private static boolean inCircle(float x, float y, float cx, float cy, float r) {
        float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }

    private static boolean inRect(float x, float y, RectF r, float slop) {
        return x >= r.left - slop && x <= r.right + slop && y >= r.top - slop && y <= r.bottom + slop;
    }

    private void hit(float x, float y, boolean[] want) {
        float dx = x - dpadX, dy = y - dpadY;
        float d = (float) Math.sqrt(dx * dx + dy * dy);
        if (d <= dpadR * DPAD_HIT && d >= dpadR * 0.18f) {
            if (Math.abs(dx) > Math.abs(dy)) {
                want[dx < 0 ? C_LEFT : C_RIGHT] = true;
            } else {
                want[dy < 0 ? C_UP : C_DOWN] = true;
            }
            return;
        }
        if (inCircle(x, y, bombX, bombY, bombR * ACTION_HIT)) {
            want[C_BOMB] = true;
        } else if (inCircle(x, y, detX, detY, detR * ACTION_HIT)) {
            want[C_DET] = true;
        } else if (inRect(x, y, lskRect, 12f)) {
            want[C_LSK] = true;
        } else if (inRect(x, y, rskRect, 12f)) {
            want[C_RSK] = true;
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int idx = e.getActionIndex();
            if (inCircle(e.getX(idx), e.getY(idx), gearX, gearY, gearR * 1.3f)) {
                if (listener != null) {
                    listener.onSettingsRequested();
                }
                return true;
            }
        }
        boolean[] want = new boolean[C_COUNT];
        if (action != MotionEvent.ACTION_CANCEL) {
            int skip = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) ? e.getActionIndex() : -1;
            for (int i = 0; i < e.getPointerCount(); i++) {
                if (i != skip) {
                    hit(e.getX(i), e.getY(i), want);
                }
            }
        }
        apply(want);
        return true;
    }

    private void apply(boolean[] want) {
        for (int k = 0; k < C_COUNT; k++) {
            if (want[k] != down[k]) {
                down[k] = want[k];
                if (engine != null) {
                    for (int s = 0; s < SCAN[k].length; s++) {
                        engine.postKey(SCAN[k][s], want[k]);
                    }
                }
            }
        }
    }

    void releaseAll() {
        apply(new boolean[C_COUNT]);
    }
}
