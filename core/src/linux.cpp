// GP2XDroid - user-mode ARM Linux runtime (system call emulation).
#include "linux.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <random>

namespace gp2x {

// ---------------------------------------------------------------------------
// Guest constants (ARM Linux)
// ---------------------------------------------------------------------------
enum {
    G_EPERM = 1, G_ENOENT = 2, G_ESRCH = 3, G_EINTR = 4, G_EIO = 5, G_EBADF = 9, G_ECHILD = 10,
    G_EAGAIN = 11, G_ENOMEM = 12, G_EACCES = 13, G_EFAULT = 14, G_EEXIST = 17, G_ENOTDIR = 20,
    G_EISDIR = 21, G_EINVAL = 22, G_EMFILE = 24, G_ENOTTY = 25, G_ESPIPE = 29, G_EPIPE = 32,
    G_ERANGE = 34, G_ENOSYS = 38, G_ELOOP = 40, G_ETIMEDOUT = 110,
};
enum {
    GO_ACCMODE = 3, GO_CREAT = 0100, GO_EXCL = 0200, GO_NOCTTY = 0400, GO_TRUNC = 01000,
    GO_APPEND = 02000, GO_NONBLOCK = 04000, GO_DIRECTORY = 040000, GO_NOFOLLOW = 0100000,
    GO_CLOEXEC = 02000000,
};
static const u32 KUSER_PAGE = 0xFFFF0000;
static const u32 SIGRET_TRAMP = 0xFFFF0E00;     // mov r7,#119; swi 0
static const u32 RTSIGRET_TRAMP = 0xFFFF0E10;   // mov r7,#173; swi 0
static const u32 MAGIC_CMPXCHG64 = 0xFFF060;
static const u32 STACK_TOP = 0xBF000000, STACK_SIZE = 8 << 20;
static const u64 SLICE = 20000;
static const int FUTEX_TAG_V = 0;
static const void* FUTEX_TAG = &FUTEX_TAG_V;
static inline u64 SB(int sig) { return 1ull << (sig - 1); }

static int to_host_flags(int g) {
    int h = g & 3;
    if (g & GO_CREAT) h |= O_CREAT;
    if (g & GO_EXCL) h |= O_EXCL;
    if (g & GO_TRUNC) h |= O_TRUNC;
    if (g & GO_APPEND) h |= O_APPEND;
    if (g & GO_DIRECTORY) h |= O_DIRECTORY;
    return h;
}

static GuestStat from_host(const struct stat& s) {
    GuestStat g;
    g.dev = s.st_dev; g.ino = s.st_ino; g.mode = s.st_mode; g.nlink = s.st_nlink;
    g.size = s.st_size; g.blksize = 4096; g.blocks = (s.st_size + 511) / 512;
    g.atime = (u32)s.st_atime; g.mtime = (u32)s.st_mtime; g.ctime = (u32)s.st_ctime;
    g.rdev = s.st_rdev;
    return g;
}

// ---------------------------------------------------------------------------
// File defaults
// ---------------------------------------------------------------------------
s32 File::read(Linux&, u32, u32) { return -G_EINVAL; }
s32 File::write(Linux&, u32, u32 len) { return -G_EINVAL; (void)len; }
s32 File::ioctl(Linux&, u32, u32) { return -G_ENOTTY; }
s64 File::lseek(Linux&, s64, int) { return -G_ESPIPE; }
s32 File::pread(Linux&, u32, u32, s64) { return -G_ESPIPE; }
s32 File::pwrite(Linux&, u32, u32, s64) { return -G_ESPIPE; }
s32 File::mmap(Linux&, u32, u32, u32, u32, u64) { return -19; /* ENODEV */ }
s32 File::fstat(Linux&, GuestStat* st) { *st = GuestStat(); st->mode = 020666; return 0; }
s32 File::ftruncate(Linux&, s64) { return -G_EINVAL; }
s32 File::getdents(Linux&, u32, u32, bool) { return -G_ENOTDIR; }
int File::poll_mask(Linux&) { return 1 | 4; }

// Regular host file
class HostFile : public File {
public:
    int fd;
    std::string hpath;
    explicit HostFile(int f, const std::string& h) : fd(f), hpath(h) {}
    ~HostFile() override { if (fd >= 0) close(fd); }
    std::string host_path() const override { return hpath; }
    s32 read(Linux& L, u32 buf, u32 len) override {
        std::vector<u8> tmp(std::min<u32>(len, 1 << 20));
        u32 total = 0;
        while (total < len) {
            u32 n = std::min<u32>(len - total, (u32)tmp.size());
            ssize_t r = ::read(fd, tmp.data(), n);
            if (r < 0) return total ? (s32)total : -errno;
            if (r == 0) break;
            if (!L.mem.write(buf + total, tmp.data(), (u32)r)) return -G_EFAULT;
            total += (u32)r;
            if ((u32)r < n) break;
        }
        return (s32)total;
    }
    s32 write(Linux& L, u32 buf, u32 len) override {
        std::vector<u8> tmp(len);
        if (!L.mem.read(buf, tmp.data(), len)) return -G_EFAULT;
        ssize_t r = ::write(fd, tmp.data(), len);
        return r < 0 ? -errno : (s32)r;
    }
    s64 lseek(Linux&, s64 off, int whence) override {
        off_t r = ::lseek(fd, (off_t)off, whence);
        return r < 0 ? -errno : (s64)r;
    }
    s32 pread(Linux& L, u32 buf, u32 len, s64 off) override {
        std::vector<u8> tmp(len);
        ssize_t r = ::pread(fd, tmp.data(), len, (off_t)off);
        if (r < 0) return -errno;
        if (!L.mem.write(buf, tmp.data(), (u32)r)) return -G_EFAULT;
        return (s32)r;
    }
    s32 pwrite(Linux& L, u32 buf, u32 len, s64 off) override {
        std::vector<u8> tmp(len);
        if (!L.mem.read(buf, tmp.data(), len)) return -G_EFAULT;
        ssize_t r = ::pwrite(fd, tmp.data(), len, (off_t)off);
        return r < 0 ? -errno : (s32)r;
    }
    s32 mmap(Linux& L, u32 va, u32 len, u32, u32, u64 off) override {
        L.mem.map_anon(va, len);
        std::vector<u8> tmp(len);
        ssize_t r = ::pread(fd, tmp.data(), len, (off_t)off);
        if (r > 0) L.mem.write(va, tmp.data(), (u32)r);
        return 0;
    }
    s32 fstat(Linux&, GuestStat* st) override {
        struct stat s;
        if (::fstat(fd, &s) < 0) return -errno;
        *st = from_host(s);
        return 0;
    }
    s32 ftruncate(Linux&, s64 len) override { return ::ftruncate(fd, (off_t)len) < 0 ? -errno : 0; }
};

// Directory
class HostDir : public File {
public:
    std::string hpath;
    std::vector<std::pair<std::string, u8>> ents;
    size_t pos = 0;
    bool loaded = false;
    explicit HostDir(const std::string& h) : hpath(h) {}
    bool is_dir() const override { return true; }
    std::string host_path() const override { return hpath; }
    void load() {
        if (loaded) return;
        loaded = true;
        DIR* d = opendir(hpath.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            u8 t = 0;
            if (e->d_type == DT_DIR) t = 4; else if (e->d_type == DT_REG) t = 8; else if (e->d_type == DT_LNK) t = 10;
            ents.push_back({e->d_name, t});
        }
        closedir(d);
    }
    s32 getdents(Linux& L, u32 buf, u32 len, bool is64) override {
        load();
        u32 off = 0;
        while (pos < ents.size()) {
            const std::string& n = ents[pos].first;
            u32 reclen;
            std::vector<u8> rec;
            if (is64) {
                reclen = (19 + (u32)n.size() + 1 + 7) & ~7u;
                rec.assign(reclen, 0);
                u64 ino = pos + 1; s64 doff = (s64)pos + 1;
                memcpy(&rec[0], &ino, 8); memcpy(&rec[8], &doff, 8);
                u16 rl = (u16)reclen; memcpy(&rec[16], &rl, 2);
                rec[18] = ents[pos].second;
                memcpy(&rec[19], n.data(), n.size());
            } else {
                reclen = (10 + (u32)n.size() + 2 + 3) & ~3u;
                rec.assign(reclen, 0);
                u32 ino = (u32)pos + 1, doff = (u32)pos + 1;
                memcpy(&rec[0], &ino, 4); memcpy(&rec[4], &doff, 4);
                u16 rl = (u16)reclen; memcpy(&rec[8], &rl, 2);
                memcpy(&rec[10], n.data(), n.size());
                rec[reclen - 1] = ents[pos].second;
            }
            if (off + reclen > len) { if (off == 0) return -G_EINVAL; break; }
            L.mem.write(buf + off, rec.data(), reclen);
            off += reclen;
            pos++;
        }
        return (s32)off;
    }
    s64 lseek(Linux&, s64 off, int whence) override {
        if (whence == 0 && off >= 0) { pos = (size_t)off; return off; }
        return -G_EINVAL;
    }
    s32 fstat(Linux&, GuestStat* st) override {
        struct stat s;
        if (::stat(hpath.c_str(), &s) < 0) return -errno;
        *st = from_host(s);
        return 0;
    }
};

// In-memory read-only file (/proc entries)
class MemFile : public File {
public:
    std::string data;
    size_t pos = 0;
    explicit MemFile(const std::string& d) : data(d) {}
    s32 read(Linux& L, u32 buf, u32 len) override {
        u32 n = (u32)std::min<size_t>(len, data.size() - std::min(pos, data.size()));
        L.mem.write(buf, data.data() + pos, n);
        pos += n;
        return (s32)n;
    }
    s64 lseek(Linux&, s64 off, int whence) override {
        s64 p = whence == 0 ? off : whence == 1 ? (s64)pos + off : (s64)data.size() + off;
        if (p < 0) return -G_EINVAL;
        pos = (size_t)p;
        return p;
    }
    s32 fstat(Linux&, GuestStat* st) override { *st = GuestStat(); st->mode = 0100444; st->size = 0; return 0; }
};

// /dev/null, /dev/zero, /dev/urandom
class SpecialFile : public File {
public:
    int kind; // 0 null, 1 zero, 2 random
    explicit SpecialFile(int k) : kind(k) {}
    s32 read(Linux& L, u32 buf, u32 len) override {
        if (kind == 0) return 0;
        std::vector<u8> t(len, 0);
        if (kind == 2) { static std::mt19937 rng(1234); for (auto& b : t) b = (u8)rng(); }
        L.mem.write(buf, t.data(), len);
        return (s32)len;
    }
    s32 write(Linux&, u32, u32 len) override { return (s32)len; }
    s64 lseek(Linux&, s64, int) override { return 0; }
    s32 mmap(Linux& L, u32 va, u32 len, u32, u32, u64) override { L.mem.map_anon(va, len); return 0; }
};

// Terminal: stdin/stdout/stderr and /dev/tty*
class TtyFile : public File {
public:
    s32 read(Linux&, u32, u32) override { return 0; }
    s32 write(Linux& L, u32 buf, u32 len) override {
        std::string s(len, '\0');
        L.mem.read(buf, &s[0], len);
        if (L.on_output) L.on_output(s);
        return (s32)len;
    }
    s32 ioctl(Linux& L, u32 req, u32 arg) override {
        if (req == 0x5401) { L.mem.zero(arg, 36); return 0; }                // TCGETS
        if (req == 0x5413) { u16 ws[4] = {25, 80, 0, 0}; L.mem.write(arg, ws, 8); return 0; } // TIOCGWINSZ
        if (req == 0x5603) { u16 st[3] = {1, 0, 2}; L.mem.write(arg, st, 6); return 0; } // VT_GETSTATE
        if (req == 0x5600) { L.mem.w32(arg, 1); return 0; }                   // VT_OPENQRY
        if (req == 0x4B3B) { L.mem.w32(arg, 0); return 0; }                   // KDGETMODE
        return 0;
    }
    s32 fstat(Linux&, GuestStat* st) override { *st = GuestStat(); st->mode = 020620; st->rdev = 0x0400; return 0; }
};

// Pipe
struct PipeBuf { std::deque<u8> data; int readers = 0, writers = 0; };
class PipeEnd : public File {
public:
    std::shared_ptr<PipeBuf> p;
    bool wr;
    PipeEnd(std::shared_ptr<PipeBuf> b, bool w) : p(b), wr(w) { if (w) p->writers++; else p->readers++; }
    ~PipeEnd() override { if (wr) p->writers--; else p->readers--; }
    s32 read(Linux& L, u32 buf, u32 len) override {
        if (wr) return -G_EBADF;
        if (p->data.empty()) {
            if (p->writers == 0) return 0;
            return L.block_retry(p.get());
        }
        u32 n = (u32)std::min<size_t>(len, p->data.size());
        std::vector<u8> t(p->data.begin(), p->data.begin() + n);
        p->data.erase(p->data.begin(), p->data.begin() + n);
        L.mem.write(buf, t.data(), n);
        L.wake(p.get());
        return (s32)n;
    }
    s32 write(Linux& L, u32 buf, u32 len) override {
        if (!wr) return -G_EBADF;
        if (p->readers == 0) return -G_EPIPE;
        std::vector<u8> t(len);
        L.mem.read(buf, t.data(), len);
        p->data.insert(p->data.end(), t.begin(), t.end());
        L.wake(p.get());
        return (s32)len;
    }
    int poll_mask(Linux&) override {
        if (wr) return 4;
        return (!p->data.empty() || p->writers == 0) ? 1 : 0;
    }
    s32 fstat(Linux&, GuestStat* st) override { *st = GuestStat(); st->mode = 010600; return 0; }
};

// ---------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------
GuestMem::GuestMem() : host(1u << 20, nullptr), kind(1u << 20, NONE), phys(1u << 20, 0) {}

GuestMem::~GuestMem() {
    for (u32 p = 0; p < (1u << 20); p++) if (kind[p] == ANON) free(host[p]);
    for (u8* p : free_pages) free(p);
}

u8* GuestMem::alloc_page() {
    if (!free_pages.empty()) {
        u8* p = free_pages.back();
        free_pages.pop_back();
        memset(p, 0, 4096);
        return p;
    }
    return (u8*)calloc(1, 4096);
}

void GuestMem::release(u32 page) {
    if (kind[page] == ANON) {
        if (free_pages.size() < 4096) free_pages.push_back(host[page]); else free(host[page]);
    }
    host[page] = nullptr;
    kind[page] = NONE;
    phys[page] = 0;
}

void GuestMem::flush() { if (cpu) cpu->flush_tlb(); }

void GuestMem::segv(u32 a) {
    if (!fault) { fault = true; fault_addr = a; }
    if (cpu) cpu->request_stop();
}

u32 GuestMem::read32(u32 a) {
    u32 p = a >> 12;
    if (host[p]) { u32 v; memcpy(&v, host[p] + (a & 0xFFC), 4); return v; }
    if (kind[p] == IO) return hw->io_read(phys[p] | (a & 0xFFC), 4);
    segv(a);
    return 0;
}
u16 GuestMem::read16(u32 a) {
    u32 p = a >> 12;
    if (host[p]) { u16 v; memcpy(&v, host[p] + (a & 0xFFE), 2); return v; }
    if (kind[p] == IO) return (u16)hw->io_read(phys[p] | (a & 0xFFE), 2);
    segv(a);
    return 0;
}
u8 GuestMem::read8(u32 a) {
    u32 p = a >> 12;
    if (host[p]) return host[p][a & 0xFFF];
    if (kind[p] == IO) return (u8)hw->io_read(phys[p] | (a & 0xFFF), 1);
    segv(a);
    return 0;
}
void GuestMem::write32(u32 a, u32 v) {
    u32 p = a >> 12;
    if (host[p]) { memcpy(host[p] + (a & 0xFFC), &v, 4); return; }
    if (kind[p] == IO) { hw->io_write(phys[p] | (a & 0xFFC), v, 4); return; }
    segv(a);
}
void GuestMem::write16(u32 a, u16 v) {
    u32 p = a >> 12;
    if (host[p]) { memcpy(host[p] + (a & 0xFFE), &v, 2); return; }
    if (kind[p] == IO) { hw->io_write(phys[p] | (a & 0xFFE), v, 2); return; }
    segv(a);
}
void GuestMem::write8(u32 a, u8 v) {
    u32 p = a >> 12;
    if (host[p]) { host[p][a & 0xFFF] = v; return; }
    if (kind[p] == IO) { hw->io_write(phys[p] | (a & 0xFFF), v, 1); return; }
    segv(a);
}

bool GuestMem::range_free(u32 addr, u32 len) const {
    u64 end = (u64)addr + len;
    if (end > 0x100000000ull) return false;
    for (u64 a = addr & ~0xFFFu; a < end; a += 4096) if (kind[a >> 12] != NONE) return false;
    return true;
}

u32 GuestMem::find_free(u32 len, u32 hint, u32 lo, u32 hi) const {
    len = (len + 0xFFF) & ~0xFFFu;
    if (hint && !(hint & 0xFFF) && range_free(hint, len)) return hint;
    u32 run = 0, start = lo;
    for (u32 a = lo; a < hi; a += 4096) {
        if (kind[a >> 12] == NONE) {
            if (run == 0) start = a;
            run += 4096;
            if (run >= len) return start;
        } else run = 0;
    }
    return 0;
}

void GuestMem::map_anon(u32 addr, u32 len) {
    u64 end = (u64)addr + len;
    for (u64 a = addr & ~0xFFFu; a < end; a += 4096) {
        u32 p = (u32)(a >> 12);
        if (kind[p] == ANON) continue;
        release(p);
        host[p] = alloc_page();
        kind[p] = ANON;
    }
    flush();
}

void GuestMem::map_phys(u32 addr, u32 len, u32 pa) {
    u64 end = (u64)addr + len;
    pa &= ~0xFFFu;
    for (u64 a = addr & ~0xFFFu; a < end; a += 4096, pa += 4096) {
        u32 p = (u32)(a >> 12);
        release(p);
        if (hw->is_io(pa)) { kind[p] = IO; phys[p] = pa; }
        else if (u8* h = hw->ram_page(pa)) { kind[p] = PHYS; host[p] = h; phys[p] = pa; }
        else { host[p] = alloc_page(); kind[p] = ANON; }
    }
    flush();
}

void GuestMem::unmap(u32 addr, u32 len) {
    u64 end = (u64)addr + len;
    for (u64 a = addr & ~0xFFFu; a < end; a += 4096) release((u32)(a >> 12));
    flush();
}

bool GuestMem::move(u32 from, u32 to, u32 len) {
    u32 n = (len + 0xFFF) >> 12;
    for (u32 i = 0; i < n; i++) {
        u32 s = (from >> 12) + i, d = (to >> 12) + i;
        host[d] = host[s]; kind[d] = kind[s]; phys[d] = phys[s];
        host[s] = nullptr; kind[s] = NONE; phys[s] = 0;
    }
    flush();
    return true;
}

bool GuestMem::read(u32 va, void* dst, u32 len) {
    u8* d = (u8*)dst;
    while (len) {
        u32 p = va >> 12, off = va & 0xFFF, n = std::min<u32>(len, 4096 - off);
        if (host[p]) memcpy(d, host[p] + off, n);
        else if (kind[p] == IO) { for (u32 i = 0; i < n; i++) d[i] = (u8)hw->io_read(phys[p] | (off + i), 1); }
        else return false;
        va += n; d += n; len -= n;
    }
    return true;
}

bool GuestMem::write(u32 va, const void* src, u32 len) {
    const u8* s = (const u8*)src;
    while (len) {
        u32 p = va >> 12, off = va & 0xFFF, n = std::min<u32>(len, 4096 - off);
        if (host[p]) memcpy(host[p] + off, s, n);
        else if (kind[p] == IO) { for (u32 i = 0; i < n; i++) hw->io_write(phys[p] | (off + i), s[i], 1); }
        else return false;
        va += n; s += n; len -= n;
    }
    return true;
}

bool GuestMem::zero(u32 va, u32 len) {
    static const u8 z[4096] = {0};
    while (len) {
        u32 n = std::min<u32>(len, 4096 - (va & 0xFFF));
        if (!write(va, z, n)) return false;
        va += n; len -= n;
    }
    return true;
}

std::string GuestMem::read_str(u32 va, u32 max) {
    std::string s;
    while (s.size() < max) {
        u32 p = va >> 12;
        if (!host[p]) break;
        char c = (char)host[p][va & 0xFFF];
        if (!c) break;
        s += c;
        va++;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------
Linux::Linux(Hardware* h) : cpu(&mem), hw(h) {
    mem.hw = h;
    mem.cpu = &cpu;
    cpu.trace_user = this;
    cpu.swi_hook = swi_entry;
    cpu.undef_hook = undef_entry;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    epoch_usec = (u64)tv.tv_sec * 1000000ull + tv.tv_usec;
}

Linux::~Linux() {}

void Linux::log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (on_log) on_log(buf);
}

u64 Linux::now() const {
    return in_slice ? base_time + (cpu.insn_count - slice_cycles0) + pending_skip : base_time;
}

void Linux::skip_to(u64 t) {
    u64 n = now();
    if (t <= n) return;
    if (in_slice) { pending_skip += t - n; cpu.request_stop(); }
    else base_time = t;
}

int Linux::thread_count() const {
    int n = 0;
    for (auto& t : threads) if (t->state != Thread::DEAD) n++;
    return n;
}

s32 Linux::block_retry(const void* obj, u64 until) {
    cur->state = Thread::BLOCKED;
    cur->wait_obj = obj;
    cur->wake_at = until;
    cur->restart = true;
    return SYS_BLOCK;
}

s32 Linux::block_wait(const void* obj, u64 until, s32 timeout_result) {
    cur->state = Thread::BLOCKED;
    cur->wait_obj = obj;
    cur->wake_at = until;
    cur->restart = false;
    cur->timeout_result = timeout_result;
    return SYS_BLOCK;
}

void Linux::unblock(Thread* t, bool timeout) {
    t->state = Thread::RUN;
    t->wait_obj = nullptr;
    t->wake_at = ~0ull;
    if (!t->restart) t->r[0] = timeout ? (u32)t->timeout_result : 0;
}

void Linux::wake(const void* obj) {
    for (auto& t : threads)
        if (t->state == Thread::BLOCKED && t->wait_obj == obj && obj != FUTEX_TAG) unblock(t.get(), false);
}

void Linux::queue_signal(int tid, int sig) {
    if (sig <= 0 || sig > 64) return;
    if (tid) {
        for (auto& t : threads) if (t->tid == tid && t->state != Thread::DEAD) { t->pending |= SB(sig); return; }
        return;
    }
    Thread* target = nullptr;
    for (auto& t : threads) if (t->state != Thread::DEAD && !(t->sigmask & SB(sig))) { target = t.get(); break; }
    if (!target) for (auto& t : threads) if (t->state != Thread::DEAD) { target = t.get(); break; }
    if (target) target->pending |= SB(sig);
}

void Linux::kill_process(const std::string& why, int code) {
    if (exited) return;
    exited = true;
    exit_code = code;
    exit_reason = why;
    for (auto& t : threads) t->state = Thread::DEAD;
    cpu.request_stop();
    log("%s", why.c_str());
}

void Linux::thread_exit(Thread* t, int code) {
    t->state = Thread::DEAD;
    if (t->clear_tid) {
        mem.w32(t->clear_tid, 0);
        do_futex(t->clear_tid, 1, 1, 0, 0, 0, false);
    }
    if (thread_count() == 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "Program exited with code %d", code);
        kill_process(buf, code);
    }
    cpu.request_stop();
}

void Linux::load_ctx(Thread* t) {
    memcpy(cpu.r, t->r, sizeof t->r);
    cpu.cpsr = t->cpsr;
    mem.w32(0xFFFF0FF0, t->tls);
}

void Linux::save_ctx(Thread* t) {
    memcpy(t->r, cpu.r, sizeof t->r);
    t->cpsr = cpu.cpsr;
}

Thread* Linux::pick() {
    u64 t = now();
    for (auto& th : threads) {
        if (th->state != Thread::BLOCKED) continue;
        if (th->wake_at <= t) { unblock(th.get(), true); continue; }
        u64 deliverable = th->pending & ~(th->sigmask & ~SB(9));
        if (deliverable) {
            // Interrupt the system call if any deliverable signal has a handler or kills.
            bool act = false;
            for (int s = 1; s <= 64; s++) if ((deliverable & SB(s)) && sigact[s].handler != 1) { act = true; break; }
            if (act) {
                if (th->restart) { th->r[15] += (th->cpsr & FLAG_T) ? 2 : 4; th->r[0] = (u32)-G_EINTR; th->restart = false; th->timeout_result = -G_EINTR; }
                th->timeout_result = -G_EINTR;
                unblock(th.get(), true);
            }
        }
    }
    size_t n = threads.size();
    for (size_t i = 0; i < n; i++) {
        size_t idx = (rr + i) % n;
        if (threads[idx]->state == Thread::RUN) { rr = idx + 1; return threads[idx].get(); }
    }
    return nullptr;
}

void Linux::deliver_signals(Thread* t) {
    u64 deliverable = t->pending & ~(t->sigmask & ~SB(9));
    if (!deliverable) return;
    for (int sig = 1; sig <= 64; sig++) {
        if (!(deliverable & SB(sig))) continue;
        t->pending &= ~SB(sig);
        SigAction& a = sigact[sig];
        if (sig == 9 || a.handler == 0) {
            if (sig == 17 || sig == 18 || sig == 23 || sig == 28 || (sig >= 19 && sig <= 22)) continue;
            char buf[64];
            snprintf(buf, sizeof buf, "Program killed by signal %d", sig);
            kill_process(buf, 128 + sig);
            return;
        }
        if (a.handler == 1) continue;
        Thread::Frame f;
        memcpy(f.r, cpu.r, sizeof f.r);
        f.cpsr = cpu.cpsr;
        f.mask = t->sigsuspend ? t->suspend_saved_mask : t->sigmask;
        t->sigsuspend = false;
        if (t->frames.size() > 64) t->frames.erase(t->frames.begin());
        t->frames.push_back(f);
        u32 sp = (cpu.r[13] - 512) & ~7u;
        mem.zero(sp, 512);
        mem.w32(sp, (u32)sig);                 // siginfo.si_signo
        // ucontext at sp+128: flags, link, stack_t(3), sigcontext(21 words), sigmask
        u32 sc = sp + 128 + 20;
        mem.w32(sc + 8, (u32)f.mask);          // oldmask
        for (int i = 0; i < 16; i++) mem.w32(sc + 12 + i * 4, f.r[i]);
        mem.w32(sc + 76, f.cpsr);
        mem.write(sc + 84, &f.mask, 8);        // uc_sigmask
        cpu.r[0] = (u32)sig;
        cpu.r[1] = sp;
        cpu.r[2] = sp + 128;
        cpu.r[13] = sp;
        cpu.r[14] = (a.flags & 0x04000000) && a.restorer ? a.restorer : ((a.flags & 4) ? RTSIGRET_TRAMP : SIGRET_TRAMP);
        if (a.handler & 1) { cpu.cpsr |= FLAG_T; cpu.r[15] = a.handler & ~1u; }
        else { cpu.cpsr &= ~FLAG_T; cpu.r[15] = a.handler & ~3u; }
        t->sigmask |= a.mask;
        if (!(a.flags & 0x40000000)) t->sigmask |= SB(sig);
        if (a.flags & 0x80000000u) a.handler = 0;
        return;
    }
}

void Linux::run_until(u64 target) {
    int idle_guard = 0;
    while (!exited) {
        u64 t = now();
        if (t >= target) break;
        // interval timer
        Thread* th = pick();
        if (!th) {
            u64 w = ~0ull;
            for (auto& x : threads) if (x->state == Thread::BLOCKED) w = std::min(w, x->wake_at);
            if (w >= target) { base_time = target; break; }
            base_time = std::max(base_time, w);
            if (++idle_guard > 100000) break;
            continue;
        }
        cur = th;
        load_ctx(th);
        deliver_signals(th);
        if (exited) break;
        u64 budget = std::min<u64>(target - t, SLICE);
        in_slice = true;
        slice_cycles0 = cpu.insn_count;
        pending_skip = 0;
        cpu.run((s64)budget);
        // Never stop inside a kuser helper (they must look atomic).
        int guard = 0;
        while (cpu.r[15] >= 0xFFFF0F00 && cpu.r[15] < 0xFFFF1000 && th->state == Thread::RUN &&
               !mem.fault && !exited && guard++ < 64)
            cpu.run(1);
        base_time += (cpu.insn_count - slice_cycles0) + pending_skip;
        pending_skip = 0;
        in_slice = false;
        save_ctx(th);
        if (mem.fault && !exited) {
            mem.fault = false;
            char buf[160];
            snprintf(buf, sizeof buf, "Segmentation fault: address 0x%08x (pc 0x%08x, lr 0x%08x)",
                     mem.fault_addr, cpu.current_pc(), cpu.r[14]);
            if (sigact[11].handler > 1) { log("%s", buf); th->pending |= SB(11); }
            else kill_process(buf, 139);
        }
        if (pending_exec) {
            pending_exec = false;
            std::string err;
            std::string p = exec_path;
            std::vector<std::string> av = exec_argv;
            reset_process();
            if (!load(p, av, &err)) kill_process("exec failed: " + err, 127);
        }
    }
    cur = nullptr;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
static std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> out;
    std::string c;
    for (char ch : p) {
        if (ch == '/') { if (!c.empty()) out.push_back(c); c.clear(); }
        else c += ch;
    }
    if (!c.empty()) out.push_back(c);
    return out;
}

std::string Linux::abs_path(const std::string& g) const {
    std::string full = (!g.empty() && g[0] == '/') ? g : cwd + "/" + g;
    std::vector<std::string> st;
    for (auto& c : split_path(full)) {
        if (c == ".") continue;
        if (c == "..") { if (!st.empty()) st.pop_back(); continue; }
        st.push_back(c);
    }
    std::string r;
    for (auto& c : st) r += "/" + c;
    return r.empty() ? "/" : r;
}

static bool ci_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (tolower((u8)a[i]) != tolower((u8)b[i])) return false;
    return true;
}

