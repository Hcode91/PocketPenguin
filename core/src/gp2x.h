// GP2XDroid - GP2X F100/F200, Wiz and Caanoo machine model.
// Provides the hardware registers games reach through /dev/mem (MMSP2 on the F100/F200, Pollux on
// the Wiz/Caanoo), the device files (/dev/fb0, /dev/GPIO, /dev/dsp, /dev/mixer, touchscreen,
// joystick), display output and audio output.
#pragma once
#include "linux.h"
#include <deque>
#include <map>

namespace gp2x {

class Hle;

enum class Model { F100, F200, WIZ, CAANOO };

// Canonical button bits (GP2X layout, also used by the Wiz /dev/GPIO driver).
enum Button : u32 {
    BTN_UP = 1u << 0, BTN_UPLEFT = 1u << 1, BTN_LEFT = 1u << 2, BTN_DOWNLEFT = 1u << 3,
    BTN_DOWN = 1u << 4, BTN_DOWNRIGHT = 1u << 5, BTN_RIGHT = 1u << 6, BTN_UPRIGHT = 1u << 7,
    BTN_START = 1u << 8, BTN_SELECT = 1u << 9, BTN_L = 1u << 10, BTN_R = 1u << 11,
    BTN_A = 1u << 12, BTN_B = 1u << 13, BTN_X = 1u << 14, BTN_Y = 1u << 15,
    BTN_VOLUP = 1u << 16, BTN_VOLDOWN = 1u << 17, BTN_PUSH = 1u << 18, BTN_HOME = 1u << 19,
};

class Machine : public Hardware {
public:
    static const int W = 320, H = 240;
    static const u32 FB_PHYS = 0x03101000;
    static const u32 FB_SIZE = 320 * 240 * 4 * 2;
    static const int OUT_RATE = 44100;

    explicit Machine(Model m);
    ~Machine() override;

    Model model;
    Linux linux;
    Hle* gl_hle = nullptr;                    // OpenGL ES (libopengles_lite.so) emulation
    u64 frame_count = 0;
    void present_gl(const u32* src, int w, int h);  // show a frame rendered by the GL emulation

    // Start a game. 'root' is the host folder used as "/", 'game' a guest path such as
    // /mnt/sd/game/game.gpe. cpu_mhz sets the virtual CPU speed.
    bool start(const std::string& root, const std::string& game, int cpu_mhz, std::string* err,
               const std::vector<std::string>& args = {});
    void run_frame();
    const u32* framebuffer() const { return fb; }
    bool running() const { return started && !linux.exited; }

    // Input
    void set_buttons(u32 mask);                 // Button bits; diagonals are derived here
    void set_touch(bool down, int x, int y);    // screen coordinates (F200)
    void set_stick(int x, int y);               // -32767..32767 (Caanoo analog stick)

    // Audio: interleaved stereo s16 at OUT_RATE. Returns frames read.
    size_t audio_read(s16* out, size_t frames);

    // Hardware interface
    u8* ram_page(u32 pa) override;
    bool is_io(u32 pa) override;
    u32 io_read(u32 pa, int size) override;
    void io_write(u32 pa, u32 v, int size) override;
    std::shared_ptr<File> open_device(Linux& L, const std::string& path, int flags, int* err) override;
    std::string cpuinfo() override;
    const char* machine() override;

    // ---- used by the device files ----
    bool pollux() const { return model == Model::WIZ || model == Model::CAANOO; }
    u64 frame_cycles = 0;
    u64 next_vblank(u64 t) const;              // start of the next vertical blank after t
    bool in_vblank(u64 t) const;
    void set_fb_mode(int bpp, u32 offset, u32 line_length);
    u32 fb_bpp = 16, fb_line = 640, fb_yoff = 0;
    u32 palette[256];
    u32 buttons = 0;
    std::map<u32, u64>* io_stats = nullptr;    // debug: register access counts (read: off, write: off|1<<31)
    u32 gpio_value();                           // /dev/GPIO read value
    // touch
    bool touch_down = false;
    int touch_x = 0, touch_y = 0;
    u32 touch_seq = 0;
    // stick
    int stick_x = 0, stick_y = 0;
    u32 input_seq = 0;
    // audio
    struct Audio {
        int rate = 22050, channels = 2, bits = 16;
        u32 capacity = 16384;                  // bytes
        std::deque<s16> ring;                  // interleaved stereo frames at 'rate'
        u64 out_done = 0;                      // output frames produced so far
        double pos = 0;                        // fractional input position
        s16 last[2] = {0, 0};
        int vol_l = 100, vol_r = 100;
    } audio;
    std::deque<s16> out;                       // produced output (stereo)
    void update_audio();
    u32 audio_queued_bytes() const;

private:
    std::vector<u8> ram;
    std::vector<u16> regs;                     // 0xC0000000..0xC001FFFF as 16-bit registers
    u32 fb[W * H];
    bool started = false;
    u64 frame_no = 0;
    // vsync polling detection
    int poll_count = 0;
    bool poll_level = false;
    u64 dpc_clear_time = 0;
    u32 mlc_addr_reg = 0x290E;                 // last written address pair (odd 0x290E / even 0x2912)
    int pal_index = 0;
    bool pal_hi = false;
    u32 pal_lo = 0;
    // MMSP2 2D accelerator ("FastIO" block at 0xE0020000, also seen at 0xE0024000)
    u32 blit_regs[64] = {0};
    u8 blit_pat[128] = {0};
    u32 blit_read(u32 off);
    void blit_write(u32 off, u32 v, int size);
    void blit_run();
    void render();
    void render_mmsp2();
    void render_pollux();
    u16 gpio_pins(int port);
};

} // namespace gp2x
