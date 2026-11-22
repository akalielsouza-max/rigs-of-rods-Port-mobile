// Teste minimo: OGRE (GLES2) desenhando numa Surface do Android, em thread propria.
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <atomic>
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

static void addLog(const std::string& in) {
    std::string s;
    for (unsigned char c : in) s += (c < 128 && (c >= 32 || c == '\n')) ? (char)c : '?';
    __android_log_print(ANDROID_LOG_INFO, "RoRRender", "%s", s.c_str());
    std::lock_guard<std::mutex> lk(g_mx);
    g_log += s + "\n";
    if (g_log.size() > 16000) g_log.erase(0, g_log.size() - 12000);
}

struct Capture : public Ogre::LogListener {
    void messageLogged(const Ogre::String& m, Ogre::LogMessageLevel, bool, const Ogre::String&, bool&) override {
        addLog("[ogre] " + m);
    }
};
static Capture g_capture;

static void renderMain(ANativeWindow* win) {
    Ogre::LogManager* lm = nullptr;
    Ogre::Root* root = nullptr;
    try {
        lm = new Ogre::LogManager();
        Ogre::Log* l = lm->createLog("ror", true, false, true);
        l->addListener(&g_capture);

        addLog("Criando Ogre::Root...");
        root = new Ogre::Root("", "", "");
        addLog("Instalando plugin GLES2...");
        root->installPlugin(new Ogre::GLES2Plugin());
        const Ogre::RenderSystemList& rl = root->getAvailableRenderers();
        if (rl.empty()) throw std::runtime_error("nenhum RenderSystem disponivel");
        addLog("RenderSystem: " + rl[0]->getName());
        root->setRenderSystem(rl[0]);
        root->initialise(false);

        Ogre::NameValuePairList p;
        p["externalWindowHandle"] = std::to_string((size_t)win);
        addLog("Criando janela de render...");
        Ogre::RenderWindow* w = root->createRenderWindow("RoR", 0, 0, false, &p);
        addLog("Janela: " + std::to_string(w->getWidth()) + "x" + std::to_string(w->getHeight()));
        const GLubyte* r = glGetString(GL_RENDERER);
        const GLubyte* v = glGetString(GL_VERSION);
        addLog(std::string("GPU: ") + (r ? (const char*)r : "?") + " | GL: " + (v ? (const char*)v : "?"));

        Ogre::SceneManager* sm = root->createSceneManager();
        Ogre::Camera* cam = sm->createCamera("cam");
        Ogre::Viewport* vp = w->addViewport(cam);
        addLog("Cena criada. Renderizando...");

        auto t0 = std::chrono::steady_clock::now();
        auto tl = t0;
        int frames = 0, last = 0;
        while (!g_stop) {
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            vp->setBackgroundColour(Ogre::ColourValue(0.25f + 0.2f * std::sin(t), 0.55f, 0.9f - 0.2f * std::sin(t)));
            if (!root->renderOneFrame()) break;
            frames++;
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
        delete lm;
    } catch (...) {
    }
    addLog("OGRE encerrado.");
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeStart(JNIEnv* env, jclass, jobject surface) {
    if (g_thread.joinable()) return env->NewStringUTF("OGRE ja esta rodando");
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
