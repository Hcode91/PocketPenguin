package com.hcode91.gp2xdroid;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Draws the game screen plus GP2X-style on-screen controls, and turns touches into button bits. */
public class EmuView extends View {
    private final EmuActivity act;
    private final int model;
    private final Bitmap bmp = Bitmap.createBitmap(Native.W, Native.H, Bitmap.Config.ARGB_8888);
    private final int[] frame = new int[Native.W * Native.H];
    private volatile boolean frameDirty;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bmpPaint = new Paint();
    private String message;
    private String console;
    private float fps;
    private volatile int buttons;
    private volatile boolean touchDown;
    private volatile int touchX, touchY;

    private final Rect screen = new Rect();
    private float dpadX, dpadY, dpadR;
    private final Btn[] btns;
    private final RectF menuRect = new RectF();

    private static class Btn {
        final String label; final int bit; final RectF r = new RectF(); final boolean round;
        Btn(String l, int b, boolean round) { label = l; bit = b; this.round = round; }
    }

    public EmuView(EmuActivity a, int model) {
        super(a);
        act = a;
        this.model = model;
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        btns = new Btn[] {
                new Btn("A", Native.A, true), new Btn("B", Native.B, true),
                new Btn("X", Native.X, true), new Btn("Y", Native.Y, true),
                new Btn("L", Native.L, false), new Btn("R", Native.R, false),
                new Btn(model == Native.WIZ ? "MENU" : model == Native.CAANOO ? "HELP1" : "START", Native.START, false),
                new Btn(model == Native.CAANOO ? "HELP2" : "SELECT", Native.SELECT, false),
                new Btn("VOL-", Native.VOLDOWN, false), new Btn("VOL+", Native.VOLUP, false),
                new Btn(model == Native.CAANOO ? "HOME" : "PUSH", model == Native.CAANOO ? Native.HOME : Native.PUSH, false),
        };
    }

    void setMessage(String m) { message = m; postInvalidate(); }
    void setFps(float f) { fps = f; }
    int getButtons() { return buttons; }
    boolean isTouchDown() { return touchDown; }
    int getTouchX() { return touchX; }
    int getTouchY() { return touchY; }

    void pushFrame(int[] px) {
        synchronized (frame) { System.arraycopy(px, 0, frame, 0, frame.length); frameDirty = true; }
        postInvalidate();
    }

