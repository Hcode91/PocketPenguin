package com.hcode91.gp2xdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

/** Runs one game. Extras: model, game (guest path), mhz. */
public class EmuActivity extends Activity {
    private EmuView view;
    private Thread emuThread;
    private volatile boolean running;
    private boolean started, finishedGame;
    private AudioTrack audio;
    private int keyMask, stickMask;
    private int model, mhz;
    private String game;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        model = getIntent().getIntExtra("model", Native.F200);
        mhz = getIntent().getIntExtra("mhz", 200);
        game = getIntent().getStringExtra("game");
        view = new EmuView(this, model);
        setContentView(view);
        hideSystemUi();
        startGame();
    }

    static File firmwareDir(File filesDir, int model) {
        String n = model == Native.WIZ ? "wiz" : model == Native.CAANOO ? "caanoo" : "gp2x";
        File d = new File(filesDir, "fw/" + n);
        d.mkdirs();
        return d;
    }

    private void startGame() {
        File root = firmwareDir(getFilesDir(), model);
        File sd = new File(getFilesDir(), "sd");
        String err = Native.init(model, root.getAbsolutePath(), sd.getAbsolutePath(), game, mhz);
        if (!err.isEmpty()) {
            finishedGame = true;
            view.setMessage("Could not start the game:\n" + wrap(err) + "\n\nTap here to go back");
            return;
        }
        started = true;
        finishedGame = false;
        view.setMessage(null);
        resumeEmu();
    }

    private static String wrap(String s) {
        StringBuilder out = new StringBuilder();
        int col = 0;
        for (String w : s.split(" ")) {
            if (col + w.length() > 48) { out.append('\n'); col = 0; }
            out.append(w).append(' ');
            col += w.length() + 1;
        }
        return out.toString().trim();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void resumeEmu() {
        if (!started || running || finishedGame) return;
        running = true;
        int rate = 44100;
        int minBuf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        audio = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(Math.max(minBuf, 44100))
                .setTransferMode(AudioTrack.MODE_STREAM).build();
        audio.play();
        emuThread = new Thread(this::emuLoop, "emu");
        emuThread.start();
    }

    private void emuLoop() {
        int[] px = new int[Native.W * Native.H];
        short[] snd = new short[4096];
        long next = System.nanoTime();
        long fpsStart = System.nanoTime();
        int frames = 0;
        while (running) {
            Native.setInput(view.getButtons() | keyMask | stickMask, view.isTouchDown(), view.getTouchX(), view.getTouchY());
            boolean alive = Native.runFrame(px);
            view.pushFrame(px);
            int n;
            while ((n = Native.readAudio(snd)) > 0) audio.write(snd, 0, n * 2, AudioTrack.WRITE_NON_BLOCKING);
            if (!alive) {
                final String why = Native.exitReason();
                runOnUiThread(() -> gameEnded(why));
                running = false;
                break;
            }
            frames++;
            long now = System.nanoTime();
            if (now - fpsStart >= 1_000_000_000L) {
                view.setFps(frames * 1e9f / (now - fpsStart));
                frames = 0;
                fpsStart = now;
            }
            next += 16_666_667L;
            long sleep = next - System.nanoTime();
            if (sleep > 0) {
                try { Thread.sleep(sleep / 1_000_000L, (int) (sleep % 1_000_000L)); } catch (InterruptedException ignored) { }
            } else if (sleep < -100_000_000L) {
                next = System.nanoTime();
            }
        }
    }

    private void gameEnded(String why) {
        finishedGame = true;
        String log = Native.console();
        boolean normal = why.startsWith("Program exited with code 0") || why.contains("returned to the menu");
        if (normal) { finish(); return; }
        view.setMessage("The game stopped:\n" + wrap(why) + "\n\nTap here to go back\n(EMU > Show log for details)");
        view.showConsole(log);
    }

    void onMessageTapped() { if (finishedGame) finish(); }

    private void stopEmu() {
        running = false;
        if (emuThread != null) {
            try { emuThread.join(); } catch (InterruptedException ignored) { }
            emuThread = null;
        }
        if (audio != null) { audio.release(); audio = null; }
    }

    void showMenu() {
        new AlertDialog.Builder(this)
                .setTitle("GP2XDroid")
                .setItems(new String[] {"Show log", "Restart game", "Quit game"}, (d, which) -> {
                    if (which == 0) view.showConsole(Native.console());
                    else if (which == 1) { stopEmu(); Native.shutdown(); startGame(); }
                    else finish();
                }).show();
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopEmu();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        resumeEmu();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopEmu();
        Native.shutdown();
    }

    // ---- physical keys / gamepads (Android face buttons follow the GP2X layout by position) ----
    private int keyToButton(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT: return Native.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Native.RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: return Native.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Native.DOWN;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_X: return Native.X;     // bottom
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_C: return Native.B;     // right
            case KeyEvent.KEYCODE_BUTTON_X: case KeyEvent.KEYCODE_Z: return Native.A;     // left
            case KeyEvent.KEYCODE_BUTTON_Y: case KeyEvent.KEYCODE_S: return Native.Y;     // top
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_Q: return Native.L;
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_W: return Native.R;
            case KeyEvent.KEYCODE_BUTTON_L2: return Native.VOLDOWN;
            case KeyEvent.KEYCODE_BUTTON_R2: return Native.VOLUP;
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_ENTER: return Native.START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_SPACE: return Native.SELECT;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return Native.PUSH;
            case KeyEvent.KEYCODE_BUTTON_MODE: return Native.HOME;
        }
        return 0;
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        int b = keyToButton(code);
        if (b != 0) { keyMask |= b; return true; }
        return super.onKeyDown(code, e);
    }

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        int b = keyToButton(code);
        if (b != 0) { keyMask &= ~b; return true; }
        return super.onKeyUp(code, e);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) != 0 && e.getAction() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_X), y = e.getAxisValue(MotionEvent.AXIS_Y);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int m = 0;
            if (x < -0.5f || hx < -0.5f) m |= Native.LEFT;
            if (x > 0.5f || hx > 0.5f) m |= Native.RIGHT;
            if (y < -0.5f || hy < -0.5f) m |= Native.UP;
            if (y > 0.5f || hy > 0.5f) m |= Native.DOWN;
            stickMask = m;
            return true;
        }
        return super.onGenericMotionEvent(e);
    }
}
