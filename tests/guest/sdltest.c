// SDL 1.2 test game: video (fbcon), audio thread (OSS), timers, joystick.
#include <SDL.h>
#include <stdio.h>
#include <math.h>
static int samples = 0;
static void audio_cb(void* u, Uint8* s, int len) {
    Sint16* p = (Sint16*)s;
    for (int i = 0; i < len / 4; i++) { Sint16 v = (Sint16)(sin(samples++ * 2 * 3.14159 * 440 / 22050) * 8000); p[i * 2] = p[i * 2 + 1] = v; }
}
int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) < 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    char drv[32];
    SDL_Surface* scr = SDL_SetVideoMode(320, 240, 16, SDL_SWSURFACE);
    if (!scr) { printf("SetVideoMode: %s\n", SDL_GetError()); return 2; }
    printf("video driver %s, %dx%d %d bpp\n", SDL_VideoDriverName(drv, 32), scr->w, scr->h, scr->format->BitsPerPixel);
    SDL_AudioSpec want = {0};
    want.freq = 22050; want.format = AUDIO_S16; want.channels = 2; want.samples = 512; want.callback = audio_cb;
    if (SDL_OpenAudio(&want, NULL) < 0) printf("OpenAudio: %s\n", SDL_GetError()); else { SDL_PauseAudio(0); printf("audio ok\n"); }
    printf("joysticks: %d\n", SDL_NumJoysticks());
    if (SDL_NumJoysticks() > 0) SDL_JoystickOpen(0);
    Uint32 start = SDL_GetTicks();
    int frames = 0, x = 0;
    while (SDL_GetTicks() - start < 3000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_JOYBUTTONDOWN) printf("joy button %d\n", e.jbutton.button);
            if (e.type == SDL_JOYAXISMOTION && e.jaxis.value) printf("joy axis %d = %d\n", e.jaxis.axis, e.jaxis.value);
            if (e.type == SDL_KEYDOWN) printf("key %d\n", e.key.keysym.sym);
        }
        SDL_FillRect(scr, NULL, SDL_MapRGB(scr->format, 0, 0, 64));
        SDL_Rect r = {x % 280, 100, 40, 40};
        SDL_FillRect(scr, &r, SDL_MapRGB(scr->format, 255, 200, 0));
        SDL_Flip(scr);
        x += 2; frames++;
        SDL_Delay(16);
    }
    printf("frames %d in %u ms, audio samples %d\n", frames, SDL_GetTicks() - start, samples);
    SDL_Quit();
    return 0;
}
