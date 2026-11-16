// GP2XDroid desktop runner, used for testing the core without Android.
//   gp2xcli [--model f100|f200|wiz|caanoo] [--root DIR] [--mhz N] [-f FRAMES] [-o shot.ppm]
//           [-k FRAME:MASK]... [--wav out.wav] [--mount /mnt/sd=HOSTDIR] [--strace] [--quiet] GAME [ARGS]
// GAME is a guest path (inside --root). Without --root the host filesystem is used as "/".
#include "../core/src/gp2x.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <chrono>

using namespace gp2x;

static void save_ppm(const char* path, const u32* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        u8 c[3] = {(u8)(px[i] >> 16), (u8)(px[i] >> 8), (u8)px[i]};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static void save_wav(const char* path, const std::vector<s16>& s) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 data = (u32)s.size() * 2, rate = Machine::OUT_RATE, br = rate * 4, sz = 36 + data, fmt = 16;
    u16 pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    Model model = Model::WIZ;
    std::string root = "/", out, wav, release;
    int frames = 600, mhz = 200;
    bool strace = false, quiet = false, iostat = false;
    std::map<u32, u64> stats;
    std::map<int, u32> keys;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> m_mounts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (!args.empty()) { args.push_back(a); continue; }
        if (a == "--model") {
            std::string m = next();
            model = m == "f100" ? Model::F100 : m == "f200" ? Model::F200 : m == "caanoo" ? Model::CAANOO : Model::WIZ;
        } else if (a == "--root") root = next();
        else if (a == "--mhz") mhz = atoi(next().c_str());
        else if (a == "-f") frames = atoi(next().c_str());
        else if (a == "-o") out = next();
        else if (a == "--wav") wav = next();
        else if (a == "--strace") strace = true;
        else if (a == "--iostat") iostat = true;

        else if (a == "--quiet") quiet = true;
        else if (a == "--release") release = next();
        else if (a == "--mount") { std::string mm = next(); size_t c = mm.find('='); m_mounts.push_back({mm.substr(0, c), mm.substr(c + 1)}); }
        else if (a == "-k") { std::string k = next(); size_t c = k.find(':'); keys[atoi(k.substr(0, c).c_str())] = (u32)strtoul(k.substr(c + 1).c_str(), nullptr, 0); }
        else args.push_back(a);
    }
    if (args.empty()) { fprintf(stderr, "usage: gp2xcli [options] GAME [ARGS]\n"); return 2; }
    Machine m(model);
    m.linux.strace = strace;
    if (iostat) m.io_stats = &stats;
    m.linux.mounts = m_mounts;
    if (!release.empty()) m.linux.uname_release = release;
    m.linux.on_log = [&](const std::string& s) { if (!quiet) fprintf(stderr, "[gp2x] %s\n", s.c_str()); };
    m.linux.on_output = [](const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); fflush(stdout); };
    std::string err;
    if (!m.start(root, args[0], mhz, &err, std::vector<std::string>(args.begin() + 1, args.end()))) { fprintf(stderr, "start failed: %s\n", err.c_str()); return 3; }
    std::vector<s16> audio;
    auto t0 = std::chrono::steady_clock::now();
    int f = 0;
    for (; f < frames && m.running(); f++) {
        auto it = keys.find(f);
        if (it != keys.end()) m.set_buttons(it->second);
        m.run_frame();
        s16 buf[4096];
        size_t n;
        while ((n = m.audio_read(buf, 2048)) > 0) if (!wav.empty()) audio.insert(audio.end(), buf, buf + n * 2);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!quiet)
        fprintf(stderr, "[gp2x] %d frames, %.1f M instructions in %.2fs (%.1f MIPS)%s\n", f, m.linux.total_insns() / 1e6, secs,
                m.linux.total_insns() / 1e6 / std::max(secs, 1e-6), m.linux.exited ? (" - " + m.linux.exit_reason).c_str() : "");
    if (iostat) {
        for (auto& s : stats) fprintf(stderr, "[io] %s 0x%05x x%llu\n", (s.first >> 31) ? "write" : "read ", s.first & 0x1FFFF, (unsigned long long)s.second);
        fprintf(stderr, "[io] final pc 0x%08x\n", m.linux.cpu.r[15]);
    }
    if (!out.empty()) save_ppm(out.c_str(), m.framebuffer(), Machine::W, Machine::H);
    if (!wav.empty()) save_wav(wav.c_str(), audio);
    return m.linux.exited ? m.linux.exit_code : 0;
}
