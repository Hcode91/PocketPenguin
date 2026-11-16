// GP2XDroid - GP2X F100/F200, Wiz and Caanoo machine model.
#include "gp2x.h"
#include "glhle.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <cstdlib>

namespace gp2x {

static const u32 IO_BASE = 0xC0000000, IO_SIZE = 0x20000;
static const u32 FASTIO_BASE = 0xE0000000, FASTIO_SIZE = 0x100000;

// ---------------------------------------------------------------------------
// Device files
// ---------------------------------------------------------------------------
class MemDev : public File {
public:
    Machine* m;
    s64 pos = 0;
    explicit MemDev(Machine* mm) : m(mm) {}
    s32 mmap(Linux& L, u32 va, u32 len, u32, u32, u64 off) override { L.mem.map_phys(va, len, (u32)off); return 0; }
    s64 lseek(Linux&, s64 off, int whence) override { if (whence == 0) pos = off; else if (whence == 1) pos += off; return pos; }
    s32 read(Linux& L, u32 buf, u32 len) override {
        for (u32 i = 0; i < len; i++) {
            u32 pa = (u32)(pos + i);
            u8 v = 0;
            if (u8* p = m->ram_page(pa & ~0xFFFu)) v = p[pa & 0xFFF];
            else if (m->is_io(pa)) v = (u8)m->io_read(pa, 1);
            L.mem.write(buf + i, &v, 1);
        }
        pos += len;
        return (s32)len;
    }
    s32 write(Linux& L, u32 buf, u32 len) override {
        for (u32 i = 0; i < len; i++) {
            u32 pa = (u32)(pos + i);
            u8 v = 0;
            L.mem.read(buf + i, &v, 1);
            if (u8* p = m->ram_page(pa & ~0xFFFu)) p[pa & 0xFFF] = v;
            else if (m->is_io(pa)) m->io_write(pa, v, 1);
        }
        pos += len;
        return (s32)len;
    }
};

class FbDev : public File {
public:
    Machine* m;
    u32 base;
    s64 pos = 0;
    FbDev(Machine* mm, u32 b) : m(mm), base(b) {}
    void put_var(Linux& L, u32 a) {
        u32 v[40];
        memset(v, 0, sizeof v);
        u32 bpp = m->fb_bpp;
        v[0] = Machine::W; v[1] = Machine::H; v[2] = Machine::W;
        v[3] = Machine::FB_SIZE / (Machine::W * std::max<u32>(bpp / 8, 1));
        v[5] = m->fb_yoff; v[6] = bpp;
        if (bpp == 16) { v[8] = 11; v[9] = 5; v[11] = 5; v[12] = 6; v[14] = 0; v[15] = 5; }
        else if (bpp >= 24) { v[8] = 16; v[9] = 8; v[11] = 8; v[12] = 8; v[14] = 0; v[15] = 8; }
        else { v[9] = 8; v[12] = 8; v[15] = 8; }
        v[22] = 0xFFFFFFFF; v[23] = 0xFFFFFFFF; // height/width mm
        v[25] = 100000;
        L.mem.write(a, v, 160);
    }
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        switch (req) {
        case 0x4600: put_var(L, arg); return 0;
        case 0x4601: case 0x4606: { // FBIOPUT_VSCREENINFO, FBIOPAN_DISPLAY
            u32 bpp = L.mem.r32(arg + 24), yoff = L.mem.r32(arg + 20);
            if (req == 0x4606 || bpp == 0) bpp = m->fb_bpp;
            if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 16;
            if (bpp == 15) bpp = 16;
            u32 line = Machine::W * (bpp / 8);
            if ((u64)yoff * line + (u64)line * Machine::H > Machine::FB_SIZE) yoff = 0;
            m->set_fb_mode((int)bpp, base - Machine::FB_PHYS + yoff * line, line);
            m->fb_yoff = yoff;
            if (req == 0x4601) put_var(L, arg);
            return 0;
        }
        case 0x4602: { // FBIOGET_FSCREENINFO
            u8 f[68];
            memset(f, 0, sizeof f);
            strcpy((char*)f, m->pollux() ? "Pollux FB" : "MMSP2 FB");
            u32 smem = base, len = Machine::FB_SIZE, visual = m->fb_bpp == 8 ? 3 : 2, line = m->fb_line;
            memcpy(f + 16, &smem, 4); memcpy(f + 20, &len, 4); memcpy(f + 32, &visual, 4);
            u16 ypan = 1; memcpy(f + 38, &ypan, 2);
            memcpy(f + 44, &line, 4);
            L.mem.write(arg, f, 68);
            return 0;
        }
        case 0x4604: case 0x4605: { // FBIOGETCMAP / FBIOPUTCMAP
            u32 start = L.mem.r32(arg), n = L.mem.r32(arg + 4);
            u32 rp = L.mem.r32(arg + 8), gp = L.mem.r32(arg + 12), bp = L.mem.r32(arg + 16);
            for (u32 i = 0; i < n && start + i < 256; i++) {
                u16 r = 0, g = 0, b = 0;
                if (req == 0x4605) {
                    L.mem.read(rp + i * 2, &r, 2); L.mem.read(gp + i * 2, &g, 2); L.mem.read(bp + i * 2, &b, 2);
                    m->palette[start + i] = 0xFF000000u | ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
                } else {
                    u32 c = m->palette[start + i];
                    r = (u16)(((c >> 16) & 0xFF) * 257); g = (u16)(((c >> 8) & 0xFF) * 257); b = (u16)((c & 0xFF) * 257);
                    L.mem.write(rp + i * 2, &r, 2); L.mem.write(gp + i * 2, &g, 2); L.mem.write(bp + i * 2, &b, 2);
                }
            }
            return 0;
        }
        case 0x40044620: case 0x4620: // FBIO_WAITFORVSYNC
            return L.block_wait(nullptr, m->next_vblank(L.now()), 0);
        }
        return 0; // driver-specific ioctls: accept
    }
    s32 mmap(Linux& L, u32 va, u32 len, u32, u32, u64 off) override { L.mem.map_phys(va, len, base + (u32)off); return 0; }
    s64 lseek(Linux&, s64 off, int whence) override {
        if (whence == 0) pos = off; else if (whence == 1) pos += off; else pos = Machine::FB_SIZE + off;
        return pos;
    }
    s32 read(Linux& L, u32 buf, u32 len) override {
        if (pos >= Machine::FB_SIZE) return 0;
        len = std::min<u32>(len, (u32)(Machine::FB_SIZE - pos));
        for (u32 i = 0; i < len; i++) { u32 pa = base + (u32)pos + i; u8* p = m->ram_page(pa & ~0xFFFu); if (p) L.mem.write(buf + i, p + (pa & 0xFFF), 1); }
        pos += len;
        return (s32)len;
    }
    s32 write(Linux& L, u32 buf, u32 len) override {
        if (pos >= Machine::FB_SIZE) return -28; // ENOSPC
        len = std::min<u32>(len, (u32)(Machine::FB_SIZE - pos));
        for (u32 i = 0; i < len; i++) { u32 pa = base + (u32)pos + i; u8* p = m->ram_page(pa & ~0xFFFu); if (p) L.mem.read(buf + i, p + (pa & 0xFFF), 1); }
        pos += len;
        return (s32)len;
    }
    s32 fstat(Linux&, GuestStat* st) override { *st = GuestStat(); st->mode = 020660; st->rdev = 0x1d00; st->size = Machine::FB_SIZE; return 0; }
};

