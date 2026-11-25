#!/usr/bin/env bash
# Compila o Rigs of Rods 2026.01 para Android arm64 (libRoR.so) com os patches Android e o iniciador ror_android.cpp.
# Chamado pelo workflow android-ogre. Precisa de: NDK_DIR, PREFIX (sysroot), CXX, CC, GITHUB_WORKSPACE.
set -e
W="$GITHUB_WORKSPACE"
cd "$W"
SUM="${GITHUB_STEP_SUMMARY:-/dev/null}"

[ -d ror-src ] || git clone -q --depth 1 --branch 2026.01 https://github.com/RigsOfRods/rigs-of-rods.git ror-src
rm -rf ois && git clone -q --depth 1 --branch v1.4 https://github.com/wgois/OIS.git ois
# a OIS trata "tudo que nao e Windows/Apple" como Linux (X11): no Android fica sem plataforma
sed -i 's|^#else //Probably Linux|#elif defined(__ANDROID__)\n#\tinclude <unistd.h>\n#else //Probably Linux|' ois/includes/OISPrereqs.h

# ---------------- patches Android no codigo do RoR ----------------
python3 - <<'PY'
import re, sys
root = "ror-src/source/main/"
def edit(path, fn):
    s = open(root + path, newline="").read()
    s2 = fn(s)
    open(root + path, "w", newline="").write(s2)
def must(cond, msg):
    if not cond:
        print("PATCH FALHOU:", msg); sys.exit(1)

# 1) ErrorUtils.cpp nao conhecia a plataforma Android
def p_err(s):
    s2 = re.sub(r"^#elif OGRE_PLATFORM == OGRE_PLATFORM_LINUX(\r?)$", r"#elif OGRE_PLATFORM == OGRE_PLATFORM_LINUX || OGRE_PLATFORM == OGRE_PLATFORM_ANDROID\1", s, flags=re.M)
    must(s2 != s, "ErrorUtils"); return s2
edit("utils/ErrorUtils.cpp", p_err)

# 2) o main() de PC fica guardado com outro nome (o iniciador Android o substitui)
def p_main(s):
    must("int main(int argc, char *argv[])" in s, "main"); return s.replace("int main(int argc, char *argv[])", "int ror_desktop_main(int argc, char *argv[])", 1)
edit("main.cpp", p_main)

# 3) RoR::Log nao pode quebrar quando ainda nao existe LogManager
def p_log(s):
    old = "void Log(const char* msg)\r\n{\r\n    Ogre::LogManager::getSingleton().logMessage(msg);"
    old2 = old.replace("\r\n", "\n")
    must(old in s or old2 in s, "Log")
    nl = "\r\n" if old in s else "\n"
    o = old if old in s else old2
    return s.replace(o, "void Log(const char* msg)" + nl + "{" + nl + "    if (!Ogre::LogManager::getSingletonPtr()) return;" + nl + "    Ogre::LogManager::getSingleton().logMessage(msg);", 1)
edit("Application.cpp", p_log)

# 4) AppContext.cpp: log, plugin GLES2 estatico e janela vinda do app
def p_app(s):
    inc = """#include "AppContext.h"
#include <RenderSystems/GLES2/OgreGLES2Plugin.h>
#if __has_include(<Plugins/STBICodec/OgreSTBICodec.h>)
#include <Plugins/STBICodec/OgreSTBICodec.h>
#else
namespace Ogre { class STBIImageCodec { public: static void startup(void); }; }
#endif
#if __has_include(<Plugins/ParticleFX/OgreParticleFXPlugin.h>)
#include <Plugins/ParticleFX/OgreParticleFXPlugin.h>
#else
#include <OgrePlugin.h>
#include <OgreParticleAffectorFactory.h>
#include <OgreParticleEmitterFactory.h>
namespace Ogre { class ParticleFXPlugin : public Plugin { public: ParticleFXPlugin(); const String& getName() const; void install(); void initialise(); void shutdown(); void uninstall(); protected: std::vector<ParticleEmitterFactory*> mEmitterFactories; std::vector<ParticleAffectorFactory*> mAffectorFactories; }; }
#endif
extern size_t g_ror_android_window;"""
    s = s.replace('#include "AppContext.h"', inc, 1)
    # LogManager: no Android quem cria e o Ogre::Root (senao o Root quebra com ponteiro nulo)
    s2 = re.sub(r"auto ogre_log_manager = OGRE_NEW Ogre::LogManager\(\);.*?rorlog->addListener\(App::GetConsole\(\)\);[^\n]*",
                "// Android: o LogManager e criado pelo Ogre::Root em SetUpRendering", s, flags=re.S)
    must(s2 != s, "SetUpLogging"); s = s2
    s2 = re.sub(r"LOG\(fmt::format\(\"\[RoR\|Startup\|Rendering\] Creating OGRE renderer Root object[^\n]*\n(\s*)m_ogre_root = new Ogre::Root\(\"\", cfg_filepath, log_filepath\);",
                r'm_ogre_root = new Ogre::Root("", cfg_filepath, log_filepath);\n\1Ogre::LogManager::getSingleton().getDefaultLog()->addListener(App::GetConsole());', s)
    must(s2 != s, "Root"); s = s2
    s2 = re.sub(r"// load OGRE plugins manually.*?(?=\s*// Load renderer configuration)",
                "// Android: plugins estaticos (GLES2, codecs PNG/JPG/TGA via STBI, particulas)\n    m_ogre_root->installPlugin(new Ogre::GLES2Plugin());\n    Ogre::STBIImageCodec::startup();\n    m_ogre_root->installPlugin(new Ogre::ParticleFXPlugin());\n", s, flags=re.S)
    must(s2 != s, "plugins"); s = s2
    s2 = s.replace("    // Validate rendering resolution", "    miscParams[\"externalWindowHandle\"] = Ogre::StringConverter::toString(g_ror_android_window);\n    // Validate rendering resolution", 1)
    must(s2 != s, "janela"); return s2
