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
 * Draws the 176x208 game screen and the on-screen controls (d-pad, fire, two soft keys),
 * and turns touches into the Symbian key events the original game expects.
 */
final class GameView extends SurfaceView implements SurfaceHolder.Callback, Engine.Host {
    private static final int K_LEFT = 0, K_RIGHT = 1, K_UP = 2, K_DOWN = 3, K_FIRE = 4, K_LSK = 5, K_RSK = 6;
    private static final int[] SCAN = {Keys.LEFT, Keys.RIGHT, Keys.UP, Keys.DOWN, Keys.FIRE, Keys.LSK, Keys.RSK};

    private final SurfaceHolder holder;
    private final Object drawLock = new Object();
    private final Bitmap frame = Bitmap.createBitmap(176, 208, Bitmap.Config.ARGB_8888);
    private final Rect src = new Rect(0, 0, 176, 208);
    private final RectF gameRect = new RectF();
    private final RectF lskRect = new RectF();
    private final RectF rskRect = new RectF();
    private float dpadX, dpadY, dpadR, fireX, fireY, fireR;

    private final Paint bitmapPaint = new Paint();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();

    private final boolean[] down = new boolean[7];
    private boolean surfaceReady = false;
    private boolean haveFrame = false;
    private String status = "";
    private Engine engine;

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

    // ---------------------------------------------------------------- Engine.Host
    @Override
    public void showStatus(String s) {
        status = s;
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
                text.setTextSize(getWidth() / 18f);
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
                haveFrame = true;
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

    private void layoutControls(int w, int h) {
        float gw = w;
        float gh = gw * 208f / 176f;
        float maxH = h * 0.66f;
        if (gh > maxH) {
            gh = maxH;
            gw = gh * 176f / 208f;
        }
        float gx = (w - gw) / 2f;
        gameRect.set(gx, 0, gx + gw, gh);

        float areaTop = gh;
        float areaH = h - gh;
        float u = Math.min(w, areaH * 1.6f);          // sizing unit that works for tall and short layouts
        float skH = Math.max(areaH * 0.14f, u * 0.09f);
        float skW = w * 0.30f;
        float skY = areaTop + areaH * 0.04f;
        lskRect.set(w * 0.06f, skY, w * 0.06f + skW, skY + skH);
        rskRect.set(w * 0.94f - skW, skY, w * 0.94f, skY + skH);

        float cy = areaTop + areaH * 0.30f + (areaH * 0.70f) / 2f;
        float r = Math.min(w * 0.22f, areaH * 0.33f);
        dpadX = w * 0.26f;
        dpadY = cy;
        dpadR = r;
        fireX = w * 0.76f;
        fireY = cy;
        fireR = r * 0.80f;
    }

    // ---------------------------------------------------------------- controls drawing
    private void drawControls(Canvas c) {
        fill.setStyle(Paint.Style.FILL);
        // soft keys
        drawSoft(c, lskRect, "LEFT", down[K_LSK]);
        drawSoft(c, rskRect, "RIGHT", down[K_RSK]);
        // d-pad ring
        fill.setColor(Color.argb(60, 255, 255, 255));
        c.drawCircle(dpadX, dpadY, dpadR, fill);
        c.drawCircle(dpadX, dpadY, dpadR, stroke);
        float a = dpadR * 0.30f;
        drawArrow(c, dpadX, dpadY - dpadR * 0.62f, a, 0, down[K_UP]);
        drawArrow(c, dpadX, dpadY + dpadR * 0.62f, a, 180, down[K_DOWN]);
        drawArrow(c, dpadX - dpadR * 0.62f, dpadY, a, 270, down[K_LEFT]);
        drawArrow(c, dpadX + dpadR * 0.62f, dpadY, a, 90, down[K_RIGHT]);
        // fire button
        fill.setColor(down[K_FIRE] ? Color.argb(170, 255, 120, 40) : Color.argb(90, 255, 140, 40));
        c.drawCircle(fireX, fireY, fireR, fill);
        c.drawCircle(fireX, fireY, fireR, stroke);
        text.setTextSize(fireR * 0.38f);
        c.drawText("FIRE", fireX, fireY + fireR * 0.13f, text);
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
    private void hit(float x, float y, boolean[] want) {
        float dx = x - dpadX, dy = y - dpadY;
        float d = (float) Math.sqrt(dx * dx + dy * dy);
        if (d <= dpadR * 1.3f && d >= dpadR * 0.18f) {
            if (Math.abs(dx) > Math.abs(dy)) {
                want[dx < 0 ? K_LEFT : K_RIGHT] = true;
            } else {
                want[dy < 0 ? K_UP : K_DOWN] = true;
            }
            return;
        }
        float fx = x - fireX, fy = y - fireY;
        if (fx * fx + fy * fy <= fireR * fireR * 1.3f) {
            want[K_FIRE] = true;
            return;
        }
        float slop = 14f;
        if (x >= lskRect.left - slop && x <= lskRect.right + slop && y >= lskRect.top - slop && y <= lskRect.bottom + slop) {
            want[K_LSK] = true;
        } else if (x >= rskRect.left - slop && x <= rskRect.right + slop && y >= rskRect.top - slop && y <= rskRect.bottom + slop) {
            want[K_RSK] = true;
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        boolean[] want = new boolean[7];
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
        for (int k = 0; k < want.length; k++) {
            if (want[k] != down[k]) {
                down[k] = want[k];
                if (engine != null) {
                    engine.postKey(SCAN[k], want[k]);
                }
            }
        }
    }

    void releaseAll() {
        apply(new boolean[7]);
    }
}