    void showConsole(String t) { console = t; postInvalidate(); }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        int sh = (int) (h * 0.94f);
        int sw = sh * 4 / 3;
        if (sw > w * 0.60f) { sw = (int) (w * 0.60f); sh = sw * 3 / 4; }
        int sx = (w - sw) / 2, sy = (h - sh) / 2;
        screen.set(sx, sy, sx + sw, sy + sh);
        float side = sx;
        dpadR = Math.min(side * 0.42f, h * 0.24f);
        dpadX = side / 2;
        dpadY = h * 0.52f;
        float cx = w - side / 2, cy = h * 0.52f, r = Math.min(side * 0.15f, h * 0.085f), d = r * 1.8f;
        btns[0].r.set(cx - d - r, cy - r, cx - d + r, cy + r);   // A left
        btns[1].r.set(cx + d - r, cy - r, cx + d + r, cy + r);   // B right
        btns[2].r.set(cx - r, cy + d - r, cx + r, cy + d + r);   // X bottom
        btns[3].r.set(cx - r, cy - d - r, cx + r, cy - d + r);   // Y top
        float lw = side * 0.7f, lh = h * 0.09f;
        btns[4].r.set(side / 2 - lw / 2, h * 0.04f, side / 2 + lw / 2, h * 0.04f + lh);           // L
        btns[5].r.set(w - side / 2 - lw / 2, h * 0.04f, w - side / 2 + lw / 2, h * 0.04f + lh);   // R
        float bw = side * 0.42f, bh = h * 0.08f, y0 = h - bh - h * 0.03f;
        btns[6].r.set(w - side / 2 + 4, y0, w - side / 2 + bw + 4, y0 + bh);             // START
        btns[7].r.set(w - side / 2 - bw - 4, y0, w - side / 2 - 4, y0 + bh);             // SELECT
        btns[8].r.set(side / 2 - bw - 4, y0, side / 2 - 4, y0 + bh);                     // VOL-
        btns[9].r.set(side / 2 + 4, y0, side / 2 + bw + 4, y0 + bh);                     // VOL+
        float y1 = y0 - bh - h * 0.02f;
        btns[10].r.set(side / 2 - bw / 2, y1, side / 2 + bw / 2, y1 + bh);               // PUSH / HOME
        menuRect.set(w - side / 2 - bw / 2, h * 0.16f, w - side / 2 + bw / 2, h * 0.16f + bh);
        text.setTextSize(h * 0.032f);
    }

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(Color.rgb(18, 20, 28));
        if (frameDirty) {
            synchronized (frame) { bmp.setPixels(frame, 0, Native.W, 0, 0, Native.W, Native.H); frameDirty = false; }
        }
        c.drawBitmap(bmp, null, screen, bmpPaint);

        paint.setColor(Color.argb(80, 255, 255, 255));
        c.drawCircle(dpadX, dpadY, dpadR, paint);
        float a = dpadR * 0.58f;
        int b = buttons;
        drawArrow(c, dpadX, dpadY - a, (b & Native.UP) != 0);
        drawArrow(c, dpadX, dpadY + a, (b & Native.DOWN) != 0);
        drawArrow(c, dpadX - a, dpadY, (b & Native.LEFT) != 0);
        drawArrow(c, dpadX + a, dpadY, (b & Native.RIGHT) != 0);

        for (Btn bt : btns) {
            boolean down = (b & bt.bit) != 0;
            paint.setColor(down ? Color.argb(220, 120, 200, 255) : Color.argb(100, 255, 255, 255));
            if (bt.round) c.drawOval(bt.r, paint);
            else c.drawRoundRect(bt.r, 14, 14, paint);
            c.drawText(bt.label, bt.r.centerX(), bt.r.centerY() + text.getTextSize() / 3, text);
        }
        paint.setColor(Color.argb(100, 255, 255, 255));
        c.drawRoundRect(menuRect, 14, 14, paint);
        c.drawText("EMU", menuRect.centerX(), menuRect.centerY() + text.getTextSize() / 3, text);

        if (fps > 0) {
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(String.format("%.0f fps", fps), 12, getHeight() * 0.2f, text);
            text.setTextAlign(Paint.Align.CENTER);
        }
        if (message != null) {
            paint.setColor(Color.argb(200, 0, 0, 0));
            c.drawRect(screen, paint);
            float y = screen.centerY() - text.getTextSize() * 2;
            for (String line : message.split("\n")) { c.drawText(line, screen.centerX(), y, text); y += text.getTextSize() * 1.4f; }
        }
        if (console != null) {
            paint.setColor(Color.argb(235, 0, 0, 0));
            c.drawRect(0, 0, getWidth(), getHeight(), paint);
            Paint t = new Paint(text);
            t.setTextAlign(Paint.Align.LEFT);
            t.setTextSize(getHeight() * 0.024f);
            String[] lines = console.split("\n");
            int max = (int) (getHeight() / (t.getTextSize() * 1.2f)) - 1;
            float y = t.getTextSize();
            for (int i = Math.max(0, lines.length - max); i < lines.length; i++) { c.drawText(lines[i], 8, y, t); y += t.getTextSize() * 1.2f; }
            t.setTextAlign(Paint.Align.RIGHT);
            c.drawText("tap to close", getWidth() - 8, getHeight() - 8, t);
        }
    }

    private void drawArrow(Canvas c, float x, float y, boolean down) {
        paint.setColor(down ? Color.argb(230, 120, 200, 255) : Color.argb(140, 255, 255, 255));
        c.drawCircle(x, y, dpadR * 0.22f, paint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (console != null) {
            if (action == MotionEvent.ACTION_DOWN) { console = null; invalidate(); }
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            float x = e.getX(), y = e.getY();
            if (message != null && screen.contains((int) x, (int) y)) { act.onMessageTapped(); return true; }
            if (menuRect.contains(x, y)) { act.showMenu(); return true; }
        }
        int mask = 0;
        boolean tdown = false;
        int tx = touchX, ty = touchY;
        for (int i = 0; i < e.getPointerCount(); i++) {
            if ((action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP) && i == e.getActionIndex()) continue;
            if (action == MotionEvent.ACTION_CANCEL) continue;
            float x = e.getX(i), y = e.getY(i);
            if (model == Native.F200 && screen.contains((int) x, (int) y)) {
                tdown = true;
                tx = (int) ((x - screen.left) * Native.W / screen.width());
                ty = (int) ((y - screen.top) * Native.H / screen.height());
                continue;
            }
            float dx = x - dpadX, dy = y - dpadY;
            if (dx * dx + dy * dy <= dpadR * dpadR * 1.4f && Math.hypot(dx, dy) > dpadR * 0.15f) {
                double ang = Math.toDegrees(Math.atan2(dy, dx));
                if (ang > -67.5 && ang < 67.5) mask |= Native.RIGHT;
                if (ang > 112.5 || ang < -112.5) mask |= Native.LEFT;
                if (ang > 22.5 && ang < 157.5) mask |= Native.DOWN;
                if (ang < -22.5 && ang > -157.5) mask |= Native.UP;
            }
            for (Btn bt : btns) {
                RectF r = new RectF(bt.r);
                r.inset(-10, -10);
                if (r.contains(x, y)) mask |= bt.bit;
            }
        }
        touchDown = tdown;
        touchX = Math.max(0, Math.min(Native.W - 1, tx));
        touchY = Math.max(0, Math.min(Native.H - 1, ty));
        if (mask != buttons) { buttons = mask; invalidate(); }
        return true;
    }
}