class GpioDev : public File {
public:
    Machine* m;
    explicit GpioDev(Machine* mm) : m(mm) {}
    s32 read(Linux& L, u32 buf, u32 len) override {
        u32 v = m->gpio_value();
        u32 n = std::min<u32>(len, 4);
        L.mem.write(buf, &v, n);
        return (s32)n;
    }
    s32 write(Linux&, u32, u32 len) override { return (s32)len; }
    s32 ioctl(Linux&, u32, u32) override { return 0; }
};

class DspDev : public File {
public:
    Machine* m;
    explicit DspDev(Machine* mm) : m(mm) {}
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        Machine::Audio& a = m->audio;
        auto get = [&]() { return (s32)L.mem.r32(arg); };
        auto put = [&](s32 v) { L.mem.w32(arg, (u32)v); };
        m->update_audio();
        switch (req) {
        case 0x5000: a.ring.clear(); return 0;                         // RESET
        case 0x5001: {                                                  // SYNC
            u32 q = m->audio_queued_bytes();
            if (!q) return 0;
            u32 bpf = (u32)(a.channels * a.bits / 8);
            return L.block_retry(&m->audio, L.now() + L.cycles_from_usec((u64)(q / bpf) * 1000000 / a.rate + 1));
        }
        case 0xC0045002: { s32 r = get(); if (r < 4000) r = 4000; if (r > 96000) r = 96000; a.rate = r; put(r); return 0; } // SPEED
        case 0xC0045003: a.channels = get() ? 2 : 1; put(a.channels - 1); return 0;     // STEREO
        case 0xC0045006: { s32 c = get(); a.channels = c >= 2 ? 2 : 1; put(a.channels); return 0; } // CHANNELS
        case 0xC0045005: { s32 f = get(); if (f == 8) a.bits = 8; else if (f == 0x10) a.bits = 16; put(a.bits == 8 ? 8 : 0x10); return 0; }
        case 0x8004500B: put(0x18); return 0;                           // GETFMTS
        case 0xC004500A: {                                              // SETFRAGMENT
            u32 v = (u32)get();
            u32 size = 1u << std::min<u32>(std::max<u32>(v & 0xFFFF, 7), 16);
            u32 cnt = std::max<u32>(std::min<u32>(v >> 16, 32), 2);
            a.capacity = std::min<u32>(std::max<u32>(size * cnt, 1024), 262144);
            return 0;
        }
        case 0xC0045004: put((s32)std::min<u32>(a.capacity / 4, 65536)); return 0; // GETBLKSIZE
        case 0x8010500C: {                                              // GETOSPACE
            u32 frag = std::max<u32>(a.capacity / 4, 1);
            u32 space = a.capacity - std::min(a.capacity, m->audio_queued_bytes());
            u32 v[4] = {space / frag, 4, frag, space};
            L.mem.write(arg, v, 16);
            return 0;
        }
        case 0x80045017: put((s32)m->audio_queued_bytes()); return 0;  // GETODELAY
        case 0x8004500F: put(0); return 0;                              // GETCAPS
        case 0x80045002: put(a.rate); return 0;
        case 0x80045006: put(a.channels); return 0;
        case 0x500E: flags |= 04000; return 0;                          // NONBLOCK
        }
        return 0;
    }
    s32 write(Linux& L, u32 buf, u32 len) override {
        Machine::Audio& a = m->audio;
        m->update_audio();
        u32 bpf = (u32)(a.channels * a.bits / 8);
        u32 q = m->audio_queued_bytes();
        u32 space = a.capacity > q ? a.capacity - q : 0;
        u32 take = len;
        if (len > space) {
            if (len <= a.capacity || space < bpf) {
                u32 need = (len <= a.capacity ? len - space : bpf);
                u64 us = (u64)(need / bpf + 1) * 1000000 / a.rate + 1;
                return L.block_retry(&m->audio, L.now() + L.cycles_from_usec(us));
            }
            take = space;
        }
        take -= take % bpf;
        std::vector<u8> d(take);
        L.mem.read(buf, d.data(), take);
        for (u32 i = 0; i + bpf <= take; i += bpf) {
            s16 l, r;
            if (a.bits == 16) { memcpy(&l, &d[i], 2); r = a.channels == 2 ? (s16)(d[i + 2] | (d[i + 3] << 8)) : l; }
            else { l = (s16)((d[i] - 128) << 8); r = a.channels == 2 ? (s16)((d[i + 1] - 128) << 8) : l; }
            a.ring.push_back(l);
            a.ring.push_back(r);
        }
        return (s32)take;
    }
    int poll_mask(Linux&) override {
        m->update_audio();
        return m->audio_queued_bytes() < m->audio.capacity ? 4 : 0;
    }
};

class MixerDev : public File {
public:
    Machine* m;
    u32 vals[32];
    explicit MixerDev(Machine* mm) : m(mm) { for (auto& v : vals) v = 0x6464; }
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        if ((req & 0xFFFFFF00) == 0x80044D00) { // MIXER_READ
            u32 dev = req & 0xFF;
            u32 v = dev == 0xFE ? 0x11 : dev == 0xFD ? 0 : dev == 0xFB ? 0x11 : dev == 0xFC ? 0 : vals[dev & 31];
            L.mem.w32(arg, v);
            return 0;
        }
        if ((req & 0xFFFFFF00) == 0xC0044D00) { // MIXER_WRITE
            u32 dev = req & 0xFF, v = L.mem.r32(arg);
            vals[dev & 31] = v;
            if (dev == 0 || dev == 4) {
                u32 l = std::min<u32>(v & 0xFF, 100), r = std::min<u32>((v >> 8) & 0xFF, 100);
                m->audio.vol_l = (int)l; m->audio.vol_r = (int)r;
            }
            return 0;
        }
        return 0;
    }
};

