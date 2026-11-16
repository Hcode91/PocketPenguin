// Basic runtime test: stdio, malloc, files, directories, time, threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <dirent.h>
#include <math.h>

static int counter = 0;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static void* worker(void* arg) {
    for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mx); counter++; pthread_mutex_unlock(&mx); if (i % 100 == 0) usleep(100); }
    return arg;
}

int main(int argc, char** argv) {
    printf("hello from guest, argc=%d argv0=%s\n", argc, argv[0]);
    char* p = malloc(10 << 20);
    memset(p, 1, 10 << 20);
    printf("malloc ok %d\n", p[12345]);
    free(p);
    FILE* f = fopen("test_out.txt", "w");
    fprintf(f, "line %d\n", 42);
    fclose(f);
    f = fopen("TEST_OUT.TXT", "r");   // case-insensitive like the GP2X SD card
    char buf[64] = {0};
    fgets(buf, sizeof buf, f);
    fclose(f);
    printf("file: %s", buf);
    unlink("test_out.txt");
    struct timeval a, b;
    gettimeofday(&a, 0);
    usleep(50000);
    gettimeofday(&b, 0);
    long us = (b.tv_sec - a.tv_sec) * 1000000 + (b.tv_usec - a.tv_usec);
    printf("slept %s\n", us >= 50000 && us < 70000 ? "ok" : "BAD");
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, worker, 0);
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    printf("threads counter=%d\n", counter);
    printf("float %.3f\n", sqrt(2.0) * sin(1.0));
    DIR* d = opendir(".");
    int n = 0;
    while (readdir(d)) n++;
    closedir(d);
    printf("dir entries %s\n", n > 2 ? "ok" : "BAD");
    printf("done\n");
    return 7;
}
