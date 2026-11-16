package com.hcode91.pocketpenguin;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;

/** Read-only FAT12/16/32 reader for SD card images (.img dumps of GP2X/Wiz/Caanoo cards). */
public class FatImage {
    public static class Entry {
        public String name;
        public boolean dir;
        public long size;
        public long cluster;
    }

    private final FileChannel ch;
    private final long partStart;
    private final int bps, spc;
    private final long fatStart, rootDirStart, dataStart;
    private final int rootEntries;
    private final int fatType;            // 12, 16 or 32
    private final long rootCluster;
    private final long clusterCount;
    private final byte[] fat;

    public FatImage(FileChannel ch) throws IOException {
        this.ch = ch;
        byte[] s0 = read(0, 512);
        long start = 0;
        if ((s0[510] & 0xFF) == 0x55 && (s0[511] & 0xFF) == 0xAA && !looksLikeBootSector(s0)) {
            // MBR: first FAT partition
            for (int i = 0; i < 4; i++) {
                int o = 446 + i * 16;
                int type = s0[o + 4] & 0xFF;
                long lba = u32(s0, o + 8);
                if (lba != 0 && (type == 1 || type == 4 || type == 6 || type == 0x0B || type == 0x0C || type == 0x0E)) { start = lba * 512; break; }
            }
            if (start == 0) throw new IOException("No FAT partition found on this card image");
        }
        partStart = start;
        byte[] b = read(partStart, 512);
        bps = u16(b, 11);
        spc = b[13] & 0xFF;
        int reserved = u16(b, 14), nfats = b[16] & 0xFF;
        rootEntries = u16(b, 17);
        long tot = u16(b, 19);
        if (tot == 0) tot = u32(b, 32);
        long fatSz = u16(b, 22);
        if (fatSz == 0) fatSz = u32(b, 36);
        if (bps < 512 || spc == 0 || nfats == 0 || fatSz == 0) throw new IOException("Not a FAT file system");
        long rootSecs = ((long) rootEntries * 32 + bps - 1) / bps;
        fatStart = partStart + (long) reserved * bps;
        rootDirStart = fatStart + nfats * fatSz * bps;
        dataStart = rootDirStart + rootSecs * bps;
        clusterCount = (tot - (reserved + nfats * fatSz + rootSecs)) / spc;
        fatType = clusterCount < 4085 ? 12 : clusterCount < 65525 ? 16 : 32;
        rootCluster = fatType == 32 ? u32(b, 44) : 0;
        long fatBytes = Math.min(fatSz * bps, (clusterCount + 2) * 4 + 8);
        fat = read(fatStart, (int) fatBytes);
    }

    private static boolean looksLikeBootSector(byte[] s) {
        int jmp = s[0] & 0xFF;
        int bps = (s[11] & 0xFF) | ((s[12] & 0xFF) << 8);
        return (jmp == 0xEB || jmp == 0xE9) && (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096);
    }

    private byte[] read(long pos, int len) throws IOException {
        ByteBuffer bb = ByteBuffer.allocate(len);
        while (bb.hasRemaining()) {
            int n = ch.read(bb, pos + bb.position());
            if (n < 0) break;
        }
        return bb.array();
    }