class TouchDev : public File {
public:
    Machine* m;
    u32 seen = 0;
    explicit TouchDev(Machine* mm) : m(mm) {}
    s32 read(Linux& L, u32 buf, u32 len) override {
        if (len < 8) return -22;
        if (seen == m->touch_seq) return L.block_retry(m, L.now() + L.cycles_from_usec(10000));
        seen = m->touch_seq;
        u16 ev[4] = {(u16)(m->touch_down ? 1000 : 0), (u16)m->touch_x, (u16)m->touch_y, 0};
        u32 tv[2] = {(u32)((L.epoch_usec + L.usec()) / 1000000), (u32)((L.epoch_usec + L.usec()) % 1000000)};
        L.mem.write(buf, ev, 8);
        if (len >= 16) { L.mem.write(buf + 8, tv, 8); return 16; }
        return 8;
    }
    int poll_mask(Linux&) override { return seen != m->touch_seq ? 1 : 0; }
};

// Wiz/Caanoo touchscreen: Linux input-event device read by tslib (module_raw input). While the
// screen is held, the driver delivers a sample every 10 ms like the real one; release sends pressure 0.
class TouchEvDev : public File {
public:
    Machine* m;
    bool was_down = false;
    u64 next_sample = 0;
    explicit TouchEvDev(Machine* mm) : m(mm) {}
    std::deque<u8> queue;           // events not read yet
    bool due(Linux& L) const {
        bool down = m->touch_down;
        return (down && (!was_down || L.now() >= next_sample)) || (!down && was_down);
    }
    void generate(Linux& L) {
        bool down = m->touch_down;
        u64 us = L.epoch_usec + L.usec();
        auto ev = [&](u16 type, u16 code, s32 val) {
            u8 e[16]; u32 s = (u32)(us / 1000000), u = (u32)(us % 1000000);
            memcpy(e, &s, 4); memcpy(e + 4, &u, 4); memcpy(e + 8, &type, 2); memcpy(e + 10, &code, 2); memcpy(e + 12, &val, 4);
            queue.insert(queue.end(), e, e + 16);
        };
        if (down) {
            ev(3, 0, m->touch_x); ev(3, 1, m->touch_y); ev(3, 24, 255);
            if (!was_down) ev(1, 0x14A, 1);
        } else { ev(3, 24, 0); ev(1, 0x14A, 0); }
        ev(0, 0, 0);
        was_down = down;
        next_sample = L.now() + L.cycles_from_usec(10000);
    }
    s32 read(Linux& L, u32 buf, u32 len) override {
        if (len < 16) return -22;
        if (queue.empty()) {
            if (!due(L)) return L.block_retry(m, m->touch_down ? next_sample : ~0ull);
            generate(L);
        }
        u32 n = std::min<u32>((u32)queue.size(), len & ~15u);
        std::vector<u8> out(queue.begin(), queue.begin() + n);
        queue.erase(queue.begin(), queue.begin() + n);
        L.mem.write(buf, out.data(), n);
        return (s32)n;
    }
    int poll_mask(Linux& L) override { return (!queue.empty() || due(L)) ? 1 : 0; }
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        u32 nr = req & 0xFF, size = (req >> 16) & 0x3FFF;
        if (((req >> 8) & 0xFF) != 0x45) return -25;
        if (nr == 0x01) { L.mem.w32(arg, 0x010000); return 0; }                        // EVIOCGVERSION
        if (nr == 0x02) { u16 id[4] = {0x19, 0, 0, 0}; L.mem.write(arg, id, 8); return 0; } // EVIOCGID
        if (nr == 0x06) { const char* n = "Pollux Touchscreen"; u32 k = std::min<u32>(size, 19); L.mem.write(arg, n, k); return (s32)k; }
        if (nr >= 0x20 && nr < 0x40) {                                                   // EVIOCGBIT(ev, len)
            // Like the kernel, copy only the real bitmap size (EV_MAX 0x1f, KEY_MAX 0x1ff, ABS_MAX 0x3f).
            u32 ev = nr - 0x20;
            u32 real = ev == 0 ? 4 : ev == 1 ? 64 : ev == 3 ? 8 : 0;
            size = std::min(size, real);
            if (!size) return 0;
            std::vector<u8> bits(size, 0);
            auto set = [&](u32 b) { if (b / 8 < size) bits[b / 8] |= (u8)(1u << (b % 8)); };
            if (ev == 0) { set(0); set(1); set(3); }
            else if (ev == 1) set(0x14A);
            else if (ev == 3) { set(0); set(1); set(24); }
            L.mem.write(arg, bits.data(), size);
            return (s32)size;
        }
        if (nr >= 0x40 && nr < 0x80) {                                                   // EVIOCGABS(abs)
            u32 abs = nr - 0x40;
            s32 info[6] = {0, 0, abs == 0 ? 319 : abs == 1 ? 239 : 255, 0, 0, 0};
            L.mem.write(arg, info, std::min<u32>(size ? size : 24, 24));
            return 0;
        }
        return 0; // EVIOCGRAB etc.
    }
};

