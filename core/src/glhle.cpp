// PocketPenguin - libopengles_lite.so HLE glue (see glhle.h).
#include "glhle.h"
#include "gles.h"
#include "gp2x.h"
#include <cstdio>
#include <vector>

namespace gp2x {

Hle::Hle(Machine* s, Linux* l) : cpu(&l->cpu), sys(s), linux(l) {
    gl = new GLES(this);
}

Hle::~Hle() { delete gl; }

bool Hle::read_mem(u32 va, void* dst, u32 len) { return linux->mem.read(va, dst, len); }
bool Hle::write_mem(u32 va, const void* src, u32 len) { return linux->mem.write(va, src, len); }
std::string Hle::read_str(u32 va, u32 max) { return linux->mem.read_str(va, max); }

u32 Hle::arg(int i) {
    if (i < 4) return cpu->r[i];
    return rd32(cpu->r[13] + (u32)(i - 4) * 4);
}

u32 Hle::scratch_alloc(u32 len) {
    len = (len + 7) & ~7u;
    if (!scratch_page || scratch_next + len > scratch_page + 0x10000) {
        if (len > 0x10000) return 0;
        scratch_page = linux->mem.find_free(0x10000, 0, 0x50000000);
        if (!scratch_page) return 0;
        linux->mem.map_anon(scratch_page, 0x10000);
        scratch_next = scratch_page;
    }
    u32 a = scratch_next;
    scratch_next += len;
    return a;
}

void Hle::wait_next_frame() {
    u64 fc = sys->frame_cycles ? sys->frame_cycles : 1;
    u64 next = (linux->now() / fc + 1) * fc;
    if (linux->cur) linux->block_retry(nullptr, next);
    cpu->request_stop();
}

bool Hle::hook(ARM926* c, u32 pc, void* user) {
    Hle* h = (Hle*)user;
    auto it = h->funcs.find(pc);
    if (it == h->funcs.end()) return false;
    (void)c;
    return h->gl->call(it->second);
}

void Hle::on_map(const std::string& host_path, u32 va, u32 len, u64 off) {
    size_t slash = host_path.rfind('/');
    std::string name = host_path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.rfind("libopengles_lite.so", 0) != 0 || off != 0) return;
    // Parse the library's dynamic symbol table and trap every gl*/egl* function we implement.
    FILE* f = fopen(host_path.c_str(), "rb");
    if (!f) return;
    std::vector<u8> d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t n = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (n != d.size() || d.size() < 52) return;
    auto r16 = [&](size_t o) { u16 v = 0; if (o + 2 <= d.size()) memcpy(&v, &d[o], 2); return v; };
    auto r32 = [&](size_t o) { u32 v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; };
    u32 shoff = r32(32);
    u16 shentsize = r16(46), shnum = r16(48);
    funcs.clear();
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < shnum; i++) {
        size_t sh = shoff + (size_t)i * shentsize;
        if (r32(sh + 4) != 11) continue;            // SHT_DYNSYM
        u32 symoff = r32(sh + 16), symsize = r32(sh + 20), link = r32(sh + 24), entsize = r32(sh + 36);
        size_t strsh = shoff + (size_t)link * shentsize;
        u32 stroff = r32(strsh + 16);
        if (!entsize) entsize = 16;
        for (u32 s = 0; s + entsize <= symsize; s += entsize) {
            u32 nameoff = r32(symoff + s), value = r32(symoff + s + 4);
            u8 info = symoff + s + 12 < d.size() ? d[symoff + s + 12] : 0;
            u16 shndx = r16(symoff + s + 14);
            if ((info & 15) != 2 || shndx == 0 || !value) continue; // functions defined here
            std::string sym((const char*)&d[std::min<size_t>(stroff + nameoff, d.size() - 1)]);
            int id = gl->find(sym.c_str());
            if (id < 0) continue;
            u32 addr = va + (value & ~1u);
            funcs[addr] = id;
            lo = std::min(lo, addr);
            hi = std::max(hi, addr + 4);
        }
    }
    if (funcs.empty()) return;
    lib_base = lo;
    lib_size = hi - lo;
    cpu->hle_hook = hook;
    cpu->hle_user = this;
    cpu->hle_base = lib_base;
    cpu->hle_size = lib_size;
    (void)len;
    if (log) {
        char b[160];
        snprintf(b, sizeof b, "OpenGL ES: %zu functions of libopengles_lite.so run by the built-in renderer", funcs.size());
        log(b);
    }
}

} // namespace gp2x
