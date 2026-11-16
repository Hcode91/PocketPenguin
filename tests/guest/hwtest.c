// Hardware test written the way GP2X/Wiz homebrew talks to the machine.
// F100/F200: /dev/mem + MMSP2 registers (Rlyeh's "minimal library" style).
// Wiz/Caanoo: /dev/fb0 + /dev/GPIO + Pollux DPC vsync.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/soundcard.h>
#include <sys/utsname.h>

static volatile unsigned short* regs16;
static volatile unsigned long* regs32;

int main(int argc, char** argv) {
    struct utsname u; uname(&u);
    int pollux = strstr(u.machine, "v5") != NULL;
    printf("machine %s pollux=%d\n", u.machine, pollux);
    int mem = open("/dev/mem", O_RDWR);
    regs32 = mmap(0, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0xC0000000);
    regs16 = (volatile unsigned short*)regs32;
    unsigned short* fb;
    int fbfd = -1;
    if (!pollux) {
        // Upper memory framebuffer at 0x03500000 programmed directly into the MLC
        fb = mmap(0, 320 * 240 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0x03500000);
        regs16[0x28DA >> 1] = (2 << 9) | 0xAB;
        regs16[0x290C >> 1] = 640;
        unsigned long a = 0x03500000;
        regs16[0x290E >> 1] = a & 0xFFFF; regs16[0x2910 >> 1] = a >> 16;
        regs16[0x2912 >> 1] = a & 0xFFFF; regs16[0x2914 >> 1] = a >> 16;
    } else {
        fbfd = open("/dev/fb0", O_RDWR);
        struct fb_fix_screeninfo fix; ioctl(fbfd, FBIOGET_FSCREENINFO, &fix);
        struct fb_var_screeninfo var; ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
        printf("fb %s %dx%d bpp %d line %d\n", fix.id, var.xres, var.yres, var.bits_per_pixel, fix.line_length);
        fb = mmap(0, 320 * 240 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    }
    int gpio = open("/dev/GPIO", O_RDONLY);
    // Sound: 22050 Hz stereo 16-bit square wave
    int dsp = open("/dev/dsp", O_WRONLY);
    int fmt = AFMT_S16_LE, st = 1, rate = 22050, frag = (4 << 16) | 11;
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt); ioctl(dsp, SNDCTL_DSP_STEREO, &st); ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
    short snd[735 * 2];
    int phase = 0;
    unsigned int t0 = pollux ? regs32[0x1800 >> 2] : regs32[0x0A00 >> 2];
    for (int frame = 0; frame < 120; frame++) {
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 320; x++)
                fb[y * 320 + x] = ((x * 31 / 319) << 11) | ((y * 63 / 239) << 5) | ((frame * 2) & 31);
        // vsync
        if (!pollux) { while (!(regs16[0x1182 >> 1] & 0x10)); while (regs16[0x1182 >> 1] & 0x10); }
        else { while (!(regs16[0x308C >> 1] & (1 << 10))); regs16[0x308C >> 1] |= 1 << 10; }
        unsigned int keys;
        if (!pollux) {
            unsigned long v = (regs16[0x1198 >> 1] & 0xFF) | (regs16[0x1184 >> 1] & 0xFF00) | (regs16[0x1186 >> 1] << 16);
            keys = ~v & 0xFFFFFF;
        } else read(gpio, &keys, 4);
        if (keys) printf("frame %d keys %06x\n", frame, keys);
        for (int i = 0; i < 735; i++) { short v = (phase++ / 25) & 1 ? 6000 : -6000; snd[i * 2] = snd[i * 2 + 1] = v; }
        write(dsp, snd, sizeof snd);
    }
    unsigned int t1 = pollux ? regs32[0x1800 >> 2] : regs32[0x0A00 >> 2];
    printf("timer delta %u\n", t1 - t0);
    printf("done\n");
    return 0;
}