class JsDev : public File {
public:
    Machine* m;
    bool init = false;
    u32 last_buttons = 0;
    int last_axis[2] = {0, 0};
    explicit JsDev(Machine* mm) : m(mm) {}
    static int caanoo_index(int i) {
        // Caanoo joystick buttons: A=0 X=1 B=2 Y=3 L=4 R=5 HOME=6 HOLD=7 HELP1=8 HELP2=9
        static const u32 map[10] = {BTN_A, BTN_X, BTN_B, BTN_Y, BTN_L, BTN_R, BTN_HOME, 0, BTN_START, BTN_SELECT};
        return (int)map[i];
    }
    void axes(int* ax) {
        ax[0] = m->stick_x; ax[1] = m->stick_y;
        if (m->buttons & BTN_LEFT) ax[0] = -32767;
        if (m->buttons & BTN_RIGHT) ax[0] = 32767;
        if (m->buttons & BTN_UP) ax[1] = -32767;
        if (m->buttons & BTN_DOWN) ax[1] = 32767;
    }
    s32 read(Linux& L, u32 buf, u32 len) override {
        std::vector<u8> out;
        u32 t = (u32)(L.usec() / 1000);
        auto ev = [&](s16 val, u8 type, u8 num) {
            u8 e[8]; memcpy(e, &t, 4); memcpy(e + 4, &val, 2); e[6] = type; e[7] = num;
            out.insert(out.end(), e, e + 8);
        };
        int ax[2];
        axes(ax);
        u32 b = 0;
        for (int i = 0; i < 10; i++) if (m->buttons & (u32)caanoo_index(i)) b |= 1u << i;
        if (!init) {
            init = true;
            for (int i = 0; i < 10; i++) ev((s16)((b >> i) & 1), 0x81, (u8)i);
            for (int i = 0; i < 2; i++) ev((s16)ax[i], 0x82, (u8)i);
        } else {
            for (int i = 0; i < 10; i++) if (((b ^ last_buttons) >> i) & 1) ev((s16)((b >> i) & 1), 1, (u8)i);
            for (int i = 0; i < 2; i++) if (ax[i] != last_axis[i]) ev((s16)ax[i], 2, (u8)i);
        }
        last_buttons = b; last_axis[0] = ax[0]; last_axis[1] = ax[1];
        if (out.empty()) return L.block_retry(m, L.now() + L.cycles_from_usec(10000));
        u32 n = std::min<u32>((u32)out.size(), len & ~7u);
        L.mem.write(buf, out.data(), n);
        return (s32)n;
    }
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        switch (req) {
        case 0x80046A01: L.mem.w32(arg, 0x020100); return 0;      // JSIOCGVERSION
        case 0x80016A11: { u8 v = 2; L.mem.write(arg, &v, 1); return 0; }  // JSIOCGAXES
        case 0x80016A12: { u8 v = 10; L.mem.write(arg, &v, 1); return 0; } // JSIOCGBUTTONS
        }
        if ((req & 0xC000FFFF) == 0x80006A13) { const char* n = "GP2X Caanoo"; L.mem.write(arg, n, 12); return 12; } // JSIOCGNAME
        if (((req >> 8) & 0xFF) == 0x6A) return 0;
        return -25; // ENOTTY: not an evdev device
    }
    int poll_mask(Linux&) override { return 1; }
};

// /dev/ga3d: the 3D engine. Its work is done by the OpenGL ES emulation, so this only has to
// satisfy the Wiz's libglport (open, mmap, ioctl).
class Ga3dDev : public File {
public:
    s32 ioctl(Linux&, u32, u32) override { return 0; }
    s32 mmap(Linux& L, u32 va, u32 len, u32, u32, u64) override { L.mem.map_anon(va, len); return 0; }
};

class BattDev : public File {
public:
    s32 read(Linux& L, u32 buf, u32 len) override {
        u16 v = 1000;
        u32 n = std::min<u32>(len, 2);
        L.mem.write(buf, &v, n);
        return (s32)n;
    }
};

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------
static void set32(std::vector<u16>& r, u32 off, u32 v) { r[off >> 1] = (u16)v; r[(off >> 1) + 1] = (u16)(v >> 16); }
static u32 get32(const std::vector<u16>& r, u32 off) { return r[off >> 1] | ((u32)r[(off >> 1) + 1] << 16); }

Machine::Machine(Model m) : model(m), linux(this) {
    ram.assign(m == Model::CAANOO ? (128u << 20) : (64u << 20), 0);
    regs.assign(IO_SIZE / 2, 0);
    for (int i = 0; i < 256; i++) palette[i] = 0xFF000000u | (i << 16) | (i << 8) | i;
    memset(fb, 0, sizeof fb);
    if (pollux()) {
        set32(regs, 0x4000, 0x3);                       // MLCCONTROLT: enable
        set32(regs, 0x4004, (239u << 16) | 319);        // screen size
        set32(regs, 0x4040, 319);                       // layer 1 left/right
        set32(regs, 0x4044, 239);                       // layer 1 top/bottom
    }
    if (!pollux()) {
        // MMSP2 display controller: LCD timing as set up by the kernel (320x240 progressive)
        regs[0x2800 >> 1] = 0x0008;                     // DPC_CNTL
        regs[0x2816 >> 1] = 319;                        // DPC_X_MAX
        regs[0x2818 >> 1] = 239;                        // DPC_Y_MAX
    }
    set_fb_mode(16, 0, 640);
    gl_hle = new Hle(this, &linux);
    gl_hle->log = [this](const std::string& s) { linux.log("%s", s.c_str()); };
    gl_hle->log_calls = getenv("GP2X_GLLOG") != nullptr;
    linux.on_file_map = [this](const std::string& h, u32 va, u32 len, u64 off) { gl_hle->on_map(h, va, len, off); };
}

Machine::~Machine() { delete gl_hle; }

void Machine::present_gl(const u32* src, int w, int h) {
    // Draw into the buffer the display currently shows (Wiz/Caanoo: MLC layer 1; GP2X: RGB region).
    u32 addr, vstride;
    if (pollux()) { addr = get32(regs, 0x406C); vstride = get32(regs, 0x4060); }
    else { addr = regs[mlc_addr_reg >> 1] | ((u32)regs[(mlc_addr_reg + 2) >> 1] << 16); vstride = regs[0x290C >> 1]; }
    if (!vstride || vstride > 8192) vstride = 640;
    for (int y = 0; y < h && y < H; y++)
        for (int x = 0; x < w && x < W; x++) {
            u64 pa = addr + (u64)y * vstride + (u64)x * 2;
            if (pa + 2 > ram.size()) continue;
            u32 c = src[y * w + x];
            u16 v = (u16)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31));
            memcpy(&ram[pa], &v, 2);
        }
}

void Machine::set_fb_mode(int bpp, u32 offset, u32 line) {
    fb_bpp = (u32)bpp;
    fb_line = line;
    u32 addr = FB_PHYS + offset;
    if (pollux()) {
        u32 fmt = bpp == 16 ? 0x4432 : bpp == 8 ? 0x443A : 0x4653;
        u32 hs = bpp == 32 ? 4 : bpp == 24 ? 3 : bpp == 8 ? 1 : 2;
        set32(regs, 0x4058, (fmt << 16) | (1u << 5));
        set32(regs, 0x405C, hs);
        set32(regs, 0x4060, line);
        set32(regs, 0x406C, addr);
    } else {
        u16 mode = bpp == 8 ? 1 : bpp >= 24 ? 3 : 2;
        regs[0x28DA >> 1] = (u16)((mode << 9) | 0xAB);
        regs[0x290C >> 1] = (u16)line;
        regs[0x290E >> 1] = (u16)addr; regs[0x2910 >> 1] = (u16)(addr >> 16);
        regs[0x2912 >> 1] = (u16)addr; regs[0x2914 >> 1] = (u16)(addr >> 16);
        regs[0x2906 >> 1] = 1024;
        u32 vsc = 320u * (bpp == 8 ? 1 : bpp >= 24 ? 3 : 2);
        regs[0x2908 >> 1] = (u16)vsc; regs[0x290A >> 1] = (u16)(vsc >> 16);
    }
}

