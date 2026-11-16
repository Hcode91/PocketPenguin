// JNI bridge between the Android frontend and the PocketPenguin core.
#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>
#include "gp2x.h"

using namespace gp2x;

static Machine* g_m = nullptr;
static std::mutex g_log_mx;
static std::string g_console;

static void append_console(const std::string& raw) {
    // Keep the log plain ASCII so it is always valid for NewStringUTF.
    std::string s;
    for (char c : raw) { unsigned char u = (unsigned char)c; s += (u >= 0x80 || (u < 0x20 && c != '\n' && c != '\t')) ? '?' : c; }
    std::lock_guard<std::mutex> lk(g_log_mx);
    g_console += s;
    if (g_console.size() > 32768) g_console.erase(0, g_console.size() - 32768);
}

static std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string r(p);
    env->ReleaseStringUTFChars(s, p);
    return r;
}

extern "C" {

// model: 0 = F100, 1 = F200, 2 = Wiz, 3 = Caanoo
JNIEXPORT jstring JNICALL
Java_com_hcode91_pocketpenguin_Native_init(JNIEnv* env, jclass, jint model, jstring jroot, jstring jsd, jstring jgame, jint mhz) {
    delete g_m;
    {
        std::lock_guard<std::mutex> lk(g_log_mx);
        g_console.clear();
    }
    Model m = model == 0 ? Model::F100 : model == 1 ? Model::F200 : model == 3 ? Model::CAANOO : Model::WIZ;
    g_m = new Machine(m);
    g_m->linux.on_log = [](const std::string& s) {
        __android_log_print(ANDROID_LOG_INFO, "PocketPenguin", "%s", s.c_str());
        append_console("[emu] " + s + "\n");
    };
    g_m->linux.on_output = [](const std::string& s) { append_console(s); };
    g_m->linux.mounts.push_back({"/mnt/sd", jstr(env, jsd)});
    std::string err;
    if (!g_m->start(jstr(env, jroot), jstr(env, jgame), mhz, &err)) {
        delete g_m;
        g_m = nullptr;
        return env->NewStringUTF(err.c_str());
    }
    return env->NewStringUTF("");
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_pocketpenguin_Native_runFrame(JNIEnv* env, jclass, jintArray pixels) {
    if (!g_m) return JNI_FALSE;
    g_m->run_frame();
    env->SetIntArrayRegion(pixels, 0, Machine::W * Machine::H, (const jint*)g_m->framebuffer());
    return g_m->running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpenguin_Native_setInput(JNIEnv*, jclass, jint buttons, jboolean touch, jint tx, jint ty) {
    if (!g_m) return;
    g_m->set_buttons((u32)buttons);
    g_m->set_touch(touch, tx, ty);
}

JNIEXPORT jint JNICALL
Java_com_hcode91_pocketpenguin_Native_readAudio(JNIEnv* env, jclass, jshortArray buf) {
    if (!g_m) return 0;
    jsize len = env->GetArrayLength(buf);
    jshort* p = env->GetShortArrayElements(buf, nullptr);
    size_t n = g_m->audio_read((s16*)p, (size_t)len / 2);
    env->ReleaseShortArrayElements(buf, p, 0);
    return (jint)n;
}

JNIEXPORT jstring JNICALL
Java_com_hcode91_pocketpenguin_Native_exitReason(JNIEnv* env, jclass) {
    std::string r = g_m ? g_m->linux.exit_reason : "";
    for (auto& ch : r) if ((unsigned char)ch >= 0x80) ch = '?';
    return env->NewStringUTF(r.c_str());
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpenguin_Native_shutdown(JNIEnv*, jclass) {
    delete g_m;
    g_m = nullptr;
}

JNIEXPORT jstring JNICALL
Java_com_hcode91_pocketpenguin_Native_console(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_log_mx);
    return env->NewStringUTF(g_console.c_str());
}

} // extern "C"
