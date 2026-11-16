package com.hcode91.pocketpenguin;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.view.View;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowDialog;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Renders the tutorial screenshots in docs/screenshots off-device. The picture on the emulated
 * screen is an original placeholder drawing, not a real game.
 *
 *   gradle :app:testDebugUnitTest --tests '*Screenshots*'
 */
@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = 34, qualifiers = "w411dp-h891dp-port-xxhdpi")
public class Screenshots {
    private final File out = new File(System.getProperty("screenshots.dir", "../docs/screenshots"));

    private void save(View v, String name, int w, int h) throws Exception {
        out.mkdirs();
        v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
        v.layout(0, 0, w, h);
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        v.draw(new Canvas(bmp));
        Bitmap small = Bitmap.createScaledBitmap(bmp, w / 2, h / 2, true);
        try (FileOutputStream f = new FileOutputStream(new File(out, name + ".png"))) {
            small.compress(Bitmap.CompressFormat.PNG, 100, f);
        }
    }

    private static int[] placeholder() {
        Bitmap b = Bitmap.createBitmap(Native.W, Native.H, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        Paint p = new Paint();
        p.setShader(new LinearGradient(0, 0, 0, 240, Color.rgb(0x14, 0x21, 0x3D), Color.rgb(0x3A, 0x86, 0xFF), Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 320, 240, p);
        p.setShader(null);
        // Chunky 16-bit-era tiles and a little penguin, all drawn here.
        p.setColor(Color.rgb(0xE9, 0xEE, 0xF5));
        for (int x = 0; x < 320; x += 16) c.drawRect(x, 200, x + 15, 215, p);
        p.setColor(Color.rgb(0xA8, 0xC5, 0xE8));
        for (int x = 0; x < 320; x += 16) c.drawRect(x, 216, x + 15, 240, p);
        p.setColor(Color.rgb(0x0B, 0x0F, 0x1A)); c.drawRect(144, 152, 176, 200, p); c.drawRect(148, 144, 172, 152, p);
        p.setColor(Color.WHITE); c.drawRect(150, 160, 170, 198, p); c.drawRect(151, 147, 155, 151, p); c.drawRect(165, 147, 169, 151, p);
        p.setColor(Color.rgb(0xFF, 0xB7, 0x03)); c.drawRect(156, 153, 164, 157, p); c.drawRect(146, 198, 156, 202, p); c.drawRect(164, 198, 174, 202, p);
        p.setColor(Color.WHITE); p.setTextAlign(Paint.Align.CENTER); p.setTextSize(24); p.setFakeBoldText(true);
        c.drawText("YOUR GAME HERE", 160, 60, p);
        int[] px = new int[Native.W * Native.H];
        b.getPixels(px, 0, Native.W, 0, 0, Native.W, Native.H);
        return px;
    }

    @Test
    public void library() throws Exception {
        File sd = new File(RuntimeEnvironment.getApplication().getFilesDir(), "sd");
        for (String g : new String[] {"Example Platformer", "Homebrew Puzzle", "Test Program (Wiz)"}) {
            File d = new File(sd, g);
            d.mkdirs();
            new File(d, "game.gpe").createNewFile();
        }
        MainActivity act = Robolectric.buildActivity(MainActivity.class).setup().get();
        save(act.findViewById(android.R.id.content), "1-library", 1080, 2340);

        // Long-press options for a game.
        java.lang.reflect.Method m = MainActivity.class.getDeclaredMethod("gameOptions", String.class);
        m.setAccessible(true);
        m.invoke(act, "Example Platformer");
        AlertDialog d = (AlertDialog) ShadowDialog.getLatestDialog();
        save(d.getWindow().getDecorView(), "2-game-options", 1000, 880);
    }

    @Test
    public void playing() throws Exception {
        EmuActivity act = Robolectric.buildActivity(EmuActivity.class).get();
        EmuView view = new EmuView(act, Native.WIZ);
        view.pushFrame(placeholder());
        save(view, "3-playing", 2340, 1080);
    }
}