bool Machine::start(const std::string& root, const std::string& game, int cpu_mhz, std::string* err,
                    const std::vector<std::string>& args) {
    linux.root = root;
    linux.cpu_hz = (u64)std::max(cpu_mhz, 10) * 1000000ull;
    frame_cycles = linux.cpu_hz / 60;
    // tslib calibration for the F200 touchscreen (identity: the emulator reports screen pixels)
    std::string etc = root + "/etc";
    mkdir(etc.c_str(), 0755);
    std::string pc = etc + "/pointercal";
    // tslib calibration: the emulated touchscreens report screen pixels, so always use identity.
    if (FILE* f = fopen(pc.c_str(), "w")) { fputs("65536 0 0 0 65536 0 65536\n", f); fclose(f); }
    std::string tsdir = etc + "/tslib", tsconf = tsdir + "/ts.conf-input";
    mkdir(tsdir.c_str(), 0755);
    struct stat s;
    if (stat(tsconf.c_str(), &s) != 0) {
        if (FILE* f = fopen(tsconf.c_str(), "w")) { fputs("module_raw input\nmodule linear\n", f); fclose(f); }
    }
    std::vector<std::string> argv = {game};
    argv.insert(argv.end(), args.begin(), args.end());
    if (!linux.load(game, argv, err)) return false;
    started = true;
    return true;
}

u64 Machine::next_vblank(u64 t) const {
    u64 vs = frame_cycles * 94 / 100;
    u64 c = (t / frame_cycles) * frame_cycles + vs;
    if (c <= t) c += frame_cycles;
    return c;
}

bool Machine::in_vblank(u64 t) const { return (t % frame_cycles) >= frame_cycles * 94 / 100; }

void Machine::run_frame() {
    if (running()) {
        u64 t = linux.now();
        u64 target = (t / frame_cycles + 1) * frame_cycles;
        poll_count = 0;
        linux.run_until(target);
        update_audio();
    }
    render();
    frame_no++;
    frame_count = frame_no;
}

void Machine::set_buttons(u32 mask) {
    if (mask & BTN_UP && mask & BTN_LEFT) mask |= BTN_UPLEFT;
    if (mask & BTN_UP && mask & BTN_RIGHT) mask |= BTN_UPRIGHT;
    if (mask & BTN_DOWN && mask & BTN_LEFT) mask |= BTN_DOWNLEFT;
    if (mask & BTN_DOWN && mask & BTN_RIGHT) mask |= BTN_DOWNRIGHT;
    if (mask != buttons) { buttons = mask; input_seq++; if (started) linux.wake(this); }
}

void Machine::set_touch(bool down, int x, int y) {
    if (down == touch_down && x == touch_x && y == touch_y) return;
    touch_down = down; touch_x = x; touch_y = y;
    touch_seq++;
    if (started) linux.wake(this);
}

void Machine::set_stick(int x, int y) {
    if (x == stick_x && y == stick_y) return;
    stick_x = x; stick_y = y;
    input_seq++;
    if (started) linux.wake(this);
}

u32 Machine::gpio_value() {
    u32 b = buttons & 0xFFFF;
    if (pollux()) {
        if (buttons & BTN_VOLUP) b |= 1u << 16;
        if (buttons & BTN_VOLDOWN) b |= 1u << 17;
        if (buttons & BTN_HOME) b |= 1u << 8;
    } else {
        if (buttons & BTN_VOLUP) b |= 1u << 23;
        if (buttons & BTN_VOLDOWN) b |= 1u << 22;
        if (buttons & BTN_PUSH) b |= 1u << 27;
    }
    return b;
}

// ---- Audio ----
u32 Machine::audio_queued_bytes() const {
    return (u32)(audio.ring.size() / 2) * (u32)(audio.channels * audio.bits / 8);
}

void Machine::update_audio() {
    if (!frame_cycles) return;
    u64 target = linux.now() * OUT_RATE / linux.cpu_hz;
    if (target <= audio.out_done) return;
    u64 n = target - audio.out_done;
    if (n > OUT_RATE) { audio.out_done = target - OUT_RATE; n = OUT_RATE; }
    double step = (double)audio.rate / OUT_RATE;
    for (u64 i = 0; i < n; i++) {
        s32 l = 0, r = 0;
        size_t frames = audio.ring.size() / 2;
        if (frames >= 2) {
            double f = audio.pos;
            l = (s32)(audio.ring[0] + (audio.ring[2] - audio.ring[0]) * f);
            r = (s32)(audio.ring[1] + (audio.ring[3] - audio.ring[1]) * f);
        } else if (frames == 1) { l = audio.ring[0]; r = audio.ring[1]; }
        if (frames) {
            audio.pos += step;
            while (audio.pos >= 1.0 && audio.ring.size() >= 2) { audio.ring.pop_front(); audio.ring.pop_front(); audio.pos -= 1.0; }
            if (audio.ring.empty()) audio.pos = 0;
        }
        out.push_back((s16)(l * audio.vol_l / 100));
        out.push_back((s16)(r * audio.vol_r / 100));
    }
    audio.out_done = target;
    while (out.size() > (size_t)OUT_RATE * 2) out.pop_front();
    linux.wake(&audio);
}

size_t Machine::audio_read(s16* dst, size_t frames) {
    size_t n = std::min(frames, out.size() / 2);
    for (size_t i = 0; i < n * 2; i++) { dst[i] = out.front(); out.pop_front(); }
    return n;
}

// ---- Physical memory / registers ----
u8* Machine::ram_page(u32 pa) {
    if (pa < ram.size()) return &ram[pa & ~0xFFFu];
    return nullptr;
}

bool Machine::is_io(u32 pa) {
    if (pa >= IO_BASE && pa < IO_BASE + IO_SIZE) return true;
    return !pollux() && pa >= FASTIO_BASE && pa < FASTIO_BASE + FASTIO_SIZE;
}