std::string Linux::to_host(const std::string& guest, bool follow, bool* found) {
    std::deque<std::string> rem;
    for (auto& c : split_path(abs_path(guest))) rem.push_back(c);
    std::vector<std::string> done;
    int links = 0;
    auto host_of = [&](const std::vector<std::string>& d) {
        std::string g;
        for (auto& c : d) g += "/" + c;
        size_t best = 0;
        std::string h = root + g;
        for (auto& m : mounts) {
            const std::string& p = m.first;
            if (p.size() > best && (g == p || (g.size() > p.size() && g.compare(0, p.size(), p) == 0 && g[p.size()] == '/'))) {
                best = p.size();
                h = m.second + g.substr(p.size());
            }
        }
        return h;
    };
    if (found) *found = true;
    while (!rem.empty()) {
        std::string c = rem.front();
        rem.pop_front();
        if (c == ".") continue;
        if (c == "..") { if (!done.empty()) done.pop_back(); continue; }
        std::string dir = host_of(done);
        done.push_back(c);
        std::string cand = host_of(done);
        done.pop_back();
        struct stat s;
        if (lstat(cand.c_str(), &s) != 0) {
            // A parent of a mount point (e.g. /mnt for /mnt/sd) exists even if the root lacks it.
            std::string g;
            for (auto& x : done) g += "/" + x;
            g += "/" + c;
            bool parent_of_mount = false;
            for (auto& m : mounts) if (m.first.size() > g.size() && m.first.compare(0, g.size(), g) == 0 && m.first[g.size()] == '/') parent_of_mount = true;
            if (parent_of_mount) { done.push_back(c); continue; }
            // FAT (SD card) is case-insensitive: look for a match ignoring case.
            bool ok = false;
            if (DIR* d = opendir(dir.c_str())) {
                while (dirent* e = readdir(d)) {
                    if (ci_equal(e->d_name, c)) { c = e->d_name; ok = true; break; }
                }
                closedir(d);
            }
            if (!ok) {
                if (found) *found = false;
                std::string r = cand;
                for (auto& x : rem) r += "/" + x;
                return r;
            }
            done.push_back(c);
            cand = host_of(done);
            done.pop_back();
            if (lstat(cand.c_str(), &s) != 0) { if (found) *found = false; return cand; }
        }
        if (S_ISLNK(s.st_mode) && (follow || !rem.empty())) {
            if (++links > 40) { if (found) *found = false; return cand; }
            char buf[4096];
            ssize_t n = readlink(cand.c_str(), buf, sizeof buf - 1);
            if (n <= 0) { done.push_back(c); continue; }
            buf[n] = 0;
            std::string target(buf);
            if (target[0] == '/') done.clear();
            auto parts = split_path(target);
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) rem.push_front(*it);
            continue;
        }
        done.push_back(c);
    }
    return host_of(done);
}

