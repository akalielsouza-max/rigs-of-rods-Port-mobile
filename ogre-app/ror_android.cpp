// Iniciador do Rigs of Rods no Android (etapa 2, passo A): roda os passos de inicializacao do RoR
// (paths, log, recursos, renderizador) numa Surface do app e desenha quadros com a janela do RoR.
#include "Application.h"
#include "AppContext.h"
#include "Console.h"
#include "PlatformUtils.h"

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unwind.h>

size_t g_ror_android_window = 0; // usado pelo AppContext::SetUpRendering (patch Android)

static std::mutex g_mx;
static std::string g_log;
static char g_path[512] = {0};
static std::atomic<bool> g_stop(false);
static std::thread g_thread;
static ANativeWindow* g_win = nullptr;

static void addLog(const std::string& in) {
    std::string s;
    for (unsigned char c : in) s += (c < 128 && (c >= 32 || c == '\n')) ? (char)c : '?';
    __android_log_print(ANDROID_LOG_INFO, "RoRAndroid", "%s", s.c_str());
    std::lock_guard<std::mutex> lk(g_mx);
    g_log += s + "\n";
    if (g_log.size() > 16000) g_log.erase(0, g_log.size() - 12000);
    if (g_path[0]) {
        FILE* f = fopen(g_path, "a");
        if (f) { fputs(s.c_str(), f); fputc('\n', f); fclose(f); }
    }
}

// ---- crash nativo: grava o rastro em arquivo e sai ----
struct BT { int n; void* pcs[40]; };
static _Unwind_Reason_Code unwindCb(struct _Unwind_Context* c, void* arg) {
    BT* b = (BT*)arg;
    if (b->n < 40) b->pcs[b->n++] = (void*)_Unwind_GetIP(c);
    return _URC_NO_REASON;
}
static volatile int g_inCrash = 0;
static void crashHandler(int sig, siginfo_t* si, void*) {
    if (g_inCrash) _exit(139);
    g_inCrash = 1;
    int fd = open(g_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        char buf[600];
        int n = snprintf(buf, sizeof buf, "CRASH: sinal=%d endereco=%p\n", sig, si ? si->si_addr : nullptr);
        write(fd, buf, n);
        BT b; b.n = 0;
        _Unwind_Backtrace(unwindCb, &b);
        for (int i = 0; i < b.n; i++) {
            Dl_info d;
            if (dladdr(b.pcs[i], &d)) {
                const char* lib = d.dli_fname ? strrchr(d.dli_fname, '/') : nullptr;
                n = snprintf(buf, sizeof buf, "  #%d %s (%s) rel=0x%lx\n", i, d.dli_sname ? d.dli_sname : "?",
                             lib ? lib + 1 : "?", (unsigned long)((uintptr_t)b.pcs[i] - (uintptr_t)d.dli_fbase));
            } else {
                n = snprintf(buf, sizeof buf, "  #%d %p\n", i, b.pcs[i]);
            }
            write(fd, buf, n);
        }
        close(fd);
    }
    _exit(139);
}
static void installCrashHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}

static void rorMain(std::string base) {
    using namespace RoR;
    try {
        addLog("R1: criando as variaveis de configuracao do RoR (cVarSetupBuiltins)...");
        App::GetConsole()->cVarSetupBuiltins();
        App::GetAppContext()->SetUpThreads();
        addLog("R1 ok");

        addLog("R2: pastas (base=" + base + ")");
        CreateFolder(base);
        App::sys_process_dir->setStr(base);
        std::string user = PathCombine(base, "user");
        CreateFolder(user);
        App::sys_user_dir->setStr(user);
        App::sys_config_dir    ->setStr(PathCombine(user, "config"));
        App::sys_cache_dir     ->setStr(PathCombine(user, "cache"));
        App::sys_thumbnails_dir->setStr(PathCombine(user, "thumbnails"));
        App::sys_savegames_dir ->setStr(PathCombine(user, "savegames"));
        App::sys_screenshot_dir->setStr(PathCombine(user, "screenshots"));
        App::sys_scripts_dir   ->setStr(PathCombine(user, "scripts"));
        App::sys_projects_dir  ->setStr(PathCombine(user, "projects"));
        App::sys_repo_attachments_dir->setStr(PathCombine(user, "repo_attachments"));
        App::GetAppContext()->SetUpLogging();
        if (!App::GetAppContext()->SetUpResourcesDir()) throw std::runtime_error("SetUpResourcesDir falhou (pasta resources nao encontrada)");
        CreateFolder(App::sys_config_dir->getStr());
        addLog("R2 ok. resources=" + std::string(App::sys_resources_dir->getStr()));

        addLog("R3: SetUpRendering (Root + plugin GLES2 + janela)...");
        if (!App::GetAppContext()->SetUpRendering()) throw std::runtime_error("SetUpRendering falhou");
        Ogre::RenderWindow* w = App::GetAppContext()->GetRenderWindow();
        addLog("R3 ok. Janela: " + std::to_string(w->getWidth()) + "x" + std::to_string(w->getHeight()));

        addLog("R4: desenhando quadros pela janela do RoR...");
        Ogre::Root* root = App::GetAppContext()->GetOgreRoot();
        Ogre::Viewport* vp = App::GetAppContext()->GetViewport();
        auto t0 = std::chrono::steady_clock::now();
        auto tl = t0;
        int frames = 0, last = 0;
        while (!g_stop) {
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            vp->setBackgroundColour(Ogre::ColourValue(0.2f + 0.15f * std::sin(t), 0.4f, 0.7f - 0.15f * std::sin(t)));
            if (!root->renderOneFrame()) break;
            frames++;
            if (frames == 30) addLog("FRAMES_OK: a janela do RoR desenhou 30 quadros");
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(now - tl).count() >= 2.0f) {
                float fps = (frames - last) / std::chrono::duration<float>(now - tl).count();
                addLog("FPS: " + std::to_string((int)fps) + " (quadros: " + std::to_string(frames) + ")");
                last = frames;
                tl = now;
            }
        }
    } catch (Ogre::Exception& e) {
        addLog("ERRO OGRE: " + e.getFullDescription());
    } catch (std::exception& e) {
        addLog(std::string("ERRO: ") + e.what());
    } catch (...) {
        addLog("ERRO desconhecido");
    }
    addLog("Iniciador do RoR encerrado.");
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeRorStart(JNIEnv* env, jclass, jobject surface, jstring baseDir, jstring logPath) {
    if (g_thread.joinable()) return env->NewStringUTF("O RoR ja esta rodando");
    const char* lp = env->GetStringUTFChars(logPath, nullptr);
    strncpy(g_path, lp, sizeof(g_path) - 1);
    env->ReleaseStringUTFChars(logPath, lp);
    FILE* f = fopen(g_path, "w");
    if (f) fclose(f);
    installCrashHandlers();
    const char* bd = env->GetStringUTFChars(baseDir, nullptr);
    std::string base = bd;
    env->ReleaseStringUTFChars(baseDir, bd);
    g_win = ANativeWindow_fromSurface(env, surface);
    if (!g_win) return env->NewStringUTF("ANativeWindow_fromSurface falhou");
    g_ror_android_window = (size_t)g_win;
    { std::lock_guard<std::mutex> lk(g_mx); g_log.clear(); }
    g_stop = false;
    g_thread = std::thread(rorMain, base);
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT void JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeRorStop(JNIEnv*, jclass) {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    if (g_win) { ANativeWindow_release(g_win); g_win = nullptr; }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeRorGetLog(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mx);
    return env->NewStringUTF(g_log.c_str());
}
// FIM_DO_ARQUIVO
