package com.hcode91.gp2xdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Game library: import games (folder or zip), pick the model and launch. */
public class MainActivity extends Activity {
    private static final int PICK_GAME_DIR = 1, PICK_GAME_ZIP = 2, PICK_FW_DIR = 3, PICK_FW_ZIP = 4, PICK_SD_IMG = 5;
    private static final String[] MODEL_NAMES = {"GP2X F100", "GP2X F200 (touchscreen)", "GP2X Wiz", "GP2X Caanoo"};

    private File sdDir;
    private ListView list;
    private TextView empty;
    private final List<String> games = new ArrayList<>();
    private SharedPreferences prefs;
    private int fwModel;
    private AlertDialog busy;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("games", MODE_PRIVATE);
        sdDir = new File(getFilesDir(), "sd");
        sdDir.mkdirs();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(24, 24, 24, 24);
        TextView title = new TextView(this);
        title.setText("GP2XDroid");
        title.setTextSize(26);
        title.setTextColor(Color.WHITE);
        root.addView(title);
        TextView sub = new TextView(this);
        sub.setText("GP2X F100/F200, Wiz and Caanoo games. Tap a game to play, long-press for options.");
        sub.setPadding(0, 4, 0, 16);
        root.addView(sub);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(button("Add game folder", v -> pick(PICK_GAME_DIR)));
        row.addView(button("Add game .zip", v -> pick(PICK_GAME_ZIP)));
        root.addView(row);
        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.addView(button("Add SD card .img", v -> pick(PICK_SD_IMG)));
        row2.addView(button("Firmware files", v -> firmwareDialog()));
        root.addView(row2);

        empty = new TextView(this);
        empty.setText("\nNo games yet.\n\nAdd a game's folder (the one with the .gpe file and its data), a .zip of it, or a whole SD card dump (.img, also inside a .zip). "
                + "Most games run on their own. Games built against the console's system libraries also need "
                + "\"Firmware files\": the lib folders copied from your own device.");
        empty.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(empty);