// ---------------------------------------------------------------------------
// File descriptors
// ---------------------------------------------------------------------------
int Linux::alloc_fd(int min) {
    for (int i = min; i < 1024; i++) if (!fds.count(i)) return i;
    return -1;
}

std::shared_ptr<File> Linux::getf(int fd) {
    auto it = fds.find(fd);
    return it == fds.end() ? nullptr : it->second;
}

s32 Linux::do_open(const std::string& guest, int flags, int mode) {
    std::string p = abs_path(guest);
    std::shared_ptr<File> f;
    if (p == "/dev/null") f = std::make_shared<SpecialFile>(0);
    else if (p == "/dev/zero") f = std::make_shared<SpecialFile>(1);
    else if (p == "/dev/urandom" || p == "/dev/random") f = std::make_shared<SpecialFile>(2);
    else if (p == "/dev/tty" || p == "/dev/console" || p.rfind("/dev/tty", 0) == 0 || p.rfind("/dev/vc/", 0) == 0)
        f = std::make_shared<TtyFile>();
    else if (p == "/proc/cpuinfo") f = std::make_shared<MemFile>(hw->cpuinfo());
    else if (p == "/proc/meminfo") f = std::make_shared<MemFile>("MemTotal:        30000 kB\nMemFree:         20000 kB\nBuffers:             0 kB\nCached:              0 kB\n");
    else if (p == "/proc/self/cmdline") f = std::make_shared<MemFile>(exe_path + std::string(1, '\0'));
    else if (p.rfind("/dev/", 0) == 0 || p.rfind("/proc/", 0) == 0 || p.rfind("/sys/", 0) == 0) {
        int err = 0;
        f = hw->open_device(*this, p, flags, &err);
        if (!f && err) return -err;
    }
    if (!f) {
        bool found;
        std::string h = to_host(p, !(flags & GO_NOFOLLOW), &found);
        struct stat s;
        if (found && ::stat(h.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
            if ((flags & GO_ACCMODE) != 0) return -G_EISDIR;
            f = std::make_shared<HostDir>(h);
        } else {
            if (flags & GO_DIRECTORY) return found ? -G_ENOTDIR : -G_ENOENT;
            int hfd = ::open(h.c_str(), to_host_flags(flags), mode & ~umask_v);
            if (hfd < 0) return -errno;
            f = std::make_shared<HostFile>(hfd, h);
        }
    }
    f->path = p;
    f->flags = flags;
    int fd = alloc_fd();
    if (fd < 0) return -G_EMFILE;
    fds[fd] = f;
    if (flags & GO_CLOEXEC) fd_cloexec[fd] = 1;
    return fd;
}

s32 Linux::do_stat(const std::string& guest, bool follow, GuestStat* st) {
    std::string p = abs_path(guest);
    if (p.rfind("/dev/", 0) == 0 || p.rfind("/proc/", 0) == 0) {
        s32 fd = do_open(p, 0, 0);
        if (fd >= 0) {
            s32 r = fds[fd]->fstat(*this, st);
            fds.erase(fd);
            return r;
        }
    }
    bool found;
    std::string h = to_host(p, follow, &found);
    struct stat s;
    if ((follow ? ::stat(h.c_str(), &s) : ::lstat(h.c_str(), &s)) != 0) return -errno;
    *st = from_host(s);
    return 0;
}

void Linux::put_stat(u32 a, const GuestStat& s, int kind) {
    u8 b[104];
    memset(b, 0, sizeof b);
    auto w32 = [&](int o, u32 v) { memcpy(b + o, &v, 4); };
    auto w64 = [&](int o, u64 v) { memcpy(b + o, &v, 8); };
    auto w16 = [&](int o, u16 v) { memcpy(b + o, &v, 2); };
    if (kind == 0) { // struct stat
        w32(0, (u32)s.dev); w32(4, (u32)s.ino); w16(8, (u16)s.mode); w16(10, (u16)s.nlink);
        w16(12, (u16)s.uid); w16(14, (u16)s.gid); w32(16, (u32)s.rdev); w32(20, (u32)s.size);
        w32(24, s.blksize); w32(28, (u32)s.blocks); w32(32, s.atime); w32(40, s.mtime); w32(48, s.ctime);
        mem.write(a, b, 64);
    } else if (kind == 1) { // EABI stat64
        w64(0, s.dev); w32(12, (u32)s.ino); w32(16, s.mode); w32(20, s.nlink); w32(24, s.uid); w32(28, s.gid);
        w64(32, s.rdev); w64(48, (u64)s.size); w32(56, s.blksize); w64(64, s.blocks);
        w32(72, s.atime); w32(80, s.mtime); w32(88, s.ctime); w64(96, s.ino);
        mem.write(a, b, 104);
    } else { // OABI stat64 (packed)
        w64(0, s.dev); w32(12, (u32)s.ino); w32(16, s.mode); w32(20, s.nlink); w32(24, s.uid); w32(28, s.gid);
        w64(32, s.rdev); w64(44, (u64)s.size); w32(52, s.blksize); w64(56, s.blocks);
        w32(64, s.atime); w32(72, s.mtime); w32(80, s.ctime); w64(88, s.ino);
        mem.write(a, b, 96);
    }
}

// ---------------------------------------------------------------------------
// Program loading
// ---------------------------------------------------------------------------
static bool read_file(const std::string& h, std::vector<u8>* out) {
    FILE* f = fopen(h.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? (size_t)n : 0);
    size_t r = n > 0 ? fread(out->data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return r == out->size();
}

bool Linux::load_elf(const std::string& guest_path, u32* entry, u32* base, u32* phdr_out, u32* phnum_out,
                     std::string* interp, bool is_interp, std::string* err) {
    bool found;
    std::string h = to_host(guest_path, true, &found);
    std::vector<u8> d;
    if (!found || !read_file(h, &d)) { *err = "cannot read " + guest_path; return false; }
    if (d.size() < 52 || memcmp(d.data(), "\x7f" "ELF", 4) != 0) { *err = guest_path + " is not an ELF program"; return false; }
    if (d[4] != 1 || d[5] != 1) { *err = guest_path + ": not a 32-bit little-endian ELF"; return false; }
    auto r16 = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
    auto r32 = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    if (r16(18) != 40) { *err = guest_path + ": not an ARM program"; return false; }
    u16 type = r16(16);
    u32 e_entry = r32(24), phoff = r32(28);
    u16 phentsize = r16(42), phnum = r16(44);
    u32 eflags = r32(36);
    if (!is_interp) {
        u32 ver = eflags >> 24;
        log("Loading %s (%s, %s)", guest_path.c_str(), type == 3 ? "ET_DYN" : "ET_EXEC",
            ver ? "EABI" : "old ABI");
    }
    if ((size_t)phoff + (size_t)phnum * phentsize > d.size()) { *err = "bad program headers"; return false; }
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < phnum; i++) {
        size_t o = phoff + i * phentsize;
        if (r32(o) != 1) continue;
        lo = std::min(lo, r32(o + 8) & ~0xFFFu);
        hi = std::max(hi, r32(o + 8) + r32(o + 20));
    }
    if (lo > hi) { *err = "no loadable segments"; return false; }
    u32 bias = 0;
    if (type == 3) {
        u32 span = ((hi - lo) + 0xFFF) & ~0xFFFu;
        u32 at = mem.find_free(span, 0, is_interp ? 0x40000000 : 0x00010000);
        if (!at) { *err = "no room for program"; return false; }
        bias = at - lo;
    }
    u32 phdr = 0;
    u32 maxend = 0;
    for (int i = 0; i < phnum; i++) {
        size_t o = phoff + i * phentsize;
        u32 pt = r32(o), off = r32(o + 4), va = r32(o + 8), fsz = r32(o + 16), msz = r32(o + 20);
        if (pt == 3 && interp) {
            std::string s((const char*)&d[off], fsz);
            *interp = s.c_str();
        } else if (pt == 6) phdr = va + bias;
        else if (pt == 1) {
            u32 start = (va + bias) & ~0xFFFu, end = (va + bias + msz + 0xFFF) & ~0xFFFu;
            mem.map_anon(start, end - start);
            if (off + fsz > d.size()) { *err = "truncated program"; return false; }
            mem.write(va + bias, &d[off], fsz);
            if (msz > fsz) mem.zero(va + bias + fsz, msz - fsz);
            if (!phdr && off <= phoff && phoff < off + fsz) phdr = va + bias + (phoff - off);
            maxend = std::max(maxend, va + bias + msz);
        }
    }
    *entry = e_entry + bias;
    *base = bias;
    *phdr_out = phdr;
    *phnum_out = phnum;
    if (!is_interp) { brk_start = brk_cur = (maxend + 0xFFF) & ~0xFFFu; }
    return true;
}

void Linux::setup_kuser_page() {
    mem.map_anon(KUSER_PAGE, 4096);
    auto put = [&](u32 a, std::initializer_list<u32> code) { for (u32 w : code) { mem.w32(a, w); a += 4; } };
    put(SIGRET_TRAMP, {0xE3A07077, 0xEF000000});     // mov r7,#119; swi 0
    put(RTSIGRET_TRAMP, {0xE3A070AD, 0xEF000000});   // mov r7,#173; swi 0
    put(0xFFFF0F60, {0xEF000000 | MAGIC_CMPXCHG64, 0xE12FFF1E}); // __kuser_cmpxchg64
    put(0xFFFF0FA0, {0xE12FFF1E});                    // __kuser_memory_barrier
    put(0xFFFF0FC0, {0xE5923000, 0xE0533000, 0x05821000, 0xE2730000, 0xE12FFF1E}); // __kuser_cmpxchg
    put(0xFFFF0FE0, {0xE59F0008, 0xE12FFF1E});        // __kuser_get_tls: ldr r0,[pc,#8]
    mem.w32(0xFFFF0FFC, 5);                           // __kuser_helper_version
}

std::string Linux::parse_launcher(const std::string& hostp, const std::string& guest_dir, std::vector<std::string>* argv) {
    std::vector<u8> d;
    if (!read_file(hostp, &d)) return "";
    std::string text(d.begin(), d.end());
    std::string dir = guest_dir;
    std::string line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        line = text.substr(pos, nl - pos);
        pos = nl + 1;
        // split into commands on ';' and '&&'
        std::vector<std::string> cmds;
        std::string c;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '#') break;
            if (line[i] == ';' || (line[i] == '&' && i + 1 < line.size() && line[i + 1] == '&')) {
                cmds.push_back(c); c.clear(); if (line[i] == '&') i++;
            } else if (line[i] != '\r') c += line[i];
        }
        cmds.push_back(c);
        for (auto& cmd : cmds) {
            std::vector<std::string> tok;
            std::string t;
            bool q = false;
            char qc = 0;
            for (char ch : cmd) {
                if (q) { if (ch == qc) q = false; else t += ch; continue; }
                if (ch == '"' || ch == '\'') { q = true; qc = ch; continue; }
                if (ch == ' ' || ch == '\t') { if (!t.empty()) tok.push_back(t); t.clear(); }
                else t += ch;
            }
            if (!t.empty()) tok.push_back(t);
            if (tok.empty()) continue;
            size_t i = 0;
            if (tok[0] == "export") {
                for (size_t k = 1; k < tok.size(); k++) if (tok[k].find('=') != std::string::npos) env_extra.push_back(tok[k]);
                continue;
            }
            while (i < tok.size() && tok[i].find('=') != std::string::npos && tok[i][0] != '.' && tok[i][0] != '/') {
                env_extra.push_back(tok[i]); i++;
            }
            if (i >= tok.size()) continue;
            if (tok[i] == "exec" || tok[i] == "nice" || tok[i] == "nohup") { i++; if (i < tok.size() && tok[i] == "-n") i += 2; }
            if (i >= tok.size()) continue;
            const std::string& prog = tok[i];
            if (prog == "cd") {
                if (i + 1 < tok.size()) {
                    std::string sv = cwd; cwd = dir; dir = abs_path(tok[i + 1]); cwd = sv;
                }
                continue;
            }
            if (prog.find("gp2xmenu") != std::string::npos || prog == "sync" || prog == "echo" ||
                prog == "insmod" || prog == "/sbin/insmod" || prog == "rmmod" || prog == "/sbin/rmmod" ||
                prog == "sleep" || prog == "mount" || prog == "umount" || prog == "chmod" || prog == "ln" ||
                prog == "rm" || prog == "cp" || prog == "mkdir" || prog == "killall" || prog == "export" ||
                prog == "if" || prog == "fi" || prog == "then" || prog == "else" || prog == "[" || prog == "test")
                continue;
            if (prog == "sh" || prog == "/bin/sh") continue;
            std::string sv = cwd;
            cwd = dir;
            std::string gpath = abs_path(prog);
            cwd = sv;
            bool found;
            to_host(gpath, true, &found);
            if (!found) continue;
            argv->assign(tok.begin() + i, tok.end());
            (*argv)[0] = gpath;
            last_launch_dir = dir;
            return gpath;
        }
    }
    return "";
}

