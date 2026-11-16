// PocketPenguin - user-mode ARM Linux runtime.
// Runs a GP2X / Wiz / Caanoo game's Linux ELF program directly on the ARM interpreter and
// emulates the kernel at the system-call level (like qemu-user). Hardware registers that games
// reach through /dev/mem, and device files such as /dev/fb0 or /dev/dsp, are provided by a
// Hardware implementation (see gp2x.h).
#pragma once
#include "arm926.h"
#include "fpa.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <deque>

namespace gp2x {

class Linux;

// Result of a file/device operation that has to wait.
enum : s32 { SYS_BLOCK = -100000 };

struct GuestStat {
    u64 dev = 1, ino = 1, rdev = 0;
    u32 mode = 0100644, nlink = 1, uid = 0, gid = 0;
    s64 size = 0;
    u32 blksize = 4096;
    u64 blocks = 0;
    u32 atime = 0, mtime = 0, ctime = 0;
};

// An open file description.
class File {
public:
    virtual ~File() {}
    std::string path;            // guest path
    int flags = 0;               // guest O_* flags
    // Return >=0 on success, -errno on error, or SYS_BLOCK after calling Linux::block_*.
    virtual s32 read(Linux& L, u32 buf, u32 len);
    virtual s32 write(Linux& L, u32 buf, u32 len);
    virtual s32 ioctl(Linux& L, u32 req, u32 arg);
    virtual s64 lseek(Linux& L, s64 off, int whence);
    virtual s32 pread(Linux& L, u32 buf, u32 len, s64 off);
    virtual s32 pwrite(Linux& L, u32 buf, u32 len, s64 off);
    // Map 'len' bytes at guest address 'va' (page aligned, already free).
    virtual s32 mmap(Linux& L, u32 va, u32 len, u32 prot, u32 flags, u64 off);
    virtual s32 fstat(Linux& L, GuestStat* st);
    virtual s32 ftruncate(Linux& L, s64 len);
    virtual s32 getdents(Linux& L, u32 buf, u32 len, bool is64);
    virtual int poll_mask(Linux& L);   // POLLIN=1 | POLLOUT=4
    virtual bool is_dir() const { return false; }
    virtual std::string host_path() const { return ""; }
};

// Device hooks provided by the emulated machine.
class Hardware {
public:
    virtual ~Hardware() {}
    virtual u8* ram_page(u32 pa) = 0;               // host pointer to physical RAM page, or null
    virtual bool is_io(u32 pa) = 0;                 // device register region?
    virtual u32 io_read(u32 pa, int size) = 0;
    virtual void io_write(u32 pa, u32 v, int size) = 0;
    // Open a device/proc path. Return null with *err = 0 if the path is not a device.
    virtual std::shared_ptr<File> open_device(Linux& L, const std::string& path, int flags, int* err) = 0;
    virtual std::string cpuinfo() = 0;
    virtual const char* machine() = 0;              // uname machine
};

// Guest address space (the MMU is off: virtual == "bus" address).
class GuestMem : public Bus {
public:
    enum Kind : u8 { NONE = 0, ANON = 1, PHYS = 2, IO = 3 };
    GuestMem();
    ~GuestMem();
    Hardware* hw = nullptr;
    ARM926* cpu = nullptr;
    bool fault = false;
    u32 fault_addr = 0;

    u8* page_ptr(u32 pa) override { return host[pa >> 12]; }
    u32 read32(u32 a) override;
    u16 read16(u32 a) override;
    u8 read8(u32 a) override;
    void write32(u32 a, u32 v) override;
    void write16(u32 a, u16 v) override;
    void write8(u32 a, u8 v) override;