    private static int u16(byte[] b, int o) { return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8); }
    private static long u32(byte[] b, int o) { return ByteBuffer.wrap(b, o, 4).order(ByteOrder.LITTLE_ENDIAN).getInt() & 0xFFFFFFFFL; }

    private long next(long c) {
        if (fatType == 32) { int o = (int) (c * 4); return o + 4 <= fat.length ? u32(fat, o) & 0x0FFFFFFFL : 0x0FFFFFFF; }
        if (fatType == 16) { int o = (int) (c * 2); return o + 2 <= fat.length ? u16(fat, o) : 0xFFFF; }
        int o = (int) (c * 3 / 2);
        if (o + 2 > fat.length) return 0xFFF;
        int v = u16(fat, o);
        return (c & 1) != 0 ? v >> 4 : v & 0xFFF;
    }

    private boolean end(long c) {
        long eoc = fatType == 32 ? 0x0FFFFFF8L : fatType == 16 ? 0xFFF8 : 0xFF8;
        return c < 2 || c >= eoc || c >= clusterCount + 2;
    }

    private long clusterPos(long c) { return dataStart + (c - 2) * spc * bps; }

    private byte[] readDirData(long cluster) throws IOException {
        if (cluster == 0 && fatType != 32) return read(rootDirStart, rootEntries * 32);
        if (cluster == 0) cluster = rootCluster;
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        int guard = 0;
        while (!end(cluster) && guard++ < 65536) {
            out.write(read(clusterPos(cluster), spc * bps));
            cluster = next(cluster);
        }
        return out.toByteArray();
    }

    /** List a directory (cluster 0 = root). */
    public List<Entry> list(long cluster) throws IOException {
        byte[] d = readDirData(cluster);
        List<Entry> out = new ArrayList<>();
        StringBuilder lfn = new StringBuilder();
        for (int o = 0; o + 32 <= d.length; o += 32) {
            int first = d[o] & 0xFF;
            if (first == 0) break;
            if (first == 0xE5) { lfn.setLength(0); continue; }
            int attr = d[o + 11] & 0xFF;
            if (attr == 0x0F) {
                StringBuilder part = new StringBuilder();
                int[] offs = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int x : offs) {
                    int chr = u16(d, o + x);
                    if (chr == 0 || chr == 0xFFFF) break;
                    part.append((char) chr);
                }
                if ((first & 0x40) != 0) lfn.setLength(0);
                lfn.insert(0, part);
                continue;
            }
            if ((attr & 0x08) != 0) { lfn.setLength(0); continue; } // volume label
            String name;
            if (lfn.length() > 0) name = lfn.toString();
            else {
                String base = new String(d, o, 8, "ISO-8859-1").trim();
                String ext = new String(d, o + 8, 3, "ISO-8859-1").trim();
                if ((d[o + 12] & 0x08) != 0) base = base.toLowerCase();
                if ((d[o + 12] & 0x10) != 0) ext = ext.toLowerCase();
                name = ext.isEmpty() ? base : base + "." + ext;
            }
            lfn.setLength(0);
            if (name.equals(".") || name.equals("..")) continue;
            Entry e = new Entry();
            e.name = name;
            e.dir = (attr & 0x10) != 0;
            e.size = u32(d, o + 28);
            e.cluster = ((long) u16(d, o + 20) << 16 | u16(d, o + 26));
            if (fatType != 32) e.cluster &= 0xFFFF;
            out.add(e);
        }
        return out;
    }

    public void extractFile(Entry e, File dest) throws IOException {
        try (OutputStream os = new FileOutputStream(dest)) {
            long left = e.size, c = e.cluster;
            int csz = spc * bps;
            while (left > 0 && !end(c)) {
                int n = (int) Math.min(left, csz);
                os.write(read(clusterPos(c), n));
                left -= n;
                c = next(c);
            }
        }
    }

    /** Extract a directory tree. */
    public void extractDir(long cluster, File dest) throws IOException {
        dest.mkdirs();
        for (Entry e : list(cluster)) {
            String safe = e.name.replace('/', '_').replace('\\', '_');
            File out = new File(dest, safe);
            if (e.dir) extractDir(e.cluster, out);
            else extractFile(e, out);
        }
    }

    /** Value of key= in a GPH menu .ini, or null. */
    public static String iniValue(String text, String key) {
        for (String line : text.split("\r?\n")) {
            String t = line.trim();
            int eq = t.indexOf('=');
            if (eq > 0 && t.substring(0, eq).trim().equalsIgnoreCase(key)) {
                String v = t.substring(eq + 1).trim();
                if (v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) v = v.substring(1, v.length() - 1);
                return v;
            }
        }
        return null;
    }
}