edit("AppContext.cpp", p_app)

# 5) ImGui: o material "ImGui/material" usa pipeline fixa, que o GLES2 nao tem -> shaders proprios
def p_imgui(s):
    code = '''    {   // Android (GLES2): sem pipeline fixa, entao o ImGui usa shaders proprios
        HighLevelGpuProgramManager& hm = HighLevelGpuProgramManager::getSingleton();
        if (!hm.resourceExists("ImGui/VS"))
        {
            HighLevelGpuProgramPtr vs = hm.createProgram("ImGui/VS", RGN_INTERNAL, "glsles", GPT_VERTEX_PROGRAM);
            vs->setSource(R"(attribute vec4 vertex;
attribute vec4 colour;
attribute vec2 uv0;
uniform mat4 worldViewProj;
varying vec4 vColour;
varying vec2 vUV;
void main() { gl_Position = worldViewProj * vertex; vColour = colour; vUV = uv0; })");
            HighLevelGpuProgramPtr fs = hm.createProgram("ImGui/FS", RGN_INTERNAL, "glsles", GPT_FRAGMENT_PROGRAM);
            // ROR_IMGUI_DEBUG (so no teste "+GUI"): tudo magenta e sem recorte, para separar problemas de geometria e de textura
            const bool dbg = getenv("ROR_IMGUI_DEBUG") != nullptr;
            fs->setSource(dbg ? R"(precision mediump float;
uniform sampler2D tex;
varying vec4 vColour;
varying vec2 vUV;
void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0) + 0.0 * vColour * texture2D(tex, vUV); })" : R"(precision mediump float;
uniform sampler2D tex;
varying vec4 vColour;
varying vec2 vUV;
void main() { gl_FragColor = vColour * texture2D(tex, vUV); })");
        }
        mPass->setVertexProgram("ImGui/VS");
        mPass->setFragmentProgram("ImGui/FS");
        mPass->getVertexProgramParameters()->setNamedAutoConstant("worldViewProj", GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
    }
'''
    s2 = re.sub(r"(\n)([ \t]*mMaterial->load\(\);)", lambda m: "\n" + code + m.group(2), s, count=1)
    must(s2 != s, "imgui material")
    s2 = s2.replace("#include <OgreMaterialManager.h>", "#include <OgreMaterialManager.h>\n#include <OgreHighLevelGpuProgramManager.h>", 1)
    s3 = s2.replace("rsys->setScissorTest(true, scissor.left", "rsys->setScissorTest(getenv(\"ROR_IMGUI_DEBUG\") == nullptr, scissor.left", 1)
    must(s3 != s2, "scissor")
    return s3
edit("gui/imgui/OgreImGuiOverlay.cpp", p_imgui)
print("patches Android aplicados")
PY

# ---------------- arquivos de versao e CMake de teste ----------------
mkdir -p gen
sed -e 's/@VERSION_YEAR@/2026/g' -e 's/@VERSION_MONTH@/01/g' -e 's/@VERSION_SUFFIX@//g' \
  ror-src/source/version_info/RoRVersionDef.h.in > gen/RoRVersionDef.h
sed -e 's/@VERSION_YEAR@/2026/g' -e 's/@VERSION_MONTH@/01/g' -e 's/@VERSION_SUFFIX@//g' \
  -e 's/@BUILD_DATE@/ci/g' -e 's/@BUILD_TIME@/ci/g' ror-src/source/version_info/RoRVersion.cpp.in > gen/RoRVersion.cpp

