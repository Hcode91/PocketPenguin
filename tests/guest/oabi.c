// Exercises the old ARM ABI (OABI) system call path used by GP2X-era toolchains:
// "swi 0x900000+nr" with arguments in r0-r6, and the packed OABI struct stat64.
#include <string.h>
#define OABI(nr, a, b, c) ({ int _a = (int)(a), _b = (int)(b), _c = (int)(c); register int r0 asm("r0") = _a; \
    register int r1 asm("r1") = _b; register int r2 asm("r2") = _c; asm volatile("swi %[n]" : "+r"(r0) : [n] "i"(0x900000 + nr), "r"(r1), "r"(r2) : "memory"); r0; })
static void out(const char* s) { OABI(4, 1, s, strlen(s)); }
static void outn(unsigned v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }
int main(void) {
    out("oabi write ok\n");
    int fd = OABI(5, "/proc/cpuinfo", 0, 0);
    char buf[64]; int n = OABI(3, fd, buf, 40); OABI(6, fd, 0, 0);
    out(n > 10 && !memcmp(buf, "Processor", 9) ? "oabi open/read ok\n" : "oabi open/read BAD\n");
    unsigned char st[96];
    memset(st, 0xEE, sizeof st);
    int r = OABI(195, "/proc/cpuinfo", st, 0);  // stat64
    fd = OABI(5, "oabi_tmp.bin", 0101 | 01000, 0644);  // O_WRONLY|O_CREAT|O_TRUNC
    OABI(4, fd, "0123456789", 10); OABI(6, fd, 0, 0);
    OABI(195, "oabi_tmp.bin", st, 0);
    unsigned size; memcpy(&size, st + 44, 4);       // OABI stat64: st_size at offset 44
    out("oabi stat64 size="); outn(size); out(r == 0 ? "\n" : " (stat failed)\n");
    OABI(10, "oabi_tmp.bin", 0, 0);
    unsigned brk0 = OABI(45, 0, 0, 0);
    out(brk0 ? "oabi brk ok\n" : "oabi brk BAD\n");
    OABI(1, 5, 0, 0);
    return 0;
}