    bool mapped(u32 a) const { return kind[a >> 12] != NONE; }
    bool range_free(u32 addr, u32 len) const;
    u32 find_free(u32 len, u32 hint, u32 lo = 0x40000000, u32 hi = 0xB0000000) const;
    void map_anon(u32 addr, u32 len);              // zero-filled; existing anon pages are kept
    void map_phys(u32 addr, u32 len, u32 pa);      // alias physical RAM / device registers
    void unmap(u32 addr, u32 len);
    bool move(u32 from, u32 to, u32 len);          // move page mappings (mremap)

    // Copy helpers; return false if any byte was unmapped.
    bool read(u32 va, void* dst, u32 len);
    bool write(u32 va, const void* src, u32 len);
    bool zero(u32 va, u32 len);
    std::string read_str(u32 va, u32 max = 4096);
    u32 r32(u32 va) { u32 v = 0; read(va, &v, 4); return v; }
    void w32(u32 va, u32 v) { write(va, &v, 4); }

private:
    std::vector<u8*> host;
    std::vector<u8> kind;
    std::vector<u32> phys;
    std::vector<u8*> free_pages;
    u8* alloc_page();
    void release(u32 page);
    void segv(u32 a);
    void flush();
};

struct SigAction { u32 handler = 0, flags = 0, restorer = 0; u64 mask = 0; };

struct Thread {
    int tid = 0;
    int pid = 100, ppid = 1;     // LinuxThreads threads are separate processes (no CLONE_THREAD)
    u32 r[16] = {0};
    u32 cpsr = 0x10;
    u32 tls = 0;
    FPAState fpa;                // emulated FPA floating-point registers
    enum State { RUN, BLOCKED, DEAD } state = RUN;
    // Blocking
    u64 wake_at = ~0ull;
    const void* wait_obj = nullptr;
    bool restart = false;        // on wake, re-execute the system call
    s32 timeout_result = 0;      // r0 when woken by the timeout (non-restart blocks)
    u32 futex_addr = 0, futex_bitset = 0;
    u32 clear_tid = 0;
    // Signals
    u64 sigmask = 0, pending = 0;
    bool sigsuspend = false;
    u64 suspend_saved_mask = 0;
    struct Frame { u32 r[16]; u32 cpsr; u64 mask; };
    std::vector<Frame> frames;
    // Timed retry (poll/select)
    u32 retry_pc = 0;
    u64 last_time_query = 0;     // instruction count at the previous clock read (spin detection)
    int time_spins = 0;
    u64 retry_deadline = 0;
};

class Linux {
public:
    explicit Linux(Hardware* hw);
    ~Linux();

    // ---- configuration ----
    std::string root;                    // host directory used as the guest "/"
    // Extra mount points: guest prefix (e.g. "/mnt/sd") -> host directory.
    std::vector<std::pair<std::string, std::string>> mounts;
    u64 cpu_hz = 200000000;              // virtual CPU clock (1 instruction = 1 cycle)
    std::string uname_release = "3.2.0";
    std::string exe_path;                // guest path of the running program
    std::function<void(const std::string&)> on_log;     // emulator diagnostics
    std::function<void(const std::string&)> on_output;  // guest stdout/stderr
    bool strace = false;
    // Called when a host file is mapped into guest memory (used to spot libopengles_lite.so).
    std::function<void(const std::string& host_path, u32 va, u32 len, u64 off)> on_file_map;
    u64 fpa_count = 0;                   // FPA instructions emulated (statistics)

    // Load a program (ELF or a launcher shell script) and set up the main thread.
    bool load(const std::string& guest_path, std::vector<std::string> argv, std::string* err);

    // Run until virtual time reaches 'target' cycles (or the program exits).
    void run_until(u64 target);
    u64 now() const;                     // current virtual time in cycles
    u64 usec() const { return now() * 1000000ull / cpu_hz; }
    u64 cycles_from_usec(u64 us) const { return us * cpu_hz / 1000000ull; }
    void skip_to(u64 t);                 // jump virtual time forward (idle loops)

    bool exited = false;
    int exit_code = 0;
    std::string exit_reason;