void Linux::reset_process() {
    threads.clear();
    fds.clear();
    fd_cloexec.clear();
    for (auto& a : sigact) a = SigAction();
    mem.unmap(0, 0xFFFFF000);
    mem.unmap(0xFFFFF000, 0x1000);
    exited = false;
}

bool Linux::load(const std::string& guest_path, std::vector<std::string> argv, std::string* err) {
    std::string path = abs_path(guest_path);
    // Like the GP2X menu, start the game in its own folder.
    cwd = path.substr(0, path.rfind('/'));
    if (cwd.empty()) cwd = "/";
    // Launcher scripts (.gpe/.gpu files that are shell scripts)
    for (int depth = 0; depth < 4; depth++) {
        bool found;
        std::string h = to_host(path, true, &found);
        std::vector<u8> head;
        if (!found) { *err = "file not found: " + path; return false; }
        FILE* f = fopen(h.c_str(), "rb");
        char magic[4] = {0};
        if (f) { size_t n = fread(magic, 1, 4, f); (void)n; fclose(f); }
        if (memcmp(magic, "\x7f" "ELF", 4) == 0) break;
        if (magic[0] != '#' && !isprint((u8)magic[0])) { *err = path + " is not a program or script"; return false; }
        std::string dir = path.substr(0, path.rfind('/'));
        if (dir.empty()) dir = "/";
        std::vector<std::string> av;
        std::string next = parse_launcher(h, dir, &av);
        if (next.empty()) { *err = "could not find the game program in script " + path; return false; }
        log("Launcher script %s runs %s", path.c_str(), next.c_str());
        cwd = last_launch_dir;
        path = next;
        argv = av;
    }
    if (argv.empty()) argv.push_back(path);
    argv[0] = path;
    exe_path = path;

    u32 entry, base, phdr, phnum, ientry = 0, ibase = 0, iphdr, iphnum;
    std::string interp;
    if (!load_elf(path, &entry, &base, &phdr, &phnum, &interp, false, err)) return false;
    if (!interp.empty()) {
        log("Dynamic program, interpreter %s", interp.c_str());
        if (!load_elf(interp, &ientry, &ibase, &iphdr, &iphnum, nullptr, true, err)) {
            *err = "this game needs the firmware's libraries (" + interp + " missing). Add the firmware lib folder.";
            return false;
        }
    }
    setup_kuser_page();

    // Stack
    mem.map_anon(STACK_TOP - STACK_SIZE, STACK_SIZE);
    u32 sp = STACK_TOP - 16;
    auto push_str = [&](const std::string& s) {
        sp -= (u32)s.size() + 1;
        mem.write(sp, s.c_str(), (u32)s.size() + 1);
        return sp;
    };
    std::vector<std::string> env = {"HOME=/root", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "TERM=linux", "USER=root",
                                    "SDL_NOMOUSE=1"};
    for (auto& e : env_extra) env.push_back(e);
    u32 exefn = push_str(path);
    u32 plat = push_str(strstr(hw->machine(), "v4") ? "v4l" : "v5l");
    sp -= 16;
    u32 rnd = sp;
    for (int i = 0; i < 16; i++) mem.write(rnd + i, "\x13\x57\x9b\xdf\x24\x68\xac\xe0\x11\x22\x33\x44\x55\x66\x77\x88" + i, 1);
    std::vector<u32> argp, envp;
    for (auto& a : argv) argp.push_back(push_str(a));
    for (auto& e : env) envp.push_back(push_str(e));
    u32 hwcap = strstr(hw->machine(), "v4") ? (1 | 2 | 4 | 16) : (1 | 2 | 4 | 16 | 128);
    std::vector<u32> aux = {3, phdr, 4, 32, 5, phnum, 6, 4096, 7, ibase, 8, 0, 9, entry, 11, 0, 12, 0, 13, 0, 14, 0,
                            16, hwcap, 17, 100, 15, plat, 23, 0, 25, rnd, 31, exefn, 0, 0};
    u32 words = 1 + (u32)argp.size() + 1 + (u32)envp.size() + 1 + (u32)aux.size();
    sp = (sp - words * 4) & ~15u;
    u32 p = sp;
    mem.w32(p, (u32)argp.size()); p += 4;
    for (u32 a : argp) { mem.w32(p, a); p += 4; }
    mem.w32(p, 0); p += 4;
    for (u32 e : envp) { mem.w32(p, e); p += 4; }
    mem.w32(p, 0); p += 4;
    for (u32 a : aux) { mem.w32(p, a); p += 4; }

    // Main thread
    cpu.reset();
    cpu.switch_mode(MODE_USR);
    cpu.cpsr = MODE_USR;
    cpu.cp15_control = 0x00050078; // MMU off
    cpu.flush_tlb();
    auto t = std::make_unique<Thread>();
    t->tid = next_tid = 100;
    next_tid++;
    u32 start = interp.empty() ? entry : ientry;
    t->r[13] = sp;
    t->r[15] = start & ~1u;
    t->cpsr = MODE_USR | ((start & 1) ? FLAG_T : 0);
    threads.clear();
    threads.push_back(std::move(t));
    rr = 0;
    // stdio
    for (int i = 0; i < 3; i++) { auto f = std::make_shared<TtyFile>(); f->path = "/dev/tty"; f->flags = i ? 1 : 0; fds[i] = f; }
    exited = false;
    log("Entry 0x%08x, stack 0x%08x, brk 0x%08x", start, sp, brk_start);
    return true;
}