{ echo "set(RoR_INCS $W/gen $W/ror-src/source/version_info $W/ror-src/external/header_only $W/ror-src/external/angelscript_addons"
  for d in $(find ror-src/source/main -type d); do echo "  $W/$d"; done
  echo ")"
  echo "set(SYS_INCS $PREFIX/include"
  for d in $(find "$PREFIX/include" -mindepth 1 -maxdepth 2 -type d -not -path "*/rapidjson*"); do echo "  $d"; done
  echo "  $W/ois/includes"
  echo ")"
} > incs.cmake
mkdir -p ror-build-src
cp ogre-app/ror_android.cpp ror-build-src/ror_android.cpp
cat > ror-build-src/CMakeLists.txt <<'CMK'
cmake_minimum_required(VERSION 3.18)
project(RoRAndroid C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(${INCS_FILE})
file(GLOB_RECURSE srcs ${SRC}/source/main/*.cpp ${SRC}/external/angelscript_addons/*.cpp)
list(APPEND srcs ${GEN}/RoRVersion.cpp ${CMAKE_SOURCE_DIR}/ror_android.cpp)

file(GLOB ois_srcs ${OIS_DIR}/src/*.cpp)
add_library(OISCore STATIC ${ois_srcs})
target_include_directories(OISCore PUBLIC ${OIS_DIR}/includes)
target_compile_definitions(OISCore PUBLIC OIS_STATIC_BUILD)
target_compile_options(OISCore PRIVATE -fPIC -Wno-everything)

set(CPUF $ENV{NDK_DIR}/sources/android/cpufeatures)
add_library(cpufeatures STATIC ${CPUF}/cpu-features.c)
target_include_directories(cpufeatures PUBLIC ${CPUF})
target_compile_options(cpufeatures PRIVATE -fPIC -Wno-everything)

add_library(RoR SHARED ${srcs})
target_include_directories(RoR PRIVATE ${RoR_INCS})
foreach(d ${SYS_INCS})
  target_compile_options(RoR PRIVATE "SHELL:-idirafter ${d}")
endforeach()
target_compile_definitions(RoR PRIVATE USE_OPENAL USE_SOCKETW USE_ANGELSCRIPT AS_USE_NAMESPACE USE_CURL USE_CAELUM USE_PAGED
  _LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR _LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)
target_compile_options(RoR PRIVATE "SHELL:-include ${SRC}/source/main/pch.h" -fPIC -O1 -g0 -Wno-everything)
file(GLOB all_libs ${PREFIX}/lib/*.a)
file(GLOB ogre_plugin_libs ${PREFIX}/lib/OGRE/*.a)
target_link_options(RoR PRIVATE -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-Bsymbolic)
target_link_libraries(RoR PRIVATE OISCore cpufeatures -Wl,--start-group ${ogre_plugin_libs} ${all_libs} -Wl,--end-group android log EGL GLESv2 OpenSLES z dl)
CMK

cmake -S ror-build-src -B build-ror -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DSRC="$W/ror-src" -DGEN="$W/gen" -DPREFIX="$PREFIX" -DINCS_FILE="$W/incs.cmake" -DOIS_DIR="$W/ois" 2>&1 | tail -5
cmake --build build-ror -j"$(nproc)" -- -k -Otarget > build-ror.log 2>&1 || true

{
  echo "# libRoR.so (RoR com patches Android)"
  ls -la build-ror/libRoR.so 2>/dev/null || echo "libRoR.so NAO foi gerada"
  echo "Objetos compilados: $(find build-ror -name '*.o' | wc -l)"
  echo "## Erros de compilacao (arquivos)"
  grep -a -E "^[^ ]+\.cpp:[0-9]+:[0-9]+: (fatal )?error" build-ror.log | sed 's#^.*/ror-src/##; s#^.*/ror-build-src/##' | cut -d: -f1 | sort | uniq -c | sort -rn | head -15
  echo "## Primeiros erros"
  grep -a -E "error:" build-ror.log | grep -v "undefined symbol" | sed 's#^.*/ror-src/##; s#^.*/ror-build-src/##' | head -15 | cut -c1-260
  echo "## Simbolos indefinidos: $(grep -a -c 'undefined symbol' build-ror.log)"
  grep -a "undefined symbol" build-ror.log | sed 's/.*undefined symbol: //' | sort -u | head -30 | cut -c1-200
} | tee -a "$SUM"
test -f build-ror/libRoR.so
# FIM_DO_ARQUIVO