    ARM926 cpu;
    GuestMem mem;
    Hardware* hw;

    // ---- used by File implementations ----
    Thread* cur = nullptr;
    // Block the current thread. block_retry: re-run the syscall when woken (by wake(obj) or at
    // 'until'). block_wait: syscall returns the value given to wake_thread(), or timeout_result.
    s32 block_retry(const void* obj, u64 until = ~0ull);
    s32 block_wait(const void* obj, u64 until, s32 timeout_result);
    void wake(const void* obj);          // wake all threads waiting on obj
    void log(const char* fmt, ...);
    std::string to_host(const std::string& guest, bool follow = true, bool* found = nullptr);
    std::string abs_path(const std::string& guest) const;
    std::string cwd = "/";
    u64 epoch_usec = 0;                  // wall clock at virtual time 0
    void queue_signal(int tid, int sig); // tid 0 = process
    void stop(const std::string& why, int code) { kill_process(why, code); }

    int thread_count() const;
    u64 total_insns() const { return cpu.insn_count; }

private:
    std::vector<std::unique_ptr<Thread>> threads;
    size_t rr = 0;
    int next_tid = 100;
    u64 base_time = 0;                   // virtual time at start of the current slice
    u64 slice_cycles0 = 0;
    u64 pending_skip = 0;
    bool in_slice = false;
    u32 brk_start = 0, brk_cur = 0;
    std::map<int, std::shared_ptr<File>> fds;
    std::map<int, int> fd_cloexec;
    SigAction sigact[65];
    u32 umask_v = 022;
    bool oabi = false;                   // current syscall uses the old ABI
    std::string last_launch_dir;
    std::vector<std::string> env_extra;  // variables exported by a launcher script
    u32 sys_next_pc = 0;                 // return address of the current system call
    bool pending_exec = false;
    std::string exec_path;
    std::vector<std::string> exec_argv;
    void reset_process();

    static bool swi_entry(ARM926* cpu, u32 pc, u32 imm, void* user);
    static bool undef_entry(ARM926* cpu, u32 pc, u32 insn, void* user);
    bool syscall(u32 pc, u32 imm);
    s64 dispatch(u32 nr, u32* a, bool* noret);
    Thread* pick();
    void load_ctx(Thread* t);
    void save_ctx(Thread* t);
    void deliver_signals(Thread* t);
    void kill_process(const std::string& why, int code);
    void thread_exit(Thread* t, int code);
    void unblock(Thread* t, bool timeout);
    bool load_elf(const std::string& guest_path, u32* entry, u32* base, u32* phdr, u32* phnum,
                  std::string* interp, bool is_interp, std::string* err);
    void setup_kuser_page();
    int alloc_fd(int min = 0);
    std::shared_ptr<File> getf(int fd);
    s32 do_open(const std::string& guest, int flags, int mode);
    s32 do_stat(const std::string& guest, bool follow, GuestStat* st);
    void put_stat(u32 addr, const GuestStat& st, int kind);
    s32 do_mmap(u32 addr, u32 len, u32 prot, u32 flags, int fd, u64 off);
    s32 do_futex(u32 uaddr, int op, u32 val, u32 timeout, u32 uaddr2, u32 val3, bool time64);
    s32 do_poll(u32 fds_addr, u32 nfds, s64 timeout_ms);
    s32 do_select(int n, u32 rd, u32 wr, u32 ex, u32 tv);
    s32 do_clone(u32 flags, u32 sp, u32 ptid, u32 tls, u32 ctid);
    s32 do_sigaction(int sig, u32 act, u32 old, bool rt);
    void set_tls(u32 v);
    void time_query();                   // detect busy-waiting on the clock
    s32 do_execve(const std::string& path, u32 argv, u32 envp);
    std::string parse_launcher(const std::string& host, const std::string& guest_dir, std::vector<std::string>* argv);
};

} // namespace gp2x