// ---------------------------------------------------------------------------
// System calls
// ---------------------------------------------------------------------------
bool Linux::undef_entry(ARM926* c, u32 pc, u32 insn, void* user) {
    Linux* L = (Linux*)user;
    bool thumb = c->cpsr & FLAG_T;
    if (!thumb && (insn & 0x0FFF0FFF) == 0x0E1D0F70) { // mrc p15,0,rd,c13,c0,3 (TLS register)
        u32 rd = (insn >> 12) & 15;
        c->r[rd] = L->cur ? L->cur->tls : 0;
        c->r[15] = pc + 4;
        return true;
    }
    u32 cp = (insn >> 8) & 15;
    const char* what = (!thumb && (insn & 0x0C000000) == 0x0C000000 && (cp == 1 || cp == 2))
        ? " (FPA floating point: this game was built for hardware FP emulation, not supported yet)"
        : (!thumb && (insn & 0x0C000000) == 0x0C000000 && (cp == 10 || cp == 11)) ? " (VFP)" : "";
    char buf[200];
    snprintf(buf, sizeof buf, "Illegal instruction 0x%08x at 0x%08x%s", insn, pc, what);
    if (L->sigact[4].handler > 1 && L->cur) {
        L->log("%s", buf);
        L->cur->pending |= SB(4);
        c->r[15] = pc + (thumb ? 2 : 4);
        c->request_stop();
        return true;
    }
    L->kill_process(buf, 132);
    c->r[15] = pc;
    return true;
}

bool Linux::swi_entry(ARM926*, u32 pc, u32 imm, void* user) {
    return ((Linux*)user)->syscall(pc, imm);
}

bool Linux::syscall(u32 pc, u32 imm) {
    bool thumb = cpu.cpsr & FLAG_T;
    u32 next = pc + (thumb ? 2 : 4);
    sys_next_pc = next;
    if (!thumb && imm == MAGIC_CMPXCHG64) {
        // r0 = &oldval, r1 = &newval, r2 = ptr; returns 0 on success
        u64 o = 0, n = 0, v = 0;
        mem.read(cpu.r[0], &o, 8); mem.read(cpu.r[1], &n, 8); mem.read(cpu.r[2], &v, 8);
        if (v == o) { mem.write(cpu.r[2], &n, 8); cpu.r[0] = 0; cpu.cpsr |= FLAG_C; }
        else { cpu.r[0] = 1; cpu.cpsr &= ~FLAG_C; }
        cpu.r[15] = next;
        return true;
    }
    u32 nr;
    oabi = false;
    if (thumb || imm == 0) nr = cpu.r[7];
    else if (imm >= 0x900000 && imm <= 0x9FFFFF) { nr = imm - 0x900000; oabi = true; if (nr >= 0xF0000) nr = (nr - 0xF0000) + 0xF0000; }
    else {
        log("Unknown SWI 0x%06x at 0x%08x", imm, pc);
        cpu.r[0] = (u32)-G_ENOSYS;
        cpu.r[15] = next;
        return true;
    }
    u32 a[7];
    for (int i = 0; i < 7; i++) a[i] = cpu.r[i];
    bool noret = false;
    s64 res = dispatch(nr, a, &noret);
    if (strace && (nr == 5 || nr == 33 || nr == 106 || nr == 195 || nr == 196 || nr == 11 || nr == 12))
        log("[%d]   path \"%s\"", cur ? cur->tid : 0, mem.read_str(a[0], 256).c_str());
    if (strace && (nr == 322 || nr == 327 || nr == 334))
        log("[%d]   path \"%s\"", cur ? cur->tid : 0, mem.read_str(a[1], 256).c_str());
    if (strace && nr != 0xF0005) log("[%d] syscall %u(0x%x, 0x%x, 0x%x, 0x%x) = %lld%s", cur ? cur->tid : 0, nr, a[0], a[1], a[2], a[3],
                    (long long)res, res == SYS_BLOCK ? " (block)" : "");
    if (noret) return true;
    if (res == SYS_BLOCK) {
        cpu.r[15] = cur->restart ? pc : next;
        cpu.request_stop();
        return true;
    }
    cpu.r[0] = (u32)res;
    cpu.r[15] = next;
    return true;
}

static s32 nb_fix(Linux& L, const std::shared_ptr<File>& f, s32 r) {
    if (r == SYS_BLOCK && (f->flags & GO_NONBLOCK)) {
        L.cur->state = Thread::RUN;
        L.cur->wait_obj = nullptr;
        L.cur->wake_at = ~0ull;
        return -G_EAGAIN;
    }
    return r;
}

void Linux::set_tls(u32 v) {
    cur->tls = v;
    mem.w32(0xFFFF0FF0, v);
}

s32 Linux::do_mmap(u32 addr, u32 len, u32 prot, u32 flags, int fd, u64 off) {
    if (len == 0) return -G_EINVAL;
    u32 alen = (len + 0xFFF) & ~0xFFFu;
    u32 va;
    if (flags & 0x10) { // MAP_FIXED
        if (addr & 0xFFF) return -G_EINVAL;
        va = addr;
        mem.unmap(va, alen);
    } else {
        va = mem.find_free(alen, addr & ~0xFFFu);
        if (!va) va = mem.find_free(alen, 0, 0x10000000, 0xBE000000);
        if (!va) return -G_ENOMEM;
    }
    if (flags & 0x20) { // MAP_ANONYMOUS
        mem.map_anon(va, alen);
        return (s32)va;
    }
    auto f = getf(fd);
    if (!f) return -G_EBADF;
    s32 r = f->mmap(*this, va, alen, prot, flags, off);
    if (r < 0) return r;
    return (s32)va;
}