        list = new ListView(this);
        list.setOnItemClickListener((p, v, pos, id) -> launch(games.get(pos)));
        list.setOnItemLongClickListener((p, v, pos, id) -> { gameOptions(games.get(pos)); return true; });
        root.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        refresh();
    }

    private Button button(String label, View.OnClickListener l) {
        Button bt = new Button(this);
        bt.setText(label);
        bt.setAllCaps(false);
        bt.setOnClickListener(l);
        bt.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return bt;
    }

    private void refresh() {
        games.clear();
        File[] fs = sdDir.listFiles();
        if (fs != null) {
            Arrays.sort(fs, (a, c) -> a.getName().compareToIgnoreCase(c.getName()));
            for (File f : fs) if (f.isDirectory()) games.add(f.getName());
        }
        List<String> labels = new ArrayList<>();
        for (String g : games) labels.add(g + "   (" + MODEL_NAMES[modelFor(g)] + ")");
        list.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, labels));
        empty.setVisibility(games.isEmpty() ? View.VISIBLE : View.GONE);
    }

    // ---------------------------------------------------------------- launching
    private static boolean launchable(String n) {
        String l = n.toLowerCase(Locale.ROOT);
        return l.endsWith(".gpe") || l.endsWith(".gpu") || l.endsWith(".sh");
    }

    private void findLaunchables(File dir, String rel, List<String> out, int depth) {
        File[] fs = dir.listFiles();
        if (fs == null) return;
        Arrays.sort(fs);
        for (File f : fs) {
            String r = rel.isEmpty() ? f.getName() : rel + "/" + f.getName();
            if (f.isDirectory() && depth < 2) findLaunchables(f, r, out, depth + 1);
            else if (f.isFile() && launchable(f.getName()) && !f.getName().equalsIgnoreCase("autorun.gpu")) out.add(r);
        }
    }

    private void launch(String g) {
        List<String> files = new ArrayList<>();
        findLaunchables(new File(sdDir, g), "", files, 0);
        String saved = prefs.getString(g + ".exe", null);
        if (saved != null && files.contains(saved)) { run(g, saved); return; }
        if (files.isEmpty()) { info("No .gpe file found", "The folder " + g + " has no .gpe, .gpu or .sh file to start."); return; }
        if (files.size() == 1) { run(g, files.get(0)); return; }
        String[] arr = files.toArray(new String[0]);
        new AlertDialog.Builder(this).setTitle("Which file starts the game?")
                .setItems(arr, (d, w) -> { prefs.edit().putString(g + ".exe", arr[w]).apply(); run(g, arr[w]); }).show();
    }

    private void run(String g, String exe) {
        Intent i = new Intent(this, EmuActivity.class);
        i.putExtra("model", modelFor(g));
        i.putExtra("mhz", prefs.getInt(g + ".mhz", 200));
        i.putExtra("game", "/mnt/sd/" + g + "/" + exe);
        startActivity(i);
    }

    private int modelFor(String g) {
        int m = prefs.getInt(g + ".model", -1);
        if (m >= 0) return m;
        m = detectModel(new File(sdDir, g));
        prefs.edit().putInt(g + ".model", m).apply();
        return m;
    }

    /** Guess the target console from the game's ARM program(s). */
    private int detectModel(File dir) {
        String name = dir.getName().toLowerCase(Locale.ROOT);
        if (name.contains("caanoo")) return Native.CAANOO;
        if (name.contains("wiz")) return Native.WIZ;
        File best = null;
        List<File> stack = new ArrayList<>();
        stack.add(dir);
        int seen = 0;
        while (!stack.isEmpty() && seen < 400) {
            File d = stack.remove(stack.size() - 1);
            File[] fs = d.listFiles();
            if (fs == null) continue;
            for (File f : fs) {
                seen++;
                if (f.isDirectory()) { stack.add(f); continue; }
                if (isElf(f) && (best == null || f.length() > best.length())) best = f;
            }
        }
        if (best == null) return Native.F200;
        try (InputStream in = new FileInputStream(best)) {
            byte[] h = new byte[52];
            if (in.read(h) < 52) return Native.F200;
            int flags = (h[36] & 0xFF) | ((h[37] & 0xFF) << 8) | ((h[38] & 0xFF) << 16) | ((h[39] & 0xFF) << 24);
            boolean eabi = (flags >>> 24) != 0;
            int phoff = (h[28] & 0xFF) | ((h[29] & 0xFF) << 8) | ((h[30] & 0xFF) << 16) | ((h[31] & 0xFF) << 24);
            int phnum = (h[44] & 0xFF) | ((h[45] & 0xFF) << 8);
            boolean dynamic = false;
            try (InputStream in2 = new FileInputStream(best)) {
                in2.skip(phoff);
                byte[] ph = new byte[32];
                for (int i = 0; i < phnum && i < 32; i++) {
                    if (in2.read(ph) < 32) break;
                    if (ph[0] == 3 && ph[1] == 0 && ph[2] == 0 && ph[3] == 0) dynamic = true;
                }
            }
            if (eabi) return Native.CAANOO;
            if (dynamic) return Native.WIZ;
            return Native.F200;
        } catch (Exception e) {
            return Native.F200;
        }
    }

    private static boolean isElf(File f) {
        if (f.length() < 1024) return false;
        try (InputStream in = new FileInputStream(f)) {
            byte[] m = new byte[4];
            return in.read(m) == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
        } catch (Exception e) {
            return false;
        }
    }

    private void gameOptions(String g) {
        new AlertDialog.Builder(this).setTitle(g)
                .setItems(new String[] {"Console: " + MODEL_NAMES[modelFor(g)], "CPU speed: " + prefs.getInt(g + ".mhz", 200) + " MHz",
                        "Choose start file again", "Delete game"}, (d, w) -> {
                    if (w == 0) {
                        new AlertDialog.Builder(this).setTitle("Run as")
                                .setSingleChoiceItems(MODEL_NAMES, modelFor(g), (d2, m) -> {
                                    prefs.edit().putInt(g + ".model", m).apply(); d2.dismiss(); refresh();
                                }).show();
                    } else if (w == 1) {
                        EditText et = new EditText(this);
                        et.setText(String.valueOf(prefs.getInt(g + ".mhz", 200)));
                        new AlertDialog.Builder(this).setTitle("Emulated CPU speed (MHz)")
                                .setMessage("Higher gives slow games more power but needs a faster phone. GP2X: 200, Wiz/Caanoo: up to 533.")
                                .setView(et).setPositiveButton("OK", (d2, x) -> {
                                    try { prefs.edit().putInt(g + ".mhz", Math.max(50, Math.min(800, Integer.parseInt(et.getText().toString().trim())))).apply(); }
                                    catch (NumberFormatException ignored) { }
                                }).show();
                    } else if (w == 2) {
                        prefs.edit().remove(g + ".exe").apply();
                        launch(g);
                    } else {
                        new AlertDialog.Builder(this).setMessage("Delete " + g + " and its saves?")
                                .setPositiveButton("Delete", (d2, x) -> { deleteTree(new File(sdDir, g)); prefs.edit().remove(g + ".model").remove(g + ".exe").remove(g + ".mhz").apply(); refresh(); })
                                .setNegativeButton("Cancel", null).show();
                    }
                }).show();
    }

    // ---------------------------------------------------------------- importing
    private void pick(int req) {
        Intent i;
        if (req == PICK_GAME_DIR || req == PICK_FW_DIR) i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
        }
        startActivityForResult(i, req);
    }

    private void firmwareDialog() {
        String[] items = {"GP2X F100/F200", "GP2X Wiz", "GP2X Caanoo"};
        new AlertDialog.Builder(this).setTitle("Add firmware files for")
                .setItems(items, (d, w) -> {
                    fwModel = w == 0 ? Native.F200 : w == 1 ? Native.WIZ : Native.CAANOO;
                    new AlertDialog.Builder(this).setTitle("Firmware files")
                            .setMessage("Pick a folder or .zip holding the console's root files (e.g. the lib and usr/lib folders copied "
                                    + "from your own device). They are merged into the emulated system so dynamically linked games can run.")
                            .setPositiveButton("Folder", (d2, x) -> pick(PICK_FW_DIR))
                            .setNeutralButton(".zip", (d2, x) -> pick(PICK_FW_ZIP))
                            .setNegativeButton("Cancel", null).show();
                }).show();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        busy = new AlertDialog.Builder(this).setMessage("Copying files...").setCancelable(false).show();
        new Thread(() -> {
            String err = null;
            importedCount = -1;
            try {
                switch (req) {
                    case PICK_SD_IMG: importImage(uri); break;
                    case PICK_GAME_DIR: importTree(uri, null); break;
                    case PICK_GAME_ZIP: importZip(uri, null); break;
                    case PICK_FW_DIR: importTree(uri, EmuActivity.firmwareDir(getFilesDir(), fwModel)); break;
                    case PICK_FW_ZIP: importZip(uri, EmuActivity.firmwareDir(getFilesDir(), fwModel)); break;
                }
            } catch (Exception e) {
                err = e.toString();
            }
            final String fe = err;
            runOnUiThread(() -> {
                if (busy != null) busy.dismiss();
                if (fe != null) info("Import failed", fe);
                else if (req == PICK_FW_DIR || req == PICK_FW_ZIP) info("Firmware files added", "They will be used for games running as " + MODEL_NAMES[fwModel] + ".");
                else if (importedCount == 0) info("No games found", "No folder with a .gpe/.gpu file was found on that SD card image.");
                else if (importedCount > 0) info("SD card imported", importedCount + " game(s) added.");
                refresh();
            });
        }).start();
    }

    private volatile int importedCount = -1;

    private void setBusy(String msg) { runOnUiThread(() -> { if (busy != null) busy.setMessage(msg); }); }

    /** Import every game folder from an SD card image picked with the document picker. */
    private void importImage(Uri uri) throws Exception {
        try (android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream in = new FileInputStream(pfd.getFileDescriptor())) {
            importImage(in.getChannel());
        }
    }

    private void importImage(java.nio.channels.FileChannel chn) throws Exception {
        FatImage img = new FatImage(chn);
        List<FatImage.Entry> root = img.list(0);
        // Menu .ini files on GPH cards name each game's start file: path="/folder/game.gpe"
        java.util.Map<String, String> iniExe = new java.util.HashMap<>();
        for (FatImage.Entry e : root) {
            if (e.dir || !e.name.toLowerCase(Locale.ROOT).endsWith(".ini") || e.size > 4096) continue;
            File t = File.createTempFile("ini", ".txt", getCacheDir());
            img.extractFile(e, t);
            String text = new String(java.nio.file.Files.readAllBytes(t.toPath()), "ISO-8859-1");
            t.delete();
            String path = FatImage.iniValue(text, "path");
            if (path == null) continue;
            path = path.replace('\\', '/');
            while (path.startsWith("/")) path = path.substring(1);
            int slash = path.indexOf('/');
            if (slash > 0) iniExe.put(path.substring(0, slash).toLowerCase(Locale.ROOT), path.substring(slash + 1));
        }
        int count = 0;
        List<Object[]> games = new ArrayList<>();
        findImageGames(img, root, 0, games);
        for (Object[] g : games) {
            FatImage.Entry e = (FatImage.Entry) g[0];
            setBusy("Copying " + e.name + "...");
            File dest = new File(sdDir, safeName(e.name));
            deleteTree(dest);
            img.extractDir(e.cluster, dest);
            String exe = iniExe.get(e.name.toLowerCase(Locale.ROOT));
            if (exe != null && new File(dest, exe).exists()) prefs.edit().putString(dest.getName() + ".exe", exe).apply();
            prefs.edit().remove(dest.getName() + ".model").apply();
            count++;
        }
        importedCount = count;
    }

    /** A game folder has a .gpe/.gpu/.sh in it or one level down; otherwise look inside (e.g. /game/...). */
    private void findImageGames(FatImage img, List<FatImage.Entry> entries, int depth, List<Object[]> out) throws Exception {
        for (FatImage.Entry e : entries) {
            if (!e.dir) continue;
            String n = e.name.toLowerCase(Locale.ROOT);
            if (depth == 0 && (n.equals("system volume information") || n.equals("recycled") || n.startsWith("$"))) continue;
            List<FatImage.Entry> kids = img.list(e.cluster);
            boolean game = false;
            for (FatImage.Entry k : kids) {
                if (!k.dir && launchable(k.name) && !k.name.equalsIgnoreCase("autorun.gpu")) game = true;
                if (k.dir && !game) for (FatImage.Entry k2 : img.list(k.cluster)) if (!k2.dir && launchable(k2.name)) { game = true; break; }
            }
            if (game) out.add(new Object[] {e});
            else if (depth < 2) findImageGames(img, kids, depth + 1, out);
        }
    }

    /** Copy a SAF tree. dest == null: new game folder named after the picked folder. */
    private void importTree(Uri tree, File dest) throws Exception {
        String docId = DocumentsContract.getTreeDocumentId(tree);
        String name = queryName(DocumentsContract.buildDocumentUriUsingTree(tree, docId));
        if (dest == null) {
            dest = new File(sdDir, safeName(name == null ? "game" : name));
            dest.mkdirs();
        }
        copyTree(tree, docId, dest);
    }

    private void copyTree(Uri tree, String docId, File dest) throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor c = getContentResolver().query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), n = safeName(c.getString(1)), mime = c.getString(2);
                File out = new File(dest, n);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    out.mkdirs();
                    copyTree(tree, id, out);
                } else {
                    try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                         OutputStream o = new FileOutputStream(out)) {
                        copy(in, o);
                    }
                }
            }
        }
    }

    private void importZip(Uri uri, File dest) throws Exception {
        String name = queryName(uri);
        if (name == null) name = "game";
        if (name.toLowerCase(Locale.ROOT).endsWith(".zip")) name = name.substring(0, name.length() - 4);
        File tmp = new File(getFilesDir(), "import_tmp");
        deleteTree(tmp);
        tmp.mkdirs();
        try (ZipInputStream z = new ZipInputStream(getContentResolver().openInputStream(uri))) {
            ZipEntry e;
            String base = tmp.getCanonicalPath() + File.separator;
            while ((e = z.getNextEntry()) != null) {
                File out = new File(tmp, e.getName());
                if (!out.getCanonicalPath().startsWith(base)) continue; // zip-slip guard
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) { copy(z, o); }
            }
        }
        // A zip holding an SD card image: import the card.
        if (dest == null) {
            File imgFile = findImageFile(tmp);
            if (imgFile != null) {
                try (FileInputStream in = new FileInputStream(imgFile)) { importImage(in.getChannel()); }
                deleteTree(tmp);
                return;
            }
        }
        // A zip holding a single top-level folder: use that folder.
        File src = tmp;
        File[] top = tmp.listFiles();
        if (top != null && top.length == 1 && top[0].isDirectory()) { src = top[0]; name = top[0].getName(); }
        if (dest == null) {
            File target = new File(sdDir, safeName(name));
            deleteTree(target);
            if (!src.renameTo(target)) { target.mkdirs(); mergeInto(src, target); }
        } else {
            mergeInto(src, dest);
        }
        deleteTree(tmp);
    }

    private static File findImageFile(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return null;
        for (File f : fs) {
            if (f.isDirectory()) { File r = findImageFile(f); if (r != null) return r; }
            else if (f.getName().toLowerCase(Locale.ROOT).endsWith(".img") && f.length() > (8 << 20)) return f;
        }
        return null;
    }

    private void mergeInto(File src, File dst) throws Exception {
        File[] fs = src.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            File t = new File(dst, f.getName());
            if (f.isDirectory()) { t.mkdirs(); mergeInto(f, t); }
            else if (!f.renameTo(t)) { try (InputStream in = new FileInputStream(f); OutputStream o = new FileOutputStream(t)) { copy(in, o); } }
        }
    }

    private String queryName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        return null;
    }

    private static String safeName(String n) {
        if (n == null || n.isEmpty()) return "unnamed";
        return n.replace('/', '_').replace('\\', '_');
    }

    private static void copy(InputStream in, OutputStream out) throws Exception {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    private static void deleteTree(File f) {
        File[] fs = f.listFiles();
        if (fs != null) for (File c : fs) deleteTree(c);
        f.delete();
    }

    private void info(String t, String m) {
        new AlertDialog.Builder(this).setTitle(t).setMessage(m).setPositiveButton("OK", null).show();
    }
}
