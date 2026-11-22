// Teste minimo: OGRE (GLES2) desenhando numa Surface do Android, em thread propria.
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <Ogre.h>

#include <OgrePlugin.h>
#if __has_include(<RenderSystems/GLES2/OgreGLES2Plugin.h>)
#include <RenderSystems/GLES2/OgreGLES2Plugin.h>
#else
namespace Ogre {
    class GLES2RenderSystem;
    class GLES2Plugin : public Plugin {
    public:
        GLES2Plugin();
        const String& getName() const;
        void install();
        void initialise();
        void shutdown();
        void uninstall();
    protected:
        GLES2RenderSystem* mRenderSystem;
    };
}
#endif

static std::mutex g_mx;
static std::string g_log;
static std::atomic<bool> g_stop(false);
static std::thread g_thread;
static ANativeWindow* g_win = nullptr;
static char g_logPath[512] = {0};

// ---- registra em arquivo o que o app estava fazendo quando um crash nativo acontece ----
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
    int fd = open(g_logPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
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
                             lib ? lib + 1 : "?",
                             (unsigned long)((uintptr_t)b.pcs[i] - (uintptr_t)d.dli_fbase));
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

static void addLog(const std::string& in) {
    std::string s;
    for (unsigned char c : in) s += (c < 128 && (c >= 32 || c == '\n')) ? (char)c : '?';
    __android_log_print(ANDROID_LOG_INFO, "RoRRender", "%s", s.c_str());
    std::lock_guard<std::mutex> lk(g_mx);
    g_log += s + "\n";
    if (g_logPath[0]) {
        FILE* f = fopen(g_logPath, "a");
        if (f) { fputs(s.c_str(), f); fputc('\n', f); fclose(f); }
    }
    if (g_log.size() > 16000) g_log.erase(0, g_log.size() - 12000);
}

struct Capture : public Ogre::LogListener {
    void messageLogged(const Ogre::String& m, Ogre::LogMessageLevel, bool, const Ogre::String&, bool&) override {
        addLog("[ogre] " + m);
    }
};
static Capture g_capture;

static void renderMain(ANativeWindow* win) {
    Ogre::Root* root = nullptr;
    try {
        // OGRE 1.11 no Android falha (ponteiro nulo) se o LogManager for criado antes do Root:
        // por isso quem cria o LogManager e o proprio Root.
        addLog("Passo 1: preparando (o LogManager sera criado pelo Root)");

        addLog("Passo 2: criando Ogre::Root...");
        root = new Ogre::Root("", "", std::string(g_logPath) + ".ogre.log");
        Ogre::LogManager::getSingleton().getDefaultLog()->addListener(&g_capture);
        addLog("Passo 2 ok. Passo 3: instalando plugin GLES2...");
        root->installPlugin(new Ogre::GLES2Plugin());
        addLog("Passo 3 ok");
        const Ogre::RenderSystemList& rl = root->getAvailableRenderers();
        if (rl.empty()) throw std::runtime_error("nenhum RenderSystem disponivel");
        addLog("RenderSystem: " + rl[0]->getName());
        root->setRenderSystem(rl[0]);
        addLog("Passo 4: root->initialise...");
        root->initialise(false);
        addLog("Passo 4 ok");

        Ogre::NameValuePairList p;
        p["externalWindowHandle"] = std::to_string((size_t)win);
        addLog("Passo 5: criando janela de render (EGL)...");
        Ogre::RenderWindow* w = root->createRenderWindow("RoR", 0, 0, false, &p);
        addLog("Janela: " + std::to_string(w->getWidth()) + "x" + std::to_string(w->getHeight()));
        const GLubyte* r = glGetString(GL_RENDERER);
        const GLubyte* v = glGetString(GL_VERSION);
        addLog(std::string("GPU: ") + (r ? (const char*)r : "?") + " | GL: " + (v ? (const char*)v : "?"));

        addLog("Passo 5 ok. Passo 6: criando cena...");
        Ogre::SceneManager* sm = root->createSceneManager();
        addLog("Passo 6a ok (SceneManager)");
        Ogre::Camera* cam = sm->createCamera("cam");
        addLog("Passo 6b ok (Camera)");
        Ogre::Viewport* vp = w->addViewport(cam);
        addLog("Passo 6c ok (Viewport). Renderizando...");

        auto t0 = std::chrono::steady_clock::now();
        auto tl = t0;
        int frames = 0, last = 0;
        while (!g_stop) {
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            vp->setBackgroundColour(Ogre::ColourValue(0.25f + 0.2f * std::sin(t), 0.55f, 0.9f - 0.2f * std::sin(t)));
            if (frames == 0) addLog("Quadro 1: chamando renderOneFrame...");
            if (!root->renderOneFrame()) break;
            frames++;
            if (frames == 1) addLog("Quadro 1 ok");
            if (frames == 30) addLog("FRAMES_OK: o OGRE desenhou 30 quadros");
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
    try {
        addLog("Encerrando OGRE...");
        delete root;
    } catch (...) {
    }
    addLog("OGRE encerrado.");
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeStart(JNIEnv* env, jclass, jobject surface, jstring logPath) {
    if (g_thread.joinable()) return env->NewStringUTF("OGRE ja esta rodando");
    if (logPath) {
        const char* p = env->GetStringUTFChars(logPath, nullptr);
        if (p) {
            strncpy(g_logPath, p, sizeof(g_logPath) - 1);
            env->ReleaseStringUTFChars(logPath, p);
            FILE* f = fopen(g_logPath, "w");
            if (f) fclose(f);
        }
    }
    installCrashHandlers();
    g_win = ANativeWindow_fromSurface(env, surface);
    if (!g_win) return env->NewStringUTF("ANativeWindow_fromSurface falhou");
    { std::lock_guard<std::mutex> lk(g_mx); g_log.clear(); }
    g_stop = false;
    g_thread = std::thread(renderMain, g_win);
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT void JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeStop(JNIEnv*, jclass) {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    if (g_win) { ANativeWindow_release(g_win); g_win = nullptr; }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeGetLog(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mx);
    return env->NewStringUTF(g_log.c_str());
}
// FIM_DO_ARQUIVO