s32 Linux::do_futex(u32 uaddr, int op, u32 val, u32 timeout, u32 uaddr2, u32 val3, bool time64) {
    int cmd = op & 127;
    auto wake_n = [&](u32 addr, u32 n, u32 bitset) {
        u32 woken = 0;
        for (auto& t : threads) {
            if (woken >= n) break;
            if (t->state == Thread::BLOCKED && t->wait_obj == FUTEX_TAG && t->futex_addr == addr && (t->futex_bitset & bitset)) {
                unblock(t.get(), false);
                woken++;
            }
        }
        return woken;
    };
    switch (cmd) {
    case 0: case 9: { // WAIT, WAIT_BITSET
        if (mem.r32(uaddr) != val) return -G_EAGAIN;
        u64 until = ~0ull;
        if (timeout) {
            s64 sec, nsec;
            if (time64) { mem.read(timeout, &sec, 8); mem.read(timeout + 8, &nsec, 8); }
            else { sec = (s32)mem.r32(timeout); nsec = (s32)mem.r32(timeout + 4); }
            u64 us = (u64)sec * 1000000ull + (u64)nsec / 1000;
            if (cmd == 9) { // absolute
                u64 base = (op & 256) ? epoch_usec : 0;
                u64 nowus = usec();
                us = us > base + nowus ? us - base - nowus : 0;
            }
            until = now() + cycles_from_usec(us);
        }
        cur->futex_addr = uaddr;
        cur->futex_bitset = cmd == 9 ? val3 : 0xFFFFFFFF;
        return block_wait(FUTEX_TAG, until, -G_ETIMEDOUT);
    }
    case 1: return (s32)wake_n(uaddr, val, 0xFFFFFFFF);
    case 10: return (s32)wake_n(uaddr, val, val3);
    case 3: case 4: { // REQUEUE, CMP_REQUEUE
        if (cmd == 4 && mem.r32(uaddr) != val3) return -G_EAGAIN;
        u32 woken = wake_n(uaddr, val, 0xFFFFFFFF);
        u32 moved = 0;
        for (auto& t : threads) {
            if (moved >= timeout) break;
            if (t->state == Thread::BLOCKED && t->wait_obj == FUTEX_TAG && t->futex_addr == uaddr) { t->futex_addr = uaddr2; moved++; }
        }
        return (s32)(woken + moved);
    }
    case 5: { // WAKE_OP
        u32 opnum = (val3 >> 28) & 7, cmp = (val3 >> 24) & 15;
        s32 oparg = ((s32)(val3 << 8)) >> 20, cmparg = ((s32)(val3 << 20)) >> 20;
        if ((val3 >> 28) & 8) oparg = 1 << (oparg & 31);
        s32 old = (s32)mem.r32(uaddr2), nv = old;
        switch (opnum) { case 0: nv = oparg; break; case 1: nv = old + oparg; break; case 2: nv = old | oparg; break;
                         case 3: nv = old & ~oparg; break; case 4: nv = old ^ oparg; break; }
        mem.w32(uaddr2, (u32)nv);
        u32 w = wake_n(uaddr, val, 0xFFFFFFFF);
        bool c = false;
        switch (cmp) { case 0: c = old == cmparg; break; case 1: c = old != cmparg; break; case 2: c = old < cmparg; break;
                       case 3: c = old <= cmparg; break; case 4: c = old > cmparg; break; case 5: c = old >= cmparg; break; }
        if (c) w += wake_n(uaddr2, timeout, 0xFFFFFFFF);
        return (s32)w;
    }
    }
    return -G_ENOSYS;
}

s32 Linux::do_poll(u32 fds_addr, u32 nfds, s64 timeout_ms) {
    s32 count = 0;
    for (u32 i = 0; i < nfds; i++) {
        u32 a = fds_addr + i * 8;
        s32 fd = (s32)mem.r32(a);
        u16 ev = 0; mem.read(a + 4, &ev, 2);
        u16 rev = 0;
        if (fd >= 0) {
            auto f = getf(fd);
            if (!f) rev = 0x20;
            else rev = (u16)(f->poll_mask(*this) & (ev | 0x18));
        }
        mem.write(a + 6, &rev, 2);
        if (rev) count++;
    }
    u32 pc = sys_next_pc;
    if (count || timeout_ms == 0) { cur->retry_pc = 0; return count; }
    if (cur->retry_pc != pc) {
        cur->retry_pc = pc;
        cur->retry_deadline = timeout_ms < 0 ? ~0ull : now() + cycles_from_usec((u64)timeout_ms * 1000);
    }
    if (now() >= cur->retry_deadline) { cur->retry_pc = 0; return 0; }
    return block_retry(nullptr, std::min(cur->retry_deadline, now() + cycles_from_usec(2000)));
}

s32 Linux::do_select(int n, u32 rd, u32 wr, u32 ex, u32 tv) {
    u32 words = (u32)(n + 31) / 32;
    std::vector<u32> r(words, 0), w(words, 0), ro(words, 0), wo(words, 0);
    if (rd) mem.read(rd, r.data(), words * 4);
    if (wr) mem.read(wr, w.data(), words * 4);
    s32 count = 0;
    for (int fd = 0; fd < n; fd++) {
        bool wr_ = rd && (r[fd / 32] >> (fd % 32)) & 1, ww = wr && (w[fd / 32] >> (fd % 32)) & 1;
        if (!wr_ && !ww) continue;
        auto f = getf(fd);
        if (!f) return -G_EBADF;
        int m = f->poll_mask(*this);
        if (wr_ && (m & 1)) { ro[fd / 32] |= 1u << (fd % 32); count++; }
        if (ww && (m & 4)) { wo[fd / 32] |= 1u << (fd % 32); count++; }
    }
    s64 to_us = -1;
    if (tv) to_us = (s64)(s32)mem.r32(tv) * 1000000 + (s32)mem.r32(tv + 4);
    u32 pc = sys_next_pc;
    if (count || to_us == 0) {
        cur->retry_pc = 0;
        if (rd) mem.write(rd, ro.data(), words * 4);
        if (wr) mem.write(wr, wo.data(), words * 4);
        if (ex) mem.zero(ex, words * 4);
        return count;
    }
    if (cur->retry_pc != pc) {
        cur->retry_pc = pc;
        cur->retry_deadline = to_us < 0 ? ~0ull : now() + cycles_from_usec((u64)to_us);
    }
    if (now() >= cur->retry_deadline) {
        cur->retry_pc = 0;
        if (rd) mem.zero(rd, words * 4);
        if (wr) mem.zero(wr, words * 4);
        if (ex) mem.zero(ex, words * 4);
        if (tv) { mem.w32(tv, 0); mem.w32(tv + 4, 0); }
        return 0;
    }
    return block_retry(nullptr, std::min(cur->retry_deadline, now() + cycles_from_usec(2000)));
}

s32 Linux::do_clone(u32 flags, u32 sp, u32 ptid, u32 tls, u32 ctid) {
    if (!(flags & 0x100)) { // no CLONE_VM: fork
        log("fork() is not supported (game tried to start another program)");
        return -G_ENOSYS;
    }
    auto t = std::make_unique<Thread>();
    t->tid = next_tid++;
    memcpy(t->r, cpu.r, sizeof t->r);
    t->r[0] = 0;
    if (sp) t->r[13] = sp;
    t->r[15] = sys_next_pc;
    t->cpsr = cpu.cpsr;
    t->tls = (flags & 0x80000) ? tls : cur->tls;
    if (flags & 0x10000) { t->pid = cur->pid; t->ppid = cur->ppid; } // CLONE_THREAD
    else { t->pid = t->tid; t->ppid = (flags & 0x8000) ? cur->ppid : cur->pid; } // CLONE_PARENT
    t->sigmask = cur->sigmask;
    if (flags & 0x100000) mem.w32(ptid, (u32)t->tid);
    if (flags & 0x01000000) mem.w32(ctid, (u32)t->tid);
    if (flags & 0x200000) t->clear_tid = ctid;
    int tid = t->tid;
    threads.push_back(std::move(t));
    return tid;
}

s32 Linux::do_sigaction(int sig, u32 act, u32 old, bool rt) {
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19) return act ? -G_EINVAL : 0;
    SigAction& a = sigact[sig];
    if (old) {
        if (rt) { mem.w32(old, a.handler); mem.w32(old + 4, a.flags); mem.w32(old + 8, a.restorer); mem.write(old + 12, &a.mask, 8); }
        else { mem.w32(old, a.handler); mem.w32(old + 4, (u32)a.mask); mem.w32(old + 8, a.flags); mem.w32(old + 12, a.restorer); }
    }
    if (act) {
        if (rt) { a.handler = mem.r32(act); a.flags = mem.r32(act + 4); a.restorer = mem.r32(act + 8); mem.read(act + 12, &a.mask, 8); }
        else { a.handler = mem.r32(act); a.mask = mem.r32(act + 4); a.flags = mem.r32(act + 8); a.restorer = mem.r32(act + 12); }
    }
    return 0;
}

s32 Linux::do_execve(const std::string& path, u32 argv, u32) {
    std::string p = abs_path(path);
    if (p.find("gp2xmenu") != std::string::npos || p.find("/usr/gp2x") == 0 || p.find("gmenu2x") != std::string::npos ||
        p.find("/usr/gpwiz") == 0 || p.find("/usr/gpcaanoo") == 0) {
        kill_process("Game returned to the menu", 0);
        return 0;
    }
    std::vector<std::string> av;
    for (int i = 0; argv && i < 64; i++) {
        u32 s = mem.r32(argv + i * 4);
        if (!s) break;
        av.push_back(mem.read_str(s));
    }
    bool found;
    to_host(p, true, &found);
    if (!found) return -G_ENOENT;
    log("execve %s", p.c_str());
    exec_path = p;
    exec_argv = av;
    pending_exec = true;
    for (auto& t : threads) t->state = Thread::DEAD;
    cpu.request_stop();
    return 0;
}

static void put_timespec(GuestMem& m, u32 a, u64 us, bool t64) {
    if (t64) { s64 s = (s64)(us / 1000000), n = (s64)(us % 1000000) * 1000; m.write(a, &s, 8); m.write(a + 8, &n, 8); }
    else { m.w32(a, (u32)(us / 1000000)); m.w32(a + 4, (u32)(us % 1000000) * 1000); }
}