// ---------------------------------------------------------------------------
// MMSP2 2D accelerator: GDI ternary raster operations
// ---------------------------------------------------------------------------
enum { B_DSTCTRL = 0x00, B_DSTADDR = 0x04, B_DSTSTRIDE = 0x08, B_SRCCTRL = 0x0C, B_SRCADDR = 0x10,
       B_SRCSTRIDE = 0x14, B_SRCFG = 0x18, B_SRCBG = 0x1C, B_PATCTRL = 0x20, B_PATFG = 0x24, B_PATBG = 0x28,
       B_SIZE = 0x2C, B_CTRL = 0x30, B_RUN = 0x34 };

u32 Machine::blit_read(u32 off) {
    off &= 0xFF;
    if (off >= 0x80) { u32 v; memcpy(&v, blit_pat + ((off - 0x80) & ~3u), 4); return v; }
    if (off == B_RUN) return 0;                         // operations complete instantly
    return blit_regs[off >> 2];
}

void Machine::blit_write(u32 off, u32 v, int size) {
    off &= 0xFF;
    if (off >= 0x80) {
        u32 o = off - 0x80;
        if (size == 4) memcpy(blit_pat + (o & ~3u), &v, 4);
        else if (size == 2) memcpy(blit_pat + (o & ~1u), &v, 2);
        else blit_pat[o] = (u8)v;
        return;
    }
    u32& r = blit_regs[off >> 2];
    if (size == 4) r = v;
    else if (size == 2) { u32 sh = (off & 2) * 8; r = (r & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh); }
    else { u32 sh = (off & 3) * 8; r = (r & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
    if ((off & ~3u) == B_RUN && (r & 1)) { blit_run(); r &= ~1u; }
}

void Machine::blit_run() {
    u32 dctrl = blit_regs[B_DSTCTRL >> 2], sctrl = blit_regs[B_SRCCTRL >> 2], pctrl = blit_regs[B_PATCTRL >> 2];
    u32 ctrl = blit_regs[B_CTRL >> 2], size = blit_regs[B_SIZE >> 2];
    int w = (int)(size & 0x7FF), h = (int)((size >> 16) & 0x7FF);
    if (!w || !h) return;
    if (getenv("GP2X_BLITLOG"))
        fprintf(stderr, "blit dctrl %x daddr %x dstr %d sctrl %x saddr %x sstr %d pctrl %x pfg %x pbg %x sfg %x sbg %x size %dx%d ctrl %08x pat %08x%08x\n",
                dctrl, blit_regs[1], blit_regs[2], sctrl, blit_regs[4], blit_regs[5], pctrl, blit_regs[9], blit_regs[10],
                blit_regs[6], blit_regs[7], w, h, ctrl, *(u32*)blit_pat, *(u32*)(blit_pat + 4));
    bool d16 = dctrl & (1u << 5);
    u32 dbpp = d16 ? 2 : 1;
    u32 dfrac = dctrl & 31;
    s64 dstride = (s32)blit_regs[B_DSTSTRIDE >> 2];
    u32 daddr = (blit_regs[B_DSTADDR >> 2] & ~3u) + dfrac / 8;
    bool src_on = sctrl & (1u << 8);
    u32 sfmt = (sctrl >> 5) & 3;                       // 0 = 8bpp, 1 = 16bpp, 2 = 1bpp
    u32 sfrac = sctrl & 31;
    s64 sstride = (s32)blit_regs[B_SRCSTRIDE >> 2];
    u32 sbase = blit_regs[B_SRCADDR >> 2] & ~3u;
    bool pat_on = pctrl & (1u << 5);
    u32 pfmt = (pctrl >> 3) & 3, poff = pctrl & 7;
    u32 rop = ctrl & 0xFF;
    bool transp = ctrl & (1u << 11);
    u32 key = ctrl >> 16;
    bool xpos = ctrl & (1u << 9), ypos = ctrl & (1u << 10);
    u32 mask = d16 ? 0xFFFF : 0xFF;
    u8* ramp = ram.data();
    u64 rsz = ram.size();
    auto rd = [&](u64 a, u32 bpp) -> u32 {
        if (a + bpp > rsz) return 0;
        return bpp == 2 ? (u32)(ramp[a] | (ramp[a + 1] << 8)) : ramp[a];
    };
    // Read the whole source first so overlapping copies behave like the hardware.
    std::vector<u32> src;
    if (src_on) {
        src.resize((size_t)w * h);
        for (int y = 0; y < h; y++) {
            int sy = ypos ? y : -y;
            for (int x = 0; x < w; x++) {
                int sx = xpos ? x : -x;
                u32 v;
                if (sfmt == 2) {
                    s64 bit = (s64)sfrac + sx;
                    u64 a = (u64)((s64)sbase + (s64)sy * sstride + (bit >> 3));
                    u32 b = a < rsz ? (ramp[a] >> (7 - (bit & 7))) & 1 : 0;
                    v = b ? blit_regs[B_SRCFG >> 2] : blit_regs[B_SRCBG >> 2];
                } else {
                    u32 sb = sfmt == 1 ? 2 : 1;
                    u64 a = (u64)((s64)sbase + sfrac / 8 + (s64)sy * sstride + (s64)sx * sb);
                    v = rd(a, sb);
                }
                src[(size_t)y * w + x] = v & mask;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        int dy = ypos ? y : -y;
        for (int x = 0; x < w; x++) {
            int dx = xpos ? x : -x;
            u64 a = (u64)((s64)daddr + (s64)dy * dstride + (s64)dx * dbpp);
            if (a + dbpp > rsz) continue;
            u32 s = src_on ? src[(size_t)y * w + x] : 0;
            if (transp && src_on && s == (key & mask)) continue;
            u32 p = 0;
            if (pat_on || rop != 0xCC) {
                u32 px = (u32)(x + dfrac / (8 * dbpp)) & 7, py = (u32)(y + poff) & 7;
                if (pfmt == 2) p = ((blit_pat[py] >> (7 - px)) & 1) ? blit_regs[B_PATFG >> 2] : blit_regs[B_PATBG >> 2];
                else if (pfmt == 1) p = blit_pat[(py * 8 + px) * 2] | (blit_pat[(py * 8 + px) * 2 + 1] << 8);
                else p = blit_pat[py * 8 + px];
                if (!pat_on) p = blit_regs[B_PATFG >> 2];
            }
            u32 d = rd(a, dbpp), r;
            switch (rop) {
            case 0xCC: r = s; break;
            case 0xF0: r = p; break;
            case 0xAA: r = d; break;
            case 0x00: r = 0; break;
            case 0xFF: r = 0xFFFFFFFF; break;
            default:
                r = 0;
                for (int i = 0; i < 8; i++) {
                    if (!((rop >> i) & 1)) continue;
                    r |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
                }
            }
            r &= mask;
            ramp[a] = (u8)r;
            if (dbpp == 2) ramp[a + 1] = (u8)(r >> 8);
        }
    }
}

u16 Machine::gpio_pins(int port) {
    u16 v = 0xFFFF;
    switch (port) {
    case 1: { // GPIOB: bit 4 = vertical sync (low during vblank)
        u64 t = linux.now();
        bool level = !in_vblank(t);
        if (level == poll_level) {
            if (++poll_count > 32) {
                poll_count = 0;
                linux.skip_to(level ? next_vblank(t) : (t / frame_cycles + 1) * frame_cycles);
            }
        } else { poll_level = level; poll_count = 0; }
        if (!level) v &= ~(1u << 4);
        break;
    }
    case 2: v = (u16)~(buttons & 0xFF00); break;                       // GPIOC: START..Y
    case 3:                                                             // GPIOD: volume, stick push
        if (buttons & BTN_VOLUP) v &= ~(1u << 7);
        if (buttons & BTN_VOLDOWN) v &= ~(1u << 6);
        if (buttons & BTN_PUSH) v &= ~(1u << 11);
        break;
    case 12: v = (u16)(0xFF00 | (~buttons & 0xFF)); break;             // GPIOM: stick directions
    }
    return v;
}

u32 Machine::io_read(u32 pa, int size) {
    if (pa >= FASTIO_BASE) {
        if (io_stats) (*io_stats)[0x10000 | (pa & 0xFFFF)]++;
        u32 v = blit_read(pa);
        if (size == 4) return v;
        u32 sh = (pa & 3) * 8;
        return size == 2 ? (v >> (sh & 16)) & 0xFFFF : (v >> sh) & 0xFF;
    }
    u32 off = pa - IO_BASE;
    if (off >= IO_SIZE) return 0;
    if (io_stats) (*io_stats)[off]++;
    auto r16 = [&](u32 o) -> u16 {
        o &= ~1u;
        if (!pollux()) {
            if (o == 0x0A00 || o == 0x0A02) { // TCOUNT, 7.3728 MHz
                u32 t = (u32)(linux.now() * 7372800ull / linux.cpu_hz);
                return o == 0x0A00 ? (u16)t : (u16)(t >> 16);
            }
            if (o >= 0x1180 && o < 0x11A0) return gpio_pins((int)(o - 0x1180) / 2);
        } else {
            if (o >= 0x1800 && o < 0x1A00 && (o & 0x7F) < 4) { // TMRCOUNT (1 MHz)
                u32 t = (u32)linux.usec();
                return (o & 2) ? (u16)(t >> 16) : (u16)t;
            }
            if (o == 0x308C) { // DPCCTRL0: INTPEND (bit 10) set at each vblank
                u64 t = linux.now();
                u64 vs = frame_cycles * 94 / 100;
                u64 last = t >= vs ? ((t - vs) / frame_cycles) * frame_cycles + vs : 0;
                bool pend = t >= vs && last > dpc_clear_time;
                if (!pend && ++poll_count > 32) { poll_count = 0; linux.skip_to(next_vblank(t)); }
                return (u16)((regs[o >> 1] & ~(1u << 10)) | (pend ? (1u << 10) : 0));
            }
        }
        return regs[o >> 1];
    };
    if (size == 4) return r16(off) | ((u32)r16(off + 2) << 16);
    u16 v = r16(off);
    if (size == 2) return v;
    return (off & 1) ? (v >> 8) : (v & 0xFF);
}

void Machine::io_write(u32 pa, u32 v, int size) {
    if (pa >= FASTIO_BASE) {
        if (io_stats) (*io_stats)[0x80010000u | (pa & 0xFFFF)]++;
        blit_write(pa, v, size);
        return;
    }
    u32 off = pa - IO_BASE;
    if (off >= IO_SIZE) return;
    if (io_stats) (*io_stats)[off | 0x80000000u]++;
    auto w16 = [&](u32 o, u16 val) {
        o &= ~1u;
        if (!pollux()) {
            if (o == 0x290E || o == 0x2910) mlc_addr_reg = 0x290E;
            else if (o == 0x2912 || o == 0x2914) mlc_addr_reg = 0x2912;
            if (o == 0x2958) { pal_index = val & 0xFF; pal_hi = false; }
            else if (o == 0x295A) {
                if (!pal_hi) { pal_lo = val; pal_hi = true; }
                else {
                    palette[pal_index & 0xFF] = 0xFF000000u | ((u32)(val & 0xFF) << 16) | (pal_lo & 0xFFFF);
                    pal_index = (pal_index + 1) & 0xFF;
                    pal_hi = false;
                }
            }
        } else if (o == 0x308C) {
            if (val & (1u << 10)) dpc_clear_time = linux.now();
            val &= ~(1u << 10);
        }
        regs[o >> 1] = val;
    };
    if (size == 4) { w16(off, (u16)v); w16(off + 2, (u16)(v >> 16)); }
    else if (size == 2) w16(off, (u16)v);
    else {
        u16 cur = regs[(off & ~1u) >> 1];
        cur = (off & 1) ? (u16)((cur & 0x00FF) | ((v & 0xFF) << 8)) : (u16)((cur & 0xFF00) | (v & 0xFF));
        w16(off & ~1u, cur);
    }
}

std::shared_ptr<File> Machine::open_device(Linux& L, const std::string& p, int, int* err) {
    *err = 0;
    if (p == "/dev/mem") return std::make_shared<MemDev>(this);
    if (p == "/dev/fb0" || p == "/dev/fb/0") return std::make_shared<FbDev>(this, FB_PHYS);
    if (p == "/dev/fb1" || p == "/dev/fb/1") return std::make_shared<FbDev>(this, FB_PHYS + FB_SIZE);
    if (p == "/dev/GPIO" || p == "/dev/gpio") return std::make_shared<GpioDev>(this);
    if (p == "/dev/dsp" || p == "/dev/sound/dsp" || p == "/dev/dsp0" || p == "/dev/audio") return std::make_shared<DspDev>(this);
    if (p == "/dev/mixer" || p == "/dev/sound/mixer") return std::make_shared<MixerDev>(this);
    if (p == "/dev/touchscreen/wm97xx" || p == "/dev/touchscreen/0" || p == "/dev/ts") {
        if (model == Model::F100) { *err = 2; return nullptr; }
        return std::make_shared<TouchDev>(this);
    }
    if (p == "/dev/input/event0") {
        if (!pollux()) { *err = 2; return nullptr; }
        return std::make_shared<TouchEvDev>(this);
    }
    if (p == "/dev/input/js0" || p == "/dev/js0") {
        if (model != Model::CAANOO) { *err = 2; return nullptr; }
        return std::make_shared<JsDev>(this);
    }
    if (p == "/dev/batt" || p == "/dev/pollux_batt") return std::make_shared<BattDev>();
    if (p == "/dev/ga3d") return std::make_shared<Ga3dDev>();
    if (p == "/dev/mmc_nd") {
        L.stop("This game is copy-protected (INKA DRM from the GPH store). Its licence is kept in a protected area "
               "of the original SD card that a normal card dump does not contain, so it can't run.", 1);
        *err = 2;
        return nullptr;
    }
    // Never fall through to device nodes inside a firmware root folder.
    if (p.rfind("/dev/", 0) == 0 && p != "/dev/shm" && p.rfind("/dev/shm/", 0) != 0) { *err = 2; return nullptr; }
    return nullptr;
}

std::string Machine::cpuinfo() {
    if (pollux())
        return std::string("Processor\t: ARM926EJ-S rev 5 (v5l)\nBogoMIPS\t: 532.48\nFeatures\t: swp half thumb fastmult edsp java\n"
                           "CPU implementer\t: 0x41\nCPU architecture: 5TEJ\nCPU part\t: 0x926\nHardware\t: ") +
               (model == Model::WIZ ? "GPH Wiz (Pollux)" : "GPH Caanoo (Pollux)") + "\nRevision\t: 0000\n";
    return "Processor\t: ARM920Tid(wb) rev 0 (v4l)\nBogoMIPS\t: 199.47\nFeatures\t: swp half thumb\n"
           "CPU implementer\t: 0x41\nCPU architecture: 4T\nCPU part\t: 0x920\nHardware\t: GP2X (MMSP2)\nRevision\t: 0000\n";
}

const char* Machine::machine() { return pollux() ? "armv5tejl" : "armv4tl"; }

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void Machine::render() {
    if (!started) return;
    if (pollux()) render_pollux(); else render_mmsp2();
}

void Machine::render_mmsp2() {
    u16 cntl = regs[0x28DA >> 1];
    int mode = (cntl >> 9) & 3;
    u32 bytes = mode == 1 ? 1 : mode == 3 ? 3 : 2;
    u32 stride = regs[0x290C >> 1];
    if (!stride) stride = W * bytes;
    u32 addr = regs[mlc_addr_reg >> 1] | ((u32)regs[(mlc_addr_reg + 2) >> 1] << 16);
    u32 hsc = regs[0x2906 >> 1];
    if (!hsc) hsc = 1024;
    u32 vsc = regs[0x2908 >> 1] | ((u32)regs[0x290A >> 1] << 16);
    if (!vsc) vsc = 320 * bytes;
    const u8* base = ram.data();
    size_t size = ram.size();
    for (int y = 0; y < H; y++) {
        u64 line = (u64)y * vsc / (320 * bytes);
        u64 row = addr + line * stride;
        u32* d = fb + y * W;
        for (int x = 0; x < W; x++) {
            u64 pa = row + (u64)((u32)x * hsc / 1024) * bytes;
            if (pa + bytes > size) { d[x] = 0xFF000000u; continue; }
            const u8* p = base + pa;
            if (bytes == 2) {
                u16 v = (u16)(p[0] | (p[1] << 8));
                u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
                d[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            } else if (bytes == 1) d[x] = palette[p[0]] | 0xFF000000u;
            else d[x] = 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
        }
    }
}

static inline u32 conv_pixel(const u8* p, u32 fmt, u32 bpp, const u32* pal) {
    bool bgr = fmt & 0x8000;
    u32 r = 0, g = 0, b = 0;
    u32 f = fmt & 0x7FFF;
    if (bpp == 1) return pal[p[0]] | 0xFF000000u;
    if (bpp == 2) {
        u16 v = (u16)(p[0] | (p[1] << 8));
        switch (f) {
        case 0x4342: case 0x3342: r = ((v >> 10) & 0x1F) * 255 / 31; g = ((v >> 5) & 0x1F) * 255 / 31; b = (v & 0x1F) * 255 / 31; break;
        case 0x4211: case 0x2211: r = ((v >> 8) & 0xF) * 17; g = ((v >> 4) & 0xF) * 17; b = (v & 0xF) * 17; break;
        default: r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F; r = r * 255 / 31; g = g * 255 / 63; b = b * 255 / 31; break;
        }
    } else { b = p[0]; g = p[1]; r = p[2]; }
    if (bgr) std::swap(r, b);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void Machine::render_pollux() {
    u32 ctrlt = get32(regs, 0x4000);
    u32 bg = get32(regs, 0x4008) & 0xFFFFFF;
    for (int i = 0; i < W * H; i++) fb[i] = 0xFF000000u | bg;
    if (!(ctrlt & 2)) return;
    for (int l = 1; l >= 0; l--) {
        u32 b = 0x400C + (u32)l * 0x34;
        u32 lr = get32(regs, b), tb = get32(regs, b + 4), ctrl = get32(regs, b + 0x18);
        if (!(ctrl & (1u << 5))) continue;
        int left = (lr >> 16) & 0xFFF, right = lr & 0xFFF, top = (tb >> 16) & 0xFFF, bottom = tb & 0xFFF;
        if (left & 0x800) left -= 0x1000;
        if (top & 0x800) top -= 0x1000;
        s32 hstride = (s32)get32(regs, b + 0x1C), vstride = (s32)get32(regs, b + 0x20);
        u32 addr = get32(regs, b + 0x2C);
        u32 fmt = ctrl >> 16;
        u32 bpp = hstride > 0 && hstride <= 4 ? (u32)hstride : 2;
        for (int y = std::max(top, 0); y <= bottom && y < H; y++) {
            s64 row = (s64)addr + (s64)(y - top) * vstride;
            for (int x = std::max(left, 0); x <= right && x < W; x++) {
                s64 pa = row + (s64)(x - left) * hstride;
                if (pa < 0 || (u64)pa + bpp > ram.size()) continue;
                fb[y * W + x] = conv_pixel(&ram[(size_t)pa], fmt, bpp, palette);
            }
        }
    }
}

} // namespace gp2x
