// Teste minimo: OGRE (GLES2) desenhando numa Surface do Android, em thread propria.
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <algorithm>
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
#include <algorithm>
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

// ---------------------------------------------------------------------------
// Cena de teste leve: ceu em degrade, chao, estrada, lago ESTATICO (sem agua animada) e um "carro" de caixas.
// Sem RTShader: shaders GLSL ES 1.00 minimos criados por codigo, so com cor por vertice.
// ---------------------------------------------------------------------------
static std::atomic<float> g_dx(0.f), g_dy(0.f), g_zoom(1.f);

static const char* VS_SRC =
    "attribute vec4 vertex;\n"
    "attribute vec4 colour;\n"
    "uniform mat4 worldViewProj;\n"
    "varying lowp vec4 vCol;\n"
    "void main() {\n"
    "    gl_Position = worldViewProj * vertex;\n"
    "    vCol = colour;\n"
    "}\n";
static const char* FS_SRC =
    "precision mediump float;\n"
    "varying lowp vec4 vCol;\n"
    "void main() {\n"
    "    gl_FragColor = vCol;\n"
    "}\n";

static void makeMaterials() {
    Ogre::HighLevelGpuProgramManager& hm = Ogre::HighLevelGpuProgramManager::getSingleton();
    Ogre::HighLevelGpuProgramPtr vp = hm.createProgram("ror_vs", Ogre::RGN_DEFAULT, "glsles", Ogre::GPT_VERTEX_PROGRAM);
    vp->setSource(VS_SRC);
    vp->load();
    vp->getDefaultParameters()->setNamedAutoConstant("worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
    Ogre::HighLevelGpuProgramPtr fp = hm.createProgram("ror_fs", Ogre::RGN_DEFAULT, "glsles", Ogre::GPT_FRAGMENT_PROGRAM);
    fp->setSource(FS_SRC);
    fp->load();

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create("ror_main", Ogre::RGN_DEFAULT);
    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    p->setVertexProgram("ror_vs");
    p->setFragmentProgram("ror_fs");
    p->getVertexProgramParameters()->setNamedAutoConstant("worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
    p->setCullingMode(Ogre::CULL_NONE);
    p->setLightingEnabled(false);

    Ogre::MaterialPtr decal = mat->clone("ror_decal");
    decal->getTechnique(0)->getPass(0)->setDepthBias(4.0f, 2.0f);

    Ogre::MaterialPtr sky = mat->clone("ror_sky");
    Ogre::Pass* sp = sky->getTechnique(0)->getPass(0);
    sp->setDepthWriteEnabled(false);
    sp->setDepthCheckEnabled(false);
}

struct QuadBuilder {
    Ogre::ManualObject* mo;
    unsigned base;
    void quad(const Ogre::Vector3& a, const Ogre::Vector3& b, const Ogre::Vector3& c, const Ogre::Vector3& d,
              const Ogre::ColourValue& ca, const Ogre::ColourValue& cb,
              const Ogre::ColourValue& cc, const Ogre::ColourValue& cd) {
        mo->position(a); mo->colour(ca);
        mo->position(b); mo->colour(cb);
        mo->position(c); mo->colour(cc);
        mo->position(d); mo->colour(cd);
        mo->triangle(base, base + 1, base + 2);
        mo->triangle(base, base + 2, base + 3);
        base += 4;
    }
    void quad(const Ogre::Vector3& a, const Ogre::Vector3& b, const Ogre::Vector3& c, const Ogre::Vector3& d,
              const Ogre::ColourValue& col) {
        quad(a, b, c, d, col, col, col, col);
    }
};

static void addBox(QuadBuilder& q, float cx, float cy, float cz, float hx, float hy, float hz, const Ogre::ColourValue& c) {
    typedef Ogre::Vector3 V;
    V p[8] = { V(cx - hx, cy - hy, cz - hz), V(cx + hx, cy - hy, cz - hz), V(cx + hx, cy + hy, cz - hz), V(cx - hx, cy + hy, cz - hz),
               V(cx - hx, cy - hy, cz + hz), V(cx + hx, cy - hy, cz + hz), V(cx + hx, cy + hy, cz + hz), V(cx - hx, cy + hy, cz + hz) };
    // sombreamento "assado" por face (sem iluminacao em tempo real)
    Ogre::ColourValue top(c.r, c.g, c.b, 1.f), side(c.r * 0.8f, c.g * 0.8f, c.b * 0.8f, 1.f), dark(c.r * 0.6f, c.g * 0.6f, c.b * 0.6f, 1.f);
    q.quad(p[3], p[2], p[6], p[7], top);
    q.quad(p[0], p[1], p[2], p[3], side);
    q.quad(p[5], p[4], p[7], p[6], side);
    q.quad(p[4], p[0], p[3], p[7], dark);
    q.quad(p[1], p[5], p[6], p[2], dark);
}

static Ogre::SceneNode* buildScene(Ogre::SceneManager* sm) {
    typedef Ogre::Vector3 V;
    typedef Ogre::ColourValue C;
    makeMaterials();
    Ogre::SceneNode* rootNode = sm->getRootSceneNode();

    // chao + carro
    {
        Ogre::ManualObject* mo = sm->createManualObject("ground");
        mo->begin("ror_main", Ogre::RenderOperation::OT_TRIANGLE_LIST);
        QuadBuilder q = { mo, 0 };
        const float G = 3000.f;
        q.quad(V(-G, 0, -G), V(G, 0, -G), V(G, 0, G), V(-G, 0, G),
               C(0.42f, 0.50f, 0.28f), C(0.36f, 0.50f, 0.26f), C(0.34f, 0.52f, 0.24f), C(0.40f, 0.50f, 0.30f));
        addBox(q, 0.f, 0.75f, 0.f, 1.0f, 0.35f, 2.2f, C(0.85f, 0.12f, 0.10f));   // carroceria
        addBox(q, 0.f, 1.30f, -0.2f, 0.85f, 0.30f, 1.1f, C(0.55f, 0.65f, 0.75f)); // cabine
        const float wx[2] = { -1.05f, 1.05f }, wz[2] = { -1.4f, 1.4f };
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                addBox(q, wx[i], 0.35f, wz[j], 0.18f, 0.35f, 0.35f, C(0.10f, 0.10f, 0.10f)); // rodas
        mo->end();
        rootNode->createChildSceneNode()->attachObject(mo);
    }

    // estrada, faixas e lago estatico (com bias de profundidade para nao brigar com o chao)
    {
        Ogre::ManualObject* mo = sm->createManualObject("decals");
        mo->begin("ror_decal", Ogre::RenderOperation::OT_TRIANGLE_LIST);
        QuadBuilder q = { mo, 0 };
        q.quad(V(-3, 0, -3000), V(3, 0, -3000), V(3, 0, 3000), V(-3, 0, 3000), C(0.22f, 0.22f, 0.24f));
        for (int i = -100; i < 100; i++) {
            float z0 = i * 8.f;
            q.quad(V(-0.1f, 0, z0), V(0.1f, 0, z0), V(0.1f, 0, z0 + 3.f), V(-0.1f, 0, z0 + 3.f), C(0.9f, 0.9f, 0.82f));
        }
        q.quad(V(22, 0, -54), V(88, 0, -54), V(88, 0, 54), V(22, 0, 54), C(0.78f, 0.72f, 0.50f));   // areia
        q.quad(V(25, 0, -50), V(85, 0, -50), V(85, 0, 50), V(25, 0, 50),
               C(0.12f, 0.42f, 0.66f), C(0.12f, 0.42f, 0.66f), C(0.05f, 0.25f, 0.50f), C(0.05f, 0.25f, 0.50f)); // agua parada
        mo->end();
        rootNode->createChildSceneNode()->attachObject(mo);
    }

    // ceu: meia esfera com degrade horizonte -> zenite, que acompanha a camera
    Ogre::ManualObject* dome = sm->createManualObject("dome");
    dome->setRenderQueueGroup(Ogre::RENDER_QUEUE_SKIES_EARLY);
    dome->begin("ror_sky", Ogre::RenderOperation::OT_TRIANGLE_LIST);
    const int rings = 8, segs = 24;
    const float R = 5000.f, PI = 3.14159265f;
    const C horizon(0.78f, 0.88f, 0.98f), zenith(0.18f, 0.42f, 0.82f);
    for (int r = 0; r <= rings; r++) {
        float a = (PI / 2.f) * r / rings;
        float t = std::pow((float)r / rings, 0.7f);
        C col(horizon.r + (zenith.r - horizon.r) * t, horizon.g + (zenith.g - horizon.g) * t,
              horizon.b + (zenith.b - horizon.b) * t, 1.f);
        for (int s = 0; s <= segs; s++) {
            float b = 2.f * PI * s / segs;
            dome->position(R * std::cos(a) * std::cos(b), R * std::sin(a), R * std::cos(a) * std::sin(b));
            dome->colour(col);
        }
    }
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segs; s++) {
            unsigned i0 = r * (segs + 1) + s, i1 = i0 + 1, i2 = i0 + (segs + 1), i3 = i2 + 1;
            dome->triangle(i0, i2, i1);
            dome->triangle(i1, i2, i3);
        }
    }
    dome->end();
    Ogre::SceneNode* domeNode = rootNode->createChildSceneNode();
    domeNode->attachObject(dome);
    return domeNode;
}

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
        addLog("Passo 6c ok (Viewport)");

        addLog("Passo 7: criando shaders, materiais e geometria...");
        Ogre::SceneNode* domeNode = buildScene(sm);
        cam->setNearClipDistance(1.0f);
        cam->setFarClipDistance(8000.0f);
        cam->setAutoAspectRatio(true);
        vp->setBackgroundColour(Ogre::ColourValue(0.78f, 0.88f, 0.98f));
        addLog("Passo 7 ok. Renderizando...");

        float yaw = 35.f, pitch = 20.f, dist = 14.f;
        const Ogre::Vector3 target(0.f, 1.0f, 0.f);

        auto t0 = std::chrono::steady_clock::now();
        auto tl = t0;
        int frames = 0, last = 0;
        while (!g_stop) {
            yaw -= g_dx.exchange(0.f) * 0.25f;
            pitch += g_dy.exchange(0.f) * 0.25f;
            if (pitch < 3.f) pitch = 3.f;
            if (pitch > 88.f) pitch = 88.f;
            float z = g_zoom.exchange(1.f);
            if (z > 0.01f) dist = std::min(150.f, std::max(4.f, dist / z));
            const float k = 3.14159265f / 180.f;
            cam->setPosition(target + Ogre::Vector3(dist * std::cos(pitch * k) * std::sin(yaw * k),
                                                    dist * std::sin(pitch * k),
                                                    dist * std::cos(pitch * k) * std::cos(yaw * k)));
            cam->lookAt(target);
            domeNode->setPosition(cam->getPosition());
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
extern "C" JNIEXPORT void JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeTouch(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    g_dx = g_dx + dx;
    g_dy = g_dy + dy;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rigsofrods_mobile_MainActivity_nativeZoom(JNIEnv*, jclass, jfloat factor) {
    g_zoom = g_zoom * factor;
}

// FIM_DO_ARQUIVO