s64 Linux::dispatch(u32 nr, u32* a, bool* noret) {
    auto F = [&](u32 fd) { return getf((int)fd); };
    auto S = [&](u32 p) { return mem.read_str(p); };
    auto at_path = [&](s32 dirfd, const std::string& p) -> std::string {
        if (!p.empty() && p[0] == '/') return p;
        if (dirfd == -100) return p;
        auto f = getf(dirfd);
        if (!f) return p;
        return f->path + "/" + p;
    };
    switch (nr) {
    case 1: thread_exit(cur, (int)a[0]); *noret = true; return 0;          // exit
    case 248: { char b[64]; snprintf(b, sizeof b, "Program exited with code %d", (int)a[0]); kill_process(b, (int)a[0]); *noret = true; return 0; }
    case 2: case 190: log("fork/vfork not supported"); return -G_ENOSYS;
    case 3: { auto f = F(a[0]); if (!f) return -G_EBADF; return nb_fix(*this, f, f->read(*this, a[1], a[2])); }
    case 4: { auto f = F(a[0]); if (!f) return -G_EBADF; return nb_fix(*this, f, f->write(*this, a[1], a[2])); }
    case 145: case 146: { // readv / writev
        auto f = F(a[0]); if (!f) return -G_EBADF;
        s32 total = 0;
        for (u32 i = 0; i < a[2]; i++) {
            u32 base = mem.r32(a[1] + i * 8), len = mem.r32(a[1] + i * 8 + 4);
            if (!len) continue;
            s32 r = nr == 145 ? f->read(*this, base, len) : f->write(*this, base, len);
            if (r == SYS_BLOCK) { if (total) { cur->state = Thread::RUN; cur->wait_obj = nullptr; cur->wake_at = ~0ull; return total; } return nb_fix(*this, f, r); }
            if (r < 0) return total ? total : r;
            total += r;
            if ((u32)r < len) break;
        }
        return total;
    }
    case 5: return do_open(S(a[0]), (int)a[1], (int)a[2]);
    case 8: return do_open(S(a[0]), 01 | GO_CREAT | GO_TRUNC, (int)a[1]);
    case 322: return do_open(at_path((s32)a[0], S(a[1])), (int)a[2], (int)a[3]);
    case 6: { if (!fds.count((int)a[0])) return -G_EBADF; fds.erase((int)a[0]); fd_cloexec.erase((int)a[0]); return 0; }
    case 19: { auto f = F(a[0]); if (!f) return -G_EBADF; s64 r = f->lseek(*this, (s32)a[1], (int)a[2]); return r < 0 ? r : (s32)r; }
    case 140: { // _llseek
        auto f = F(a[0]); if (!f) return -G_EBADF;
        s64 off = (s64)(((u64)a[1] << 32) | a[2]);
        s64 r = f->lseek(*this, off, (int)a[4]);
        if (r < 0) return r;
        mem.write(a[3], &r, 8);
        return 0;
    }
    case 180: case 181: { // pread64 / pwrite64
        auto f = F(a[0]); if (!f) return -G_EBADF;
        s64 off = oabi ? (s64)(((u64)a[4] << 32) | a[3]) : (s64)(((u64)a[5] << 32) | a[4]);
        return nr == 180 ? f->pread(*this, a[1], a[2], off) : f->pwrite(*this, a[1], a[2], off);
    }
    case 41: { auto f = F(a[0]); if (!f) return -G_EBADF; int fd = alloc_fd(); fds[fd] = f; return fd; }
    case 63: case 358: { // dup2 / dup3
        auto f = F(a[0]); if (!f) return -G_EBADF;
        if (a[0] == a[1]) return (s32)a[1];
        fds[(int)a[1]] = f;
        return (s32)a[1];
    }
    case 42: case 359: { // pipe / pipe2
        auto pb = std::make_shared<PipeBuf>();
        auto r = std::make_shared<PipeEnd>(pb, false), w = std::make_shared<PipeEnd>(pb, true);
        r->path = w->path = "pipe:";
        if (nr == 359) { r->flags = (int)(a[1] & GO_NONBLOCK); w->flags = 1 | (int)(a[1] & GO_NONBLOCK); } else w->flags = 1;
        int fr = alloc_fd(); fds[fr] = r;
        int fw = alloc_fd(); fds[fw] = w;
        mem.w32(a[0], (u32)fr); mem.w32(a[0] + 4, (u32)fw);
        return 0;
    }
    case 54: { // ioctl
        auto f = F(a[0]); if (!f) return -G_EBADF;
        if (a[1] == 0x5421) { u32 v = mem.r32(a[2]); if (v) f->flags |= GO_NONBLOCK; else f->flags &= ~GO_NONBLOCK; return 0; }
        if (a[1] == 0x5451 || a[1] == 0x5450) return 0; // FIOCLEX / FIONCLEX
        return nb_fix(*this, f, f->ioctl(*this, a[1], a[2]));
    }
    case 55: case 221: { // fcntl / fcntl64
        auto f = F(a[0]); if (!f) return -G_EBADF;
        switch (a[1]) {
        case 0: case 1030: { int fd = alloc_fd((int)a[2]); fds[fd] = f; return fd; }
        case 1: return fd_cloexec.count((int)a[0]) ? 1 : 0;
        case 2: if (a[2] & 1) fd_cloexec[(int)a[0]] = 1; else fd_cloexec.erase((int)a[0]); return 0;
        case 3: return f->flags;
        case 4: f->flags = (f->flags & ~(GO_NONBLOCK | GO_APPEND)) | (int)(a[2] & (GO_NONBLOCK | GO_APPEND)); return 0;
        case 5: case 12: mem.write(a[2], "\x02\x00", 2); return 0; // F_GETLK: F_UNLCK
        default: return 0;
        }
    }
    case 106: case 107: case 195: case 196: { // stat, lstat, stat64, lstat64
        GuestStat st;
        s32 r = do_stat(S(a[0]), nr == 106 || nr == 195, &st);
        if (r < 0) return r;
        put_stat(a[1], st, nr < 195 ? 0 : (oabi ? 2 : 1));
        return 0;
    }
    case 108: case 197: { // fstat, fstat64
        auto f = F(a[0]); if (!f) return -G_EBADF;
        GuestStat st;
        s32 r = f->fstat(*this, &st);
        if (r < 0) return r;
        put_stat(a[1], st, nr == 108 ? 0 : (oabi ? 2 : 1));
        return 0;
    }
    case 327: { // fstatat64
        GuestStat st;
        std::string p = S(a[1]);
        s32 r;
        if (p.empty() && (a[3] & 0x1000)) { auto f = F(a[0]); if (!f) return -G_EBADF; r = f->fstat(*this, &st); }
        else r = do_stat(at_path((s32)a[0], p), !(a[3] & 0x100), &st);
        if (r < 0) return r;
        put_stat(a[2], st, oabi ? 2 : 1);
        return 0;
    }
    case 397: return -G_ENOSYS; // statx: glibc falls back to fstatat64
    case 141: case 217: { auto f = F(a[0]); if (!f) return -G_EBADF; return f->getdents(*this, a[1], a[2], nr == 217); }
    case 89: { // old readdir
        auto f = F(a[0]); if (!f) return -G_EBADF;
        u8 tmp[300];
        u32 scratch = (cpu.r[13] - 1024) & ~3u;
        s32 r = f->getdents(*this, scratch, 280, false);
        if (r <= 0) return r;
        mem.read(scratch, tmp, 280);
        u16 rl; memcpy(&rl, tmp + 8, 2);
        u16 nl = (u16)strlen((const char*)tmp + 10);
        mem.write(a[1], tmp, 8); mem.write(a[1] + 8, &nl, 2); mem.write(a[1] + 10, tmp + 10, nl + 1u);
        return 1;
    }
    case 33: case 334: { // access, faccessat
        std::string p = nr == 33 ? S(a[0]) : at_path((s32)a[0], S(a[1]));
        GuestStat st;
        s32 r = do_stat(p, true, &st);
        return r < 0 ? r : 0;
    }
    case 85: case 332: { // readlink, readlinkat
        std::string p = nr == 85 ? S(a[0]) : at_path((s32)a[0], S(a[1]));
        u32 buf = nr == 85 ? a[1] : a[2], sz = nr == 85 ? a[2] : a[3];
        std::string ap = abs_path(p), out;
        if (ap == "/proc/self/exe" || ap.rfind("/proc/", 0) == 0) out = exe_path;
        else {
            bool found;
            std::string h = to_host(ap, false, &found);
            char tmp[4096];
            ssize_t n = ::readlink(h.c_str(), tmp, sizeof tmp);
            if (n < 0) return -errno;
            out.assign(tmp, (size_t)n);
        }
        u32 n = std::min<u32>(sz, (u32)out.size());
        mem.write(buf, out.data(), n);
        return (s32)n;
    }
    case 12: { // chdir
        std::string p = abs_path(S(a[0]));
        GuestStat st;
        s32 r = do_stat(p, true, &st);
        if (r < 0) return r;
        if (!S_ISDIR(st.mode)) return -G_ENOTDIR;
        cwd = p;
        return 0;
    }
    case 133: { auto f = F(a[0]); if (!f) return -G_EBADF; if (!f->is_dir()) return -G_ENOTDIR; cwd = f->path; return 0; }
    case 183: { // getcwd
        if (cwd.size() + 1 > a[1]) return -G_ERANGE;
        mem.write(a[0], cwd.c_str(), (u32)cwd.size() + 1);
        return (s32)cwd.size() + 1;
    }
    case 10: case 40: { std::string h = to_host(S(a[0]), false); return (nr == 10 ? ::unlink(h.c_str()) : ::rmdir(h.c_str())) < 0 ? -errno : 0; }
    case 328: { std::string h = to_host(at_path((s32)a[0], S(a[1])), false); return ((a[2] & 0x200) ? ::rmdir(h.c_str()) : ::unlink(h.c_str())) < 0 ? -errno : 0; }
    case 39: { std::string h = to_host(S(a[0])); return ::mkdir(h.c_str(), 0777) < 0 ? -errno : 0; }
    case 323: { std::string h = to_host(at_path((s32)a[0], S(a[1]))); return ::mkdir(h.c_str(), 0777) < 0 ? -errno : 0; }
    case 38: { std::string x = to_host(S(a[0]), false), y = to_host(S(a[1]), false); return ::rename(x.c_str(), y.c_str()) < 0 ? -errno : 0; }
    case 92: case 193: { // truncate, truncate64
        std::string h = to_host(S(a[0]));
        s64 len = nr == 92 ? (s64)a[1] : oabi ? (s64)(((u64)a[2] << 32) | a[1]) : (s64)(((u64)a[3] << 32) | a[2]);
        return ::truncate(h.c_str(), (off_t)len) < 0 ? -errno : 0;
    }
    case 93: case 194: {
        auto f = F(a[0]); if (!f) return -G_EBADF;
        s64 len = nr == 93 ? (s64)a[1] : oabi ? (s64)(((u64)a[2] << 32) | a[1]) : (s64)(((u64)a[3] << 32) | a[2]);
        return f->ftruncate(*this, len);
    }
    case 15: case 94: case 30: case 271: case 348: case 182: case 95: case 212: case 207: case 16: return 0; // chmod/fchmod/utime/chown...
    case 83: case 9: case 14: return -G_EPERM;   // symlink, link, mknod
    case 36: case 118: case 148: return 0;       // sync, fsync, fdatasync
    case 60: { u32 o = umask_v; umask_v = a[0] & 0777; return o; }
    case 45: { // brk
        u32 want = a[0];
        if (want == 0 || want < brk_start) return brk_cur;
        u32 cur_top = (brk_cur + 0xFFF) & ~0xFFFu, new_top = (want + 0xFFF) & ~0xFFFu;
        if (new_top > cur_top) {
            if (!mem.range_free(cur_top, new_top - cur_top)) return brk_cur;
            mem.map_anon(cur_top, new_top - cur_top);
        } else if (new_top < cur_top) mem.unmap(new_top, cur_top - new_top);
        brk_cur = want;
        return brk_cur;
    }
    case 90: { // old mmap: args in memory
        u32 v[6];
        mem.read(a[0], v, 24);
        if (strace) log("  old_mmap(addr 0x%x, len 0x%x, prot %u, flags 0x%x, fd %d, off 0x%x)", v[0], v[1], v[2], v[3], (int)v[4], v[5]);
        return do_mmap(v[0], v[1], v[2], v[3], (int)v[4], v[5]);
    }
    case 192: return do_mmap(a[0], a[1], a[2], a[3], (int)a[4], (u64)a[5] << 12);
    case 91: if (a[0] & 0xFFF) return -G_EINVAL; mem.unmap(a[0], (a[1] + 0xFFF) & ~0xFFFu); return 0;
    case 125: case 220: case 144: case 150: case 151: case 152: case 153: return 0; // mprotect, madvise, msync, mlock...
    case 163: { // mremap
        u32 old = a[0], os = (a[1] + 0xFFF) & ~0xFFFu, ns = (a[2] + 0xFFF) & ~0xFFFu;
        if (ns <= os) { if (ns < os) mem.unmap(old + ns, os - ns); return (s32)old; }
        if (mem.range_free(old + os, ns - os)) { mem.map_anon(old + os, ns - os); return (s32)old; }
        if (!(a[3] & 1)) return -G_ENOMEM;
        u32 nv = mem.find_free(ns, 0);
        if (!nv) return -G_ENOMEM;
        mem.move(old, nv, os);
        mem.map_anon(nv + os, ns - os);
        return (s32)nv;
    }
    case 120: return do_clone(a[0], a[1], a[2], a[3], a[4]);
    case 11: return do_execve(S(a[0]), a[1], a[2]);
    case 114: case 7: return -G_ECHILD;           // wait4, waitpid
    case 20: return cur->pid;                     // getpid
    case 224: return cur->tid;                    // gettid
    case 64: return cur->ppid;                    // getppid
    case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: return 0; // uid/gid
    case 23: case 46: case 213: case 214: case 57: case 66: case 65: case 132: case 147: return 0;
    case 205: case 80: return 0;                  // getgroups
    case 256: cur->clear_tid = a[0]; return cur->tid; // set_tid_address
    case 338: case 339: return 0;                 // robust list
    case 398: case 435: return -G_ENOSYS;         // rseq, clone3 (glibc falls back to clone)
    case 122: { // uname
        char u[390];
        memset(u, 0, sizeof u);
        strcpy(u, "Linux"); strcpy(u + 65, "gp2x"); strncpy(u + 130, uname_release.c_str(), 64);
        strcpy(u + 195, "#1 PREEMPT"); strncpy(u + 260, hw->machine(), 64); strcpy(u + 325, "(none)");
        mem.write(a[0], u, 390);
        return 0;
    }
    case 13: { u32 t = (u32)((epoch_usec + usec()) / 1000000); if (a[0]) mem.w32(a[0], t); return t; }
    case 78: { // gettimeofday
        u64 us = epoch_usec + usec();
        if (a[0]) { mem.w32(a[0], (u32)(us / 1000000)); mem.w32(a[0] + 4, (u32)(us % 1000000)); }
        if (a[1]) { mem.w32(a[1], 0); mem.w32(a[1] + 4, 0); }
        return 0;
    }
    case 79: return 0;
    case 263: case 403: { // clock_gettime(64)
        u64 us = (a[0] == 0 || a[0] == 8) ? epoch_usec + usec() : usec();
        put_timespec(mem, a[1], us, nr == 403);
        return 0;
    }
    case 264: case 406: if (a[1]) put_timespec(mem, a[1], 1, nr == 406); return 0;
    case 43: { // times
        u32 ticks = (u32)(usec() / 10000);
        if (a[0]) { mem.w32(a[0], ticks); mem.w32(a[0] + 4, 0); mem.w32(a[0] + 8, 0); mem.w32(a[0] + 12, 0); }
        return ticks;
    }
    case 162: case 265: case 407: { // nanosleep, clock_nanosleep(64)
        u32 ts = nr == 162 ? a[0] : a[2];
        bool t64 = nr == 407;
        s64 sec, nsec;
        if (t64) { mem.read(ts, &sec, 8); mem.read(ts + 8, &nsec, 8); }
        else { sec = (s32)mem.r32(ts); nsec = (s32)mem.r32(ts + 4); }
        u64 us = (u64)sec * 1000000ull + (u64)(nsec / 1000);
        if (nr != 162 && (a[1] & 1)) { // TIMER_ABSTIME
            u64 base = (a[0] == 0) ? epoch_usec : 0, n = base + usec();
            us = us > n ? us - n : 0;
        }
        if (us == 0) { cpu.request_stop(); return 0; }
        return block_wait(nullptr, now() + cycles_from_usec(us), 0);
    }
    case 158: cpu.request_stop(); return 0;       // sched_yield
    case 154: case 155: case 156: case 157: case 241: case 242: case 96: case 97: return 0;
    case 159: return 99;
    case 160: return 1;
    case 168: return do_poll(a[0], a[1], (s32)a[2]);
    case 142: return do_select((int)a[0], a[1], a[2], a[3], a[4]);
    case 335: case 413: { // pselect6(_time64): timespec timeout
        u32 tv = 0;
        u32 scratch = (cpu.r[13] - 64) & ~7u;
        if (a[4]) {
            s64 s, ns;
            if (nr == 413) { mem.read(a[4], &s, 8); mem.read(a[4] + 8, &ns, 8); }
            else { s = (s32)mem.r32(a[4]); ns = (s32)mem.r32(a[4] + 4); }
            mem.w32(scratch, (u32)s); mem.w32(scratch + 4, (u32)(ns / 1000));
            tv = scratch;
        }
        return do_select((int)a[0], a[1], a[2], a[3], tv);
    }
    case 336: case 414: { // ppoll(_time64)
        s64 ms = -1;
        if (a[2]) {
            s64 s, ns;
            if (nr == 414) { mem.read(a[2], &s, 8); mem.read(a[2] + 8, &ns, 8); }
            else { s = (s32)mem.r32(a[2]); ns = (s32)mem.r32(a[2] + 4); }
            ms = s * 1000 + ns / 1000000;
        }
        return do_poll(a[0], a[1], ms);
    }
    case 82: { u32 v[5]; mem.read(a[0], v, 20); return do_select((int)v[0], v[1], v[2], v[3], v[4]); }
    case 240: return do_futex(a[0], (int)a[1], a[2], a[3], a[4], a[5], false);
    case 422: return do_futex(a[0], (int)a[1], a[2], a[3], a[4], a[5], true);
    case 67: return do_sigaction((int)a[0], a[1], a[2], false);
    case 174: return do_sigaction((int)a[0], a[1], a[2], true);
    case 126: case 175: { // sigprocmask / rt_sigprocmask
        u64 old = cur->sigmask, set = 0;
        if (a[2]) { if (nr == 175) mem.write(a[2], &old, 8); else mem.w32(a[2], (u32)old); }
        if (a[1]) {
            if (nr == 175) mem.read(a[1], &set, 8); else set = mem.r32(a[1]);
            if (a[0] == 0) cur->sigmask |= set;
            else if (a[0] == 1) cur->sigmask &= ~set;
            else if (a[0] == 2) cur->sigmask = set;
            else return -G_EINVAL;
            cur->sigmask &= ~(SB(9) | SB(19));
            cpu.request_stop(); // let newly unblocked signals be delivered
        }
        return 0;
    }
    case 73: case 176: { u64 p = cur->pending; if (nr == 176) mem.write(a[0], &p, 8); else mem.w32(a[0], (u32)p); return 0; }
    case 72: case 179: case 29: { // sigsuspend, rt_sigsuspend, pause
        u64 m = cur->sigmask;
        if (nr == 179) mem.read(a[0], &m, 8);
        else if (nr == 72) m = a[2];
        cur->suspend_saved_mask = cur->sigmask;
        cur->sigmask = m & ~(SB(9) | SB(19));
        cur->sigsuspend = true;
        return block_wait(nullptr, ~0ull, -G_EINTR);
    }
    case 177: { // rt_sigtimedwait
        u64 set = 0; mem.read(a[0], &set, 8);
        u64 hit = cur->pending & set;
        if (hit) {
            int s = __builtin_ctzll(hit) + 1;
            cur->pending &= ~SB(s);
            if (a[1]) { mem.zero(a[1], 128); mem.w32(a[1], (u32)s); }
            cur->retry_pc = 0;
            return s;
        }
        u32 pc = sys_next_pc;
        if (cur->retry_pc != pc) {
            cur->retry_pc = pc;
            cur->retry_deadline = a[2] ? now() + cycles_from_usec((u64)mem.r32(a[2]) * 1000000 + mem.r32(a[2] + 4) / 1000) : ~0ull;
        }
        if (now() >= cur->retry_deadline) { cur->retry_pc = 0; return -G_EAGAIN; }
        return block_retry(nullptr, std::min(cur->retry_deadline, now() + cycles_from_usec(2000)));
    }
    case 119: case 173: { // sigreturn, rt_sigreturn
        *noret = true;
        if (cur->frames.empty()) { cpu.r[15] = sys_next_pc; return 0; }
        Thread::Frame f = cur->frames.back();
        cur->frames.pop_back();
        memcpy(cpu.r, f.r, sizeof f.r);
        cpu.cpsr = (f.cpsr & ~0x1Fu) | MODE_USR;
        cur->sigmask = f.mask;
        cpu.request_stop();
        return 0;
    }
    case 186: if (a[1]) { mem.w32(a[1], 0); mem.w32(a[1] + 4, 2); mem.w32(a[1] + 8, 0); } return 0; // sigaltstack
    case 37: { // kill
        s32 pid = (s32)a[0];
        if (pid == 0 || pid == -1 || pid == -100) { if (a[1]) queue_signal(0, (int)a[1]); cpu.request_stop(); return 0; }
        if (pid < 0) pid = -pid;
        Thread* target = nullptr;
        for (auto& t : threads) if (t->state != Thread::DEAD && t->tid == pid) target = t.get();
        if (!target) for (auto& t : threads) if (t->state != Thread::DEAD && t->pid == pid) { target = t.get(); break; }
        if (!target) return -G_ESRCH;
        if (a[1]) {
            if (target->pid == 100 && pid == 100 && target->tid == 100 && thread_count() > 1 && sigact[a[1]].handler == 0) queue_signal(0, (int)a[1]);
            else queue_signal(target->tid, (int)a[1]);
        }
        cpu.request_stop();
        return 0;
    }
    case 238: case 268: { // tkill, tgkill
        int tid = (int)(nr == 238 ? a[0] : a[1]), sig = (int)(nr == 238 ? a[1] : a[2]);
        bool ok = false;
        for (auto& t : threads) if (t->tid == tid && t->state != Thread::DEAD) ok = true;
        if (!ok) return -G_ESRCH;
        if (sig) queue_signal(tid, sig);
        cpu.request_stop();
        return 0;
    }
    case 27: return 0;                           // alarm (unsupported, rarely used)
    case 104: case 105: if (nr == 105 || a[2]) mem.zero(nr == 105 ? a[1] : a[2], 16); return 0; // set/getitimer
    case 76: case 191: { // getrlimit / ugetrlimit
        u32 cur_v = a[0] == 3 ? STACK_SIZE : 0xFFFFFFFF;
        mem.w32(a[1], cur_v); mem.w32(a[1] + 4, 0xFFFFFFFF);
        return 0;
    }
    case 75: return 0;
    case 369: { // prlimit64
        if (a[3]) { u64 v[2] = {a[1] == 3 ? (u64)STACK_SIZE : ~0ull, ~0ull}; mem.write(a[3], v, 16); }
        return 0;
    }
    case 77: mem.zero(a[1], 72); return 0;      // getrusage
    case 116: { // sysinfo
        mem.zero(a[0], 64);
        mem.w32(a[0], (u32)(usec() / 1000000));
        mem.w32(a[0] + 16, 32u << 20); mem.w32(a[0] + 20, 20u << 20);
        u16 procs = 1; mem.write(a[0] + 40, &procs, 2);
        mem.w32(a[0] + 52, 1);
        return 0;
    }
    case 99: case 100: case 266: case 267: { // statfs family
        u32 buf = (nr == 266 || nr == 267) ? a[2] : a[1];
        mem.zero(buf, 84);
        mem.w32(buf, 0x4d44); mem.w32(buf + 4, 4096);
        if (nr >= 266) { u64 bl = 500000, fr = 250000; mem.write(buf + 8, &bl, 8); mem.write(buf + 16, &fr, 8); mem.write(buf + 24, &fr, 8); mem.w32(buf + 56, 255); }
        else { mem.w32(buf + 8, 500000); mem.w32(buf + 12, 250000); mem.w32(buf + 16, 250000); mem.w32(buf + 36, 255); }
        return 0;
    }
    case 136: case 172: return 0;                // personality, prctl
    case 149: return -G_ENOSYS;                  // _sysctl
    case 270: return 0;                          // arm_fadvise64_64
    case 384: { // getrandom
        static std::mt19937 rng(42);
        for (u32 i = 0; i < a[1]; i++) { u8 b = (u8)rng(); mem.write(a[0] + i, &b, 1); }
        return (s32)a[1];
    }
    case 345: if (a[0]) mem.w32(a[0], 0); if (a[1]) mem.w32(a[1], 0); return 0; // getcpu
    case 21: case 22: case 52: case 88: case 128: case 129: return -G_EPERM;    // mount, umount, reboot, modules
    case 102: case 117: case 281: case 283: case 290: return -97;               // sockets / ipc: EAFNOSUPPORT
    case 0xF0002: return 0;                      // cacheflush
    case 0xF0005: set_tls(a[0]); return 0;       // set_tls
    case 0xF0006: return cur->tls;               // get_tls
    }
    log("Unimplemented syscall %u (args 0x%x 0x%x 0x%x) at 0x%08x", nr, a[0], a[1], a[2], cpu.current_pc());
    return -G_ENOSYS;
}

} // namespace gp2x
