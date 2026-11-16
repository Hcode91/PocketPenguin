// GP2XDroid - high-level emulation of the Wiz/Caanoo OpenGL ES library (libopengles_lite.so).
//
// The Wiz has the same MagicEyes GA3D 3D engine as the LeapFrog Didj. Rather than emulating that
// undocumented hardware, calls into libopengles_lite.so's exported gl*/egl* functions are trapped
// and run by the OpenGL ES 1.1 software renderer shared with DidjDroid (gles.cpp).
#pragma once
#include "arm926.h"
#include <functional>
#include <map>
#include <string>

namespace gp2x {

class Machine;
class Linux;
class GLES;

class Hle {
public:
    Hle(Machine* sys, Linux* linux);
    ~Hle();

    // Called when a file is mapped into guest memory: spots libopengles_lite.so.
    void on_map(const std::string& host_path, u32 va, u32 len, u64 off);

    bool log_calls = false;
    std::function<void(const std::string&)> log;
    GLES* gl;
    ARM926* cpu;
    Machine* sys;
    Linux* linux;
    bool active() const { return lib_base != 0; }

    // ---- guest memory helpers ----
    bool read_mem(u32 va, void* dst, u32 len);
    bool write_mem(u32 va, const void* src, u32 len);
    u32 rd32(u32 va) { u32 v = 0; read_mem(va, &v, 4); return v; }
    void wr32(u32 va, u32 v) { write_mem(va, &v, 4); }
    std::string read_str(u32 va, u32 max = 256);
    u32 arg(int i);   // i-th 32-bit argument (AAPCS)
    float argf(int i) { u32 v = arg(i); float f; memcpy(&f, &v, 4); return f; }
    u32 scratch_alloc(u32 len);
    void wait_next_frame();   // block the calling thread until the next video frame, then re-run the call

private:
    u32 lib_base = 0, lib_size = 0;
    std::map<u32, int> funcs;     // guest address -> GLES function id
    u32 scratch_page = 0, scratch_next = 0;
    static bool hook(ARM926* cpu, u32 pc, void* user);
};

} // namespace gp2x
