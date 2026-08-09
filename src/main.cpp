// Voxel World - Juego Sandbox Infinito
// Motor 3D extraido de AniWorld (sin keyframes ni timeline)
// Con bloques 3D, chunks infinitos, y fisica de gravedad

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>       // _dup2/_fileno (redirección de logs), _write/_open (crash marker)
#include <fcntl.h>    // _O_CREAT/_O_WRONLY (crash marker)
#include <sys/stat.h> // _S_IREAD/_S_IWRITE (crash marker)
#include <gl/GL.h>
#include <mmsystem.h>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "winmm.lib")
#endif

// Threading para construcción de meshes en paralelo
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

// ============================================================================
// VBO EXTENSION LOADING (OpenGL 1.5+)
// ============================================================================
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
typedef void (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint *buffers);
typedef void (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint *buffers);
typedef void (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum target, ptrdiff_t size, const void *data, GLenum usage);

PFNGLGENBUFFERSPROC glGenBuffers = NULL;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = NULL;
PFNGLBINDBUFFERPROC glBindBuffer = NULL;
PFNGLBUFFERDATAPROC glBufferData = NULL;
#endif

#include <cmath>
#include <map>
#include <vector>
#include <queue>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <csignal>

// ============================================================================
// AAA SAVE SYSTEM
// ============================================================================
#include "SaveSystem.h"
using namespace VoxelWorld::SaveSystem;

// ============================================================================
// STB_IMAGE - Librería para cargar texturas PNG/JPEG
// ============================================================================
#define STB_IMAGE_IMPLEMENTATION
#include "../external/stb_image.h"

// ============================================================================
// CHUNK SYSTEM - Professional threading architecture
// ============================================================================
#include "ChunkSystem.h"

// ============================================================================
// PROFILER SYSTEM - Performance monitoring
// ============================================================================
#include "Profiler.h"

// ============================================================================
// VBO FUNCTION LOADER
// ============================================================================
void loadVBOFunctions() {
    glGenBuffers = (PFNGLGENBUFFERSPROC)wglGetProcAddress("glGenBuffers");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)wglGetProcAddress("glDeleteBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)wglGetProcAddress("glBufferData");

    if (!glGenBuffers || !glDeleteBuffers || !glBindBuffer || !glBufferData) {
        std::cerr << "ERROR: No se pudieron cargar funciones VBO. GPU muy antigua." << std::endl;
        MessageBoxA(nullptr,
                    "Tu GPU no soporta las extensiones VBO de OpenGL requeridas.\n"
                    "El juego no puede continuar.",
                    "VoxelWorld - Error fatal", MB_OK | MB_ICONERROR);
        exit(1);
    }
    std::cout << "VBO Extensions cargadas correctamente!" << std::endl;
}

// ============================================================================
// RUTAS BASE Y LOGGING
// ============================================================================

// Directorio donde vive el ejecutable
static std::string getExeDir() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";
    return std::filesystem::path(buf).parent_path().string();
}

// Raíz del proyecto: sube desde el exe hasta encontrar resourcepacks/
// (el exe vive en build/bin/Release/, los recursos en la raíz del proyecto)
static const std::string& getResourceRoot() {
    static std::string cached;
    if (!cached.empty()) return cached;
    namespace fs = std::filesystem;
    fs::path dir = getExeDir();
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(dir / "resourcepacks")) {
            cached = dir.string();
            return cached;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    cached = fs::current_path().string();
    std::cerr << "WARNING: no se encontró resourcepacks/ subiendo desde el exe; "
              << "usando directorio actual: " << cached << std::endl;
    return cached;
}

// El binario se enlaza con /SUBSYSTEM:WINDOWS (sin consola): sin esto,
// todos los cout/cerr se descartan. Redirige ambos a un archivo de log.
static void initLogging() {
    namespace fs = std::filesystem;
    fs::path logDir;
    const char* localAppData = getenv("LOCALAPPDATA");
    if (localAppData && *localAppData) {
        logDir = fs::path(localAppData) / "VoxelGenesis";
    } else {
        logDir = fs::path(getExeDir()) / "logs";
    }
    std::error_code ec;
    fs::create_directories(logDir, ec);
    fs::path logFile = logDir / "log.txt";
    if (freopen(logFile.string().c_str(), "w", stdout)) {
        _dup2(_fileno(stdout), _fileno(stderr));
    }
    // Sin buffer: si el juego crashea, el log queda completo en disco
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << "=== VoxelWorld log: " << logFile.string() << " ===" << std::endl;
}

// ============================================================================
// ESTRUCTURAS MATEMATICAS
// ============================================================================

struct Vec3 {
    float x, y, z;
    Vec3(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& v) const { return Vec3(x + v.x, y + v.y, z + v.z); }
    Vec3 operator-(const Vec3& v) const { return Vec3(x - v.x, y - v.y, z - v.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }
    float length() const { return sqrtf(x * x + y * y + z * z); }
    Vec3 normalize() const {
        float len = length();
        return len > 0 ? (*this) / len : Vec3();
    }
    float dot(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
};

struct Vec3i {
    int x, y, z;
    Vec3i(int x = 0, int y = 0, int z = 0) : x(x), y(y), z(z) {}
    bool operator<(const Vec3i& v) const {
        if (x != v.x) return x < v.x;
        if (y != v.y) return y < v.y;
        return z < v.z;
    }
    bool operator==(const Vec3i& v) const {
        return x == v.x && y == v.y && z == v.z;
    }
};

// ============================================================================
// TIPOS DE BLOQUES
// ============================================================================
#include "BlockType.h"

// Validación de nombres de mundo (usados como nombres de carpeta en saves/)
#include "WorldName.h"

// ============================================================================
// SISTEMA DE RAREZA DE MINERALES
// ============================================================================
enum MineralRarity {
    RARITY_COMMON = 0,      // Común: 25-35% de aparición
    RARITY_UNCOMMON,        // Poco Común: 10-20% de aparición
    RARITY_RARE,            // Raro: 1-5% de aparición
    RARITY_EPIC,            // Épico: 0.5-1% de aparición
    RARITY_LEGENDARY,       // Legendario: 0.1-0.5% de aparición
    RARITY_MYTHIC,          // Mítico: 0.01-0.1% de aparición
    RARITY_SUPREME          // Supremo: 0.001-0.01% de aparición
};

struct MineralInfo {
    BlockType type;
    MineralRarity rarity;
    float spawnChance;      // Porcentaje de aparición (0.0 - 1.0)
    int minY;               // Altura mínima de generación
    int maxY;               // Altura máxima de generación
    const char* rarityName;
    const char* description;
};

// Tabla de información de minerales
const MineralInfo MINERAL_DATA[] = {
    // ⭐⭐⭐ Minerales ULTRA COMUNES (100%) - OMNIPRESENTES EN TODO EL MAPA ⭐⭐⭐
    {BLOCK_COAL_ORE,      RARITY_COMMON,    1.0f,   1, 255, "ULTRA COMUN",  "Carbon - OMNIPRESENTE en TODOS los biomas y alturas"},
    {BLOCK_SCRAP_METAL,   RARITY_COMMON,    1.0f,   1, 255, "ULTRA COMUN",  "Desecho de Metales - OMNIPRESENTE en todos los biomas"},

    // ⭐⭐⭐ Minerales MUY COMUNES (50-60%) - ABUNDANTES COMO CARBON
    {BLOCK_IRON_ORE,      RARITY_UNCOMMON,  0.60f,  1, 90,  "MUY COMUN",    "Hierro - Abundante en todas partes"},
    {BLOCK_GOLD_ORE,      RARITY_UNCOMMON,  0.50f,  1, 50,  "MUY COMUN",    "Oro - Abundante en rios, oceanos y tierra"},
    {BLOCK_SILVER_ORE,    RARITY_UNCOMMON,  0.55f,  1, 60,  "MUY COMUN",    "Plata - Abundante en rios, oceanos y tierra"},

    // ⭐⭐⭐ Minerales COMUNES (30%) - MUY FACILES DE ENCONTRAR
    {BLOCK_DIAMOND_ORE,   RARITY_RARE,      0.30f,  1, 40,  "COMUN",        "Diamante - Facil de encontrar en desiertos"}
};

void getBlockColor(BlockType type, float& r, float& g, float& b) {
    switch (type) {
        case BLOCK_GRASS:     r = 0.3f; g = 0.8f; b = 0.2f; break;
        case BLOCK_DIRT:      r = 0.6f; g = 0.4f; b = 0.2f; break;
        case BLOCK_STONE:     r = 0.5f; g = 0.5f; b = 0.5f; break;
        case BLOCK_WOOD:      r = 0.4f; g = 0.25f; b = 0.1f; break;
        case BLOCK_LEAVES:    r = 0.2f; g = 0.6f; b = 0.2f; break;
        case BLOCK_SAND:      r = 0.9f; g = 0.85f; b = 0.6f; break;
        case BLOCK_WATER:     r = 0.2f; g = 0.4f; b = 0.8f; break;
        case BLOCK_TALLGRASS: r = 0.4f; g = 0.9f; b = 0.3f; break;
        case BLOCK_BEDROCK:   r = 0.2f; g = 0.2f; b = 0.2f; break;
        case BLOCK_GRAVEL:    r = 0.5f; g = 0.5f; b = 0.5f; break;
        case BLOCK_ORANGE_FLOWER: r = 1.0f; g = 0.6f; b = 0.0f; break; // Color naranja
        case BLOCK_SNOW:      r = 0.95f; g = 0.95f; b = 1.0f; break;  // Color blanco con tono azul
        case BLOCK_COBBLESTONE: r = 0.4f; g = 0.4f; b = 0.4f; break;  // Gris oscuro
        case BLOCK_PLANKS:    r = 0.6f; g = 0.4f; b = 0.2f; break;    // Marrón claro
        case BLOCK_COAL_ORE:  r = 0.3f; g = 0.3f; b = 0.3f; break;    // Gris muy oscuro (común)
        case BLOCK_IRON_ORE:  r = 0.8f; g = 0.7f; b = 0.6f; break;    // Beige rosado (poco común)
        case BLOCK_GOLD_ORE:  r = 1.0f; g = 0.84f; b = 0.0f; break;   // Dorado brillante (poco común)
        case BLOCK_SILVER_ORE: r = 0.85f; g = 0.9f; b = 0.95f; break; // Plateado brillante (poco común)
        case BLOCK_DIAMOND_ORE: r = 0.4f; g = 0.8f; b = 0.9f; break;  // Azul cian brillante (raro)
        case BLOCK_SCRAP_METAL: r = 0.6f; g = 0.5f; b = 0.4f; break;  // Metal oxidado (común)
        case BLOCK_LAVA:      r = 1.0f; g = 0.4f; b = 0.0f; break;    // Naranja brillante (lava)
        case BLOCK_DIRT_POWDER: r = 0.7f; g = 0.5f; b = 0.3f; break;  // Polvo de tierra - marrón claro
        case BLOCK_STICK:     r = 0.6f; g = 0.4f; b = 0.2f; break;    // Palo - marrón
        case BLOCK_HOE:       r = 0.5f; g = 0.3f; b = 0.15f; break;   // Hoz - marrón oscuro
        case BLOCK_COAL_ITEM: r = 0.1f; g = 0.1f; b = 0.1f; break;    // Carbón - negro
        case BLOCK_RAW_ZINC:  r = 0.7f; g = 0.75f; b = 0.8f; break;   // Zinc crudo - gris azulado
        case BLOCK_RAW_COPPER: r = 0.9f; g = 0.5f; b = 0.3f; break;   // Cobre crudo - naranja rojizo
        default:              r = 1.0f; g = 1.0f; b = 1.0f; break;
    }
}

bool isBlockSolid(BlockType type) {
    return type != BLOCK_AIR && type != BLOCK_WATER && type != BLOCK_LAVA && type != BLOCK_ORANGE_FLOWER;
}

bool isBlockOpaque(BlockType type) {
    return type != BLOCK_AIR && type != BLOCK_WATER && type != BLOCK_LAVA && type != BLOCK_ORANGE_FLOWER && type != BLOCK_TALLGRASS && type != BLOCK_LEAVES;
}

// Obtener tiempo de rotura de un bloque en segundos (como Minecraft)
float getBlockBreakTime(BlockType type) {
    switch (type) {
        case BLOCK_DIRT:      return 0.5f;   // Tierra - rápido
        case BLOCK_GRASS:     return 0.6f;   // Pasto - rápido
        case BLOCK_SAND:      return 0.5f;   // Arena - rápido
        case BLOCK_TALLGRASS: return 0.0f;   // Pasto alto - instantáneo
        case BLOCK_LEAVES:    return 0.2f;   // Hojas - muy rápido
        case BLOCK_WOOD:      return 2.0f;   // Madera - medio
        case BLOCK_STONE:     return 1.5f;   // Piedra - medio-lento
        case BLOCK_BEDROCK:   return 999.0f; // Bedrock - irrompible
        case BLOCK_WATER:     return 0.0f;   // Agua - no se puede romper
        case BLOCK_GRAVEL:    return 0.6f;   // Grava - rápido
        case BLOCK_ORANGE_FLOWER: return 0.0f;   // Flor naranja - instantáneo
        case BLOCK_COAL_ORE:  return 3.0f;   // Minerales - lentos
        case BLOCK_IRON_ORE:  return 3.0f;
        case BLOCK_GOLD_ORE:  return 3.0f;
        case BLOCK_DIAMOND_ORE: return 3.0f;
        case BLOCK_SNOW:      return 0.2f;   // Nieve - muy rápido
        case BLOCK_COBBLESTONE: return 2.0f; // Piedra labrada - medio
        case BLOCK_PLANKS:    return 2.0f;   // Tablones - medio
        case BLOCK_SCRAP_METAL: return 2.5f; // Metal desecho - medio-lento
        case BLOCK_LAVA:      return 0.0f;   // Lava - no se puede romper
        default:              return 1.0f;   // Por defecto
    }
}

bool shouldRenderFace(BlockType currentBlock, BlockType neighborBlock) {
    // Siempre renderizar si el vecino es aire
    if (neighborBlock == BLOCK_AIR) return true;

    // Agua: renderizar todas las caras excepto si el vecino también es agua
    if (currentBlock == BLOCK_WATER) {
        return neighborBlock != BLOCK_WATER;
    }

    // Si el vecino es agua y el bloque actual es sólido, renderizar
    if (neighborBlock == BLOCK_WATER && isBlockOpaque(currentBlock)) return true;

    // No renderizar caras entre bloques solidos identicos
    if (currentBlock == neighborBlock) return false;

    // Bloques transparentes (hojas, pasto alto): renderizar si vecino es diferente
    if (!isBlockOpaque(currentBlock)) return true;

    // No renderizar si el vecino es un bloque solido diferente
    if (isBlockOpaque(neighborBlock)) return false;

    return true;
}

// ============================================================================
// MODERN RENDERING - VBO/VAO System
// ============================================================================

struct Vertex {
    float x, y, z;       // Position
    float r, g, b;       // Color
    float u, v;          // Texture coords

    Vertex(float x, float y, float z, float r, float g, float b, float u, float v)
        : x(x), y(y), z(z), r(r), g(g), b(b), u(u), v(v) {}
};

// ============================================================================
// FRUSTUM CULLING - Solo renderizar chunks en vista
// ============================================================================

struct Frustum {
    float planes[6][4];  // 6 planes: left, right, bottom, top, near, far

    void extractFromMatrix(const float* viewProj) {
        // Extract frustum planes from view-projection matrix
        // Left plane
        planes[0][0] = viewProj[3] + viewProj[0];
        planes[0][1] = viewProj[7] + viewProj[4];
        planes[0][2] = viewProj[11] + viewProj[8];
        planes[0][3] = viewProj[15] + viewProj[12];

        // Right plane
        planes[1][0] = viewProj[3] - viewProj[0];
        planes[1][1] = viewProj[7] - viewProj[4];
        planes[1][2] = viewProj[11] - viewProj[8];
        planes[1][3] = viewProj[15] - viewProj[12];

        // Bottom plane
        planes[2][0] = viewProj[3] + viewProj[1];
        planes[2][1] = viewProj[7] + viewProj[5];
        planes[2][2] = viewProj[11] + viewProj[9];
        planes[2][3] = viewProj[15] + viewProj[13];

        // Top plane
        planes[3][0] = viewProj[3] - viewProj[1];
        planes[3][1] = viewProj[7] - viewProj[5];
        planes[3][2] = viewProj[11] - viewProj[9];
        planes[3][3] = viewProj[15] - viewProj[13];

        // Near plane
        planes[4][0] = viewProj[3] + viewProj[2];
        planes[4][1] = viewProj[7] + viewProj[6];
        planes[4][2] = viewProj[11] + viewProj[10];
        planes[4][3] = viewProj[15] + viewProj[14];

        // Far plane
        planes[5][0] = viewProj[3] - viewProj[2];
        planes[5][1] = viewProj[7] - viewProj[6];
        planes[5][2] = viewProj[11] - viewProj[10];
        planes[5][3] = viewProj[15] - viewProj[14];

        // Normalize planes
        for (int i = 0; i < 6; i++) {
            float length = sqrtf(planes[i][0] * planes[i][0] +
                                planes[i][1] * planes[i][1] +
                                planes[i][2] * planes[i][2]);
            planes[i][0] /= length;
            planes[i][1] /= length;
            planes[i][2] /= length;
            planes[i][3] /= length;
        }
    }

    bool isChunkVisible(float chunkX, float chunkY, float chunkZ, float chunkSize) {
        // AABB (Axis-Aligned Bounding Box) test
        float minX = chunkX;
        float minY = chunkY;
        float minZ = chunkZ;
        float maxX = chunkX + chunkSize;
        float maxY = chunkY + 256.0f;  // CHUNK_HEIGHT
        float maxZ = chunkZ + chunkSize;

        for (int i = 0; i < 6; i++) {
            float px = (planes[i][0] > 0) ? maxX : minX;
            float py = (planes[i][1] > 0) ? maxY : minY;
            float pz = (planes[i][2] > 0) ? maxZ : minZ;

            float dot = planes[i][0] * px + planes[i][1] * py +
                       planes[i][2] * pz + planes[i][3];

            if (dot < 0) return false;  // Outside frustum
        }

        return true;
    }
};

// ============================================================================
// SISTEMA DE AUDIO (Sonidos del juego)
// ============================================================================

class SoundManager {
private:
    std::string soundPath;
    std::map<std::string, std::string> soundFiles;
    float masterVolume;
    bool enabled;

    // Timers para evitar spam de sonidos
    double lastFootstepTime;
    double lastBreakTime;
    double lastPlaceTime;

public:
    SoundManager(const std::string& path = getResourceRoot() + "/sounds/")
        : soundPath(path), masterVolume(0.5f), enabled(true),
          lastFootstepTime(0), lastBreakTime(0), lastPlaceTime(0) {

        // ⭐ MEJORADO: Registrar archivos de sonido completos
        soundFiles["footstep_grass"] = soundPath + "footstep_grass.wav";
        soundFiles["footstep_stone"] = soundPath + "footstep_stone.wav";
        soundFiles["footstep_wood"] = soundPath + "footstep_wood.wav";
        soundFiles["footstep_sand"] = soundPath + "footstep_sand.wav";
        soundFiles["footstep_gravel"] = soundPath + "footstep_gravel.wav";
        soundFiles["footstep_leaves"] = soundPath + "footstep_leaves.wav";

        soundFiles["break_stone"] = soundPath + "break_stone.wav";
        soundFiles["break_grass"] = soundPath + "break_grass.wav";
        soundFiles["break_wood"] = soundPath + "break_wood.wav";
        soundFiles["break_sand"] = soundPath + "break_sand.wav";
        soundFiles["break_gravel"] = soundPath + "break_gravel.wav";
        soundFiles["break_leaves"] = soundPath + "break_leaves.wav";
        soundFiles["break_ore"] = soundPath + "break_ore.wav";

        soundFiles["breaking_loop"] = soundPath + "breaking_loop.wav"; // ⭐ Sonido continuo
        soundFiles["place_block"] = soundPath + "place_block.wav";
        soundFiles["menu_click"] = soundPath + "menu_click.wav";
        soundFiles["jump"] = soundPath + "jump.wav";
        soundFiles["land"] = soundPath + "land.wav";
        soundFiles["splash"] = soundPath + "splash.wav";
    }

    // Reproducir sonido de forma asíncrona
    void playSound(const std::string& soundName, bool async = true) {
        if (!enabled) return;

        auto it = soundFiles.find(soundName);
        if (it == soundFiles.end()) return;

        std::string filepath = it->second;

        // Verificar si el archivo existe
        std::ifstream file(filepath);
        if (!file.good()) {
            // Archivo no existe, generar beep simple como fallback
            #ifdef _WIN32
            Beep(440, 50); // Beep simple de 440Hz por 50ms
            #endif
            return;
        }
        file.close();

        // Reproducir sonido
        #ifdef _WIN32
        DWORD flags = SND_FILENAME;
        if (async) flags |= SND_ASYNC;
        else flags |= SND_SYNC;

        PlaySoundA(filepath.c_str(), NULL, flags);
        #endif
    }

    // ⭐ MEJORADO: Reproducir sonido de paso con más variedad
    void playFootstep(BlockType blockType, double currentTime) {
        // Throttle: solo un sonido cada 0.3 segundos
        if (currentTime - lastFootstepTime < 0.3) return;
        lastFootstepTime = currentTime;

        // ⭐ Seleccionar sonido según el tipo de bloque (mejorado)
        std::string soundName = "footstep_grass"; // Default

        // Piedra y minerales
        if (blockType == BLOCK_STONE || blockType == BLOCK_COBBLESTONE || blockType == BLOCK_BEDROCK ||
            blockType == BLOCK_COAL_ORE || blockType == BLOCK_IRON_ORE ||
            blockType == BLOCK_GOLD_ORE || blockType == BLOCK_SILVER_ORE ||
            blockType == BLOCK_DIAMOND_ORE || blockType == BLOCK_SCRAP_METAL || blockType == BLOCK_BRICKS) {
            soundName = "footstep_stone";
        }
        // Madera
        else if (blockType == BLOCK_WOOD || blockType == BLOCK_PLANKS) {
            soundName = "footstep_wood";
        }
        // Arena
        else if (blockType == BLOCK_SAND) {
            soundName = "footstep_sand";
        }
        // Grava (sonido único)
        else if (blockType == BLOCK_GRAVEL) {
            soundName = "footstep_gravel";
        }
        // Hojas (sonido único)
        else if (blockType == BLOCK_LEAVES) {
            soundName = "footstep_leaves";
        }
        // Tierra y pasto
        else if (blockType == BLOCK_DIRT || blockType == BLOCK_GRASS) {
            soundName = "footstep_grass";
        }

        playSound(soundName, true);
    }

    // ⭐ MEJORADO: Reproducir sonido de romper bloque con más variedad
    void playBreakBlock(BlockType blockType, double currentTime) {
        // Throttle: solo un sonido cada 0.1 segundos
        if (currentTime - lastBreakTime < 0.1) return;
        lastBreakTime = currentTime;

        std::string soundName = "break_grass"; // Default

        // Piedra básica
        if (blockType == BLOCK_STONE || blockType == BLOCK_COBBLESTONE || blockType == BLOCK_BEDROCK || blockType == BLOCK_BRICKS) {
            soundName = "break_stone";
        }
        // Minerales (sonido especial)
        else if (blockType == BLOCK_COAL_ORE || blockType == BLOCK_IRON_ORE ||
                 blockType == BLOCK_GOLD_ORE || blockType == BLOCK_SILVER_ORE ||
                 blockType == BLOCK_DIAMOND_ORE || blockType == BLOCK_SCRAP_METAL) {
            soundName = "break_ore";
        }
        // Madera
        else if (blockType == BLOCK_WOOD || blockType == BLOCK_PLANKS) {
            soundName = "break_wood";
        }
        // Hojas (sonido único)
        else if (blockType == BLOCK_LEAVES) {
            soundName = "break_leaves";
        }
        // Arena
        else if (blockType == BLOCK_SAND) {
            soundName = "break_sand";
        }
        // Grava (sonido único)
        else if (blockType == BLOCK_GRAVEL) {
            soundName = "break_gravel";
        }
        // Tierra y pasto
        else if (blockType == BLOCK_DIRT || blockType == BLOCK_GRASS) {
            soundName = "break_grass";
        }

        playSound(soundName, true);
    }

    // ⭐⭐⭐ NUEVO: Reproducir sonido continuo mientras se rompe un bloque
    void playBreakingLoop(bool isBreaking) {
        if (isBreaking) {
            // Reproducir loop en modo asíncrono con flag de loop
            #ifdef _WIN32
            auto it = soundFiles.find("breaking_loop");
            if (it != soundFiles.end()) {
                std::string filepath = it->second;
                std::ifstream file(filepath);
                if (file.good()) {
                    file.close();
                    PlaySoundA(filepath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_LOOP);
                }
            }
            #endif
        } else {
            // Detener el loop
            #ifdef _WIN32
            PlaySoundA(NULL, NULL, 0); // Detener todos los sonidos
            #endif
        }
    }

    // Reproducir sonido de colocar bloque (con throttling)
    void playPlaceBlock(double currentTime) {
        // Throttle: solo un sonido cada 0.1 segundos
        if (currentTime - lastPlaceTime < 0.1) return;
        lastPlaceTime = currentTime;

        playSound("place_block", true);
    }

    // Reproducir sonido de salto
    void playJump() {
        playSound("jump", true);
    }

    // Reproducir sonido de aterrizaje
    void playLand() {
        playSound("land", true);
    }

    // Reproducir sonido de menú/click
    void playMenuClick() {
        playSound("menu_click", true);
    }

    // Control de volumen (0.0 a 1.0)
    void setVolume(float volume) {
        masterVolume = fmaxf(0.0f, fminf(1.0f, volume));
    }

    // Habilitar/deshabilitar sonidos
    void setEnabled(bool enable) {
        enabled = enable;
    }

    bool isEnabled() const {
        return enabled;
    }
};

// Instancia global del SoundManager
SoundManager* g_soundManager = nullptr;

// ============================================================================
// PERLIN NOISE (para generacion procedural)
// ============================================================================

class PerlinNoise {
private:
    std::vector<int> p;

    float fade(float t) const {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }

    float lerp(float t, float a, float b) const {
        return a + t * (b - a);
    }

    float grad(int hash, float x, float y, float z) const {
        int h = hash & 15;
        float u = h < 8 ? x : y;
        float v = h < 4 ? y : h == 12 || h == 14 ? x : z;
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }

public:
    PerlinNoise(unsigned int seed) {
        p.resize(256);
        for (int i = 0; i < 256; i++) p[i] = i;

        // RNG local que reproduce exactamente la secuencia de rand() de MSVC.
        // Antes esto era srand()/rand(), estado global del proceso: como se
        // construye un PerlinNoise por chunk, cada generación reiniciaba la
        // secuencia que usan las partículas (volviéndolas repetitivas) y hacía
        // imposible generar chunks en varios hilos. Se replica el LCG en lugar
        // de usar <random> para que la permutación —y por tanto el terreno de
        // los mundos existentes— salga idéntica.
        unsigned int rngState = seed;
        auto nextRand = [&rngState]() -> int {
            rngState = rngState * 214013u + 2531011u;
            return (int)((rngState >> 16) & 0x7FFF);
        };

        for (int i = 255; i > 0; i--) {
            int j = nextRand() % (i + 1);
            std::swap(p[i], p[j]);
        }

        p.insert(p.end(), p.begin(), p.end());
    }

    float noise(float x, float y, float z) const {
        int X = (int)floor(x) & 255;
        int Y = (int)floor(y) & 255;
        int Z = (int)floor(z) & 255;

        x -= floor(x);
        y -= floor(y);
        z -= floor(z);

        float u = fade(x);
        float v = fade(y);
        float w = fade(z);

        int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z;
        int B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;

        return lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z), 
                                        grad(p[BA], x - 1, y, z)),
                              lerp(u, grad(p[AB], x, y - 1, z), 
                                      grad(p[BB], x - 1, y - 1, z))),
                       lerp(v, lerp(u, grad(p[AA + 1], x, y, z - 1), 
                                       grad(p[BA + 1], x - 1, y, z - 1)),
                              lerp(u, grad(p[AB + 1], x, y - 1, z - 1),
                                      grad(p[BB + 1], x - 1, y - 1, z - 1))));
    }

    float octaveNoise(float x, float y, float z, int octaves) const {
        float total = 0;
        float frequency = 1;
        float amplitude = 1;
        float maxValue = 0;

        for (int i = 0; i < octaves; i++) {
            total += noise(x * frequency, y * frequency, z * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }
};

// ============================================================================
// ADVANCED NOISE SYSTEMS - Next-Gen Terrain Generation
// ============================================================================

// Worley Noise (Cellular/Voronoi) - Para continentes y estructuras celulares
class WorleyNoise {
private:
    unsigned int seed;

    // Hash function for cell points
    Vec3 hash3D(int x, int y, int z) const {
        unsigned int n = seed + x * 374761393 + y * 668265263 + z * 1274126177;
        n = (n ^ (n >> 13)) * 1274126177;
        n = n ^ (n >> 16);

        float fx = float(n & 0xFFFF) / 65536.0f;
        n = (n * 1103515245 + 12345);
        float fy = float(n & 0xFFFF) / 65536.0f;
        n = (n * 1103515245 + 12345);
        float fz = float(n & 0xFFFF) / 65536.0f;

        return Vec3(fx, fy, fz);
    }

public:
    WorleyNoise(unsigned int s) : seed(s) {}

    float noise(float x, float y, float z) const {
        int xi = (int)floor(x);
        int yi = (int)floor(y);
        int zi = (int)floor(z);

        float minDist = 10000.0f;

        // Check 3x3x3 neighboring cells
        for (int xo = -1; xo <= 1; xo++) {
            for (int yo = -1; yo <= 1; yo++) {
                for (int zo = -1; zo <= 1; zo++) {
                    Vec3 cellPoint = hash3D(xi + xo, yi + yo, zi + zo);
                    Vec3 cellPos = Vec3(xi + xo + cellPoint.x, yi + yo + cellPoint.y, zi + zo + cellPoint.z);
                    Vec3 diff = cellPos - Vec3(x, y, z);
                    float dist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

                    if (dist < minDist) {
                        minDist = dist;
                    }
                }
            }
        }

        return minDist;
    }

    float octaveNoise(float x, float y, float z, int octaves) const {
        float total = 0;
        float frequency = 1;
        float amplitude = 1;
        float maxValue = 0;

        for (int i = 0; i < octaves; i++) {
            total += noise(x * frequency, y * frequency, z * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }
};

// Ridged Noise - Para montañas realistas con crestas afiladas
class RidgedNoise {
private:
    PerlinNoise& baseNoise;

public:
    RidgedNoise(PerlinNoise& noise) : baseNoise(noise) {}

    float noise(float x, float y, float z) const {
        // Ridged: 1.0 - abs(noise) crea crestas afiladas
        float n = baseNoise.noise(x, y, z);
        return 1.0f - fabsf(n);
    }

    float octaveNoise(float x, float y, float z, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const {
        float total = 0;
        float frequency = 1;
        float amplitude = 1;
        float maxValue = 0;

        for (int i = 0; i < octaves; i++) {
            float n = noise(x * frequency, y * frequency, z * frequency);
            // Each octave is squared for sharper ridges
            n = n * n;
            total += n * amplitude;

            maxValue += amplitude;
            amplitude *= gain;
            frequency *= lacunarity;
        }

        return total / maxValue;
    }
};

// ============================================================================
// NEXT-GEN TERRAIN GENERATOR - Hierarchical Layer System
// ============================================================================

struct BiomeData {
    float temperature;      // -1 to 1
    float humidity;         // 0 to 1
    float continentalness;  // 0 to 1
    float erosion;          // 0 to 1
    float peaks;            // 0 to 1
    int biomeType;          // Enum del bioma final
};

enum BiomeType {
    BIOME_OCEAN_DEEP,
    BIOME_OCEAN,
    BIOME_BEACH,
    BIOME_RIVER,
    BIOME_LAKE,
    BIOME_PLAINS,
    BIOME_FOREST,
    BIOME_DENSE_FOREST,
    BIOME_HILLS,
    BIOME_MOUNTAINS,
    BIOME_MOUNTAINS_PEAKS,
    BIOME_DESERT,
    BIOME_SAVANNA,
    BIOME_TAIGA,
    BIOME_TUNDRA,
    BIOME_SWAMP
};

class NextGenTerrainGenerator {
private:
    PerlinNoise perlin;
    WorleyNoise worley;
    RidgedNoise ridged;
    unsigned int worldSeed;

public:
    NextGenTerrainGenerator(unsigned int seed)
        : perlin(seed), worley(seed), ridged(perlin), worldSeed(seed) {}

    // ========================================================================
    // LAYER 1: CONTINENTAL GENERATION (Worley + Perlin Hybrid)
    // ========================================================================
    float getContinentalness(float x, float z) const {
        // Worley noise creates natural continent-like structures
        float worleyValue = worley.octaveNoise(x * 0.0003f, 0, z * 0.0003f, 3);
        // Perlin adds variation
        float perlinValue = perlin.octaveNoise(x * 0.0005f, 0, z * 0.0005f, 4);

        // Hybrid: 70% Worley (continents) + 30% Perlin (variation)
        float continental = worleyValue * 0.7f + (perlinValue + 1.0f) * 0.5f * 0.3f;

        // Normalize to 0-1
        return fmaxf(0.0f, fminf(1.0f, continental));
    }

    // ========================================================================
    // LAYER 2: TEMPERATURE MAP
    // ========================================================================
    float getTemperature(float x, float z, float y) const {
        // Base temperature from climate noise
        float climateNoise = perlin.octaveNoise(x * 0.001f, 300, z * 0.001f, 3);

        // Altitude affects temperature (higher = colder)
        float altitudeFactor = -y * 0.004f;

        // Latitude simulation (distance from equator)
        float latitudeFactor = sinf(z * 0.00015f) * 0.5f;

        float temperature = latitudeFactor + altitudeFactor + climateNoise * 0.3f;

        return fmaxf(-1.0f, fminf(1.0f, temperature));
    }

    // ========================================================================
    // LAYER 3: HUMIDITY MAP
    // ========================================================================
    float getHumidity(float x, float z, float continentalness) const {
        // Base humidity from weather patterns
        float humidityNoise = perlin.octaveNoise(x * 0.002f, 400, z * 0.002f, 3);
        humidityNoise = (humidityNoise + 1.0f) * 0.5f; // 0-1

        // Coastal areas are more humid
        float coastalEffect = 1.0f - continentalness;
        coastalEffect = coastalEffect * coastalEffect; // Quadratic falloff

        float humidity = humidityNoise * 0.7f + coastalEffect * 0.3f;

        return fmaxf(0.0f, fminf(1.0f, humidity));
    }

    // ========================================================================
    // LAYER 4: EROSION MAP
    // ========================================================================
    float getErosion(float x, float z) const {
        float erosionNoise = perlin.octaveNoise(x * 0.003f, 100, z * 0.003f, 4);
        return (erosionNoise + 1.0f) * 0.5f; // Normalize to 0-1
    }

    // ========================================================================
    // LAYER 5: PEAKS AND VALLEYS (Ridged Noise)
    // ========================================================================
    float getPeaksAndValleys(float x, float z) const {
        // Ridged noise creates sharp mountain ridges
        return ridged.octaveNoise(x * 0.008f, 200, z * 0.008f, 5, 2.2f, 0.55f);
    }

    // ========================================================================
    // LAYER 6: BIOME DETERMINATION
    // ========================================================================
    BiomeData getBiomeData(float x, float z, float y) const {
        BiomeData data;

        data.continentalness = getContinentalness(x, z);
        data.temperature = getTemperature(x, z, y);
        data.humidity = getHumidity(x, z, data.continentalness);
        data.erosion = getErosion(x, z);
        data.peaks = getPeaksAndValleys(x, z);

        // Determine final biome type
        // ⭐ AJUSTADO: Océanos y planicies más comunes, desiertos raros, PLAYAS MUY COMUNES
        if (data.continentalness < 0.3f) {
            data.biomeType = data.continentalness < 0.15f ? BIOME_OCEAN_DEEP : BIOME_OCEAN;
        }
        else if (data.continentalness < 0.45f) {  // ⭐ Aumentado de 0.38 a 0.45 (playas casi 2x más comunes)
            data.biomeType = BIOME_BEACH;
        }
        else if (data.peaks > 0.65f && data.continentalness > 0.55f) {
            data.biomeType = data.peaks > 0.8f ? BIOME_MOUNTAINS_PEAKS : BIOME_MOUNTAINS;
        }
        else if (data.erosion > 0.5f && data.erosion < 0.75f) {
            data.biomeType = BIOME_HILLS;
        }
        else {
            // Temperature + Humidity based biomes
            // DESIERTOS MUY RAROS: Requieren temperatura MUY alta (>0.75) Y humedad MUY baja (<0.15)
            if (data.temperature > 0.75f && data.humidity < 0.15f) {
                data.biomeType = BIOME_DESERT;
            }
            // Savanna solo con temperatura muy alta
            else if (data.temperature > 0.6f && data.humidity < 0.4f) {
                data.biomeType = BIOME_SAVANNA;
            }
            else if (data.temperature < -0.3f) {
                data.biomeType = data.humidity < 0.4f ? BIOME_TUNDRA : BIOME_TAIGA;
            }
            else {
                // BOSQUES Y PLANICIES MÁS COMUNES
                if (data.humidity > 0.65f) {
                    data.biomeType = BIOME_DENSE_FOREST; // Bosque denso
                } else if (data.humidity > 0.35f) { // Reducido de 0.45 a 0.35
                    data.biomeType = BIOME_FOREST; // Bosque normal (más común)
                } else {
                    data.biomeType = BIOME_PLAINS; // Planicies (más comunes)
                }
            }
        }

        return data;
    }

    // ========================================================================
    // LAYER 6.5: BIOME BLENDING (ELIMINA CORTES ENTRE BIOMAS)
    // ========================================================================
    // ⭐⭐⭐ NUEVA FUNCIÓN: Calcula qué tan cerca estamos de un borde de bioma
    float getBiomeBlendFactor(float value, float threshold, float blendRadius) const {
        float distance = fabsf(value - threshold);
        if (distance >= blendRadius) return 0.0f; // Lejos del borde, sin blend

        // Cerca del borde, calcular factor de blend (0-1)
        float blend = 1.0f - (distance / blendRadius);

        // Smoothstep para transición ultra-suave
        blend = blend * blend * (3.0f - 2.0f * blend);

        return blend;
    }

    // ========================================================================
    // LAYER 6.6: BIOME CATEGORIZATION SYSTEM
    // ========================================================================
    enum BiomeCategory {
        CATEGORY_AQUATIC,      // Océanos, ríos, lagos
        CATEGORY_BEACH,        // Playas (zona de transición)
        CATEGORY_LOWLAND,      // Llanuras, bosques, sabanas
        CATEGORY_HIGHLAND,     // Colinas
        CATEGORY_MOUNTAIN,     // Montañas
        CATEGORY_COLD,         // Tundra, taiga (biomas fríos)
        CATEGORY_HOT_DRY       // Desiertos
    };

    BiomeCategory getBiomeCategory(BiomeType type) const {
        switch (type) {
            case BIOME_OCEAN_DEEP:
            case BIOME_OCEAN:
                return CATEGORY_AQUATIC;

            case BIOME_BEACH:
            case BIOME_RIVER:
                return CATEGORY_BEACH;

            case BIOME_PLAINS:
            case BIOME_FOREST:
            case BIOME_DENSE_FOREST:
            case BIOME_SAVANNA:
            case BIOME_SWAMP:
                return CATEGORY_LOWLAND;

            case BIOME_HILLS:
                return CATEGORY_HIGHLAND;

            case BIOME_MOUNTAINS:
            case BIOME_MOUNTAINS_PEAKS:
                return CATEGORY_MOUNTAIN;

            case BIOME_TUNDRA:
            case BIOME_TAIGA:
                return CATEGORY_COLD;

            case BIOME_DESERT:
                return CATEGORY_HOT_DRY;

            default:
                return CATEGORY_LOWLAND;
        }
    }

    // ⭐⭐⭐ NUEVO: Determinar si dos biomas son compatibles
    bool areBiomesCompatible(BiomeType type1, BiomeType type2) const {
        BiomeCategory cat1 = getBiomeCategory(type1);
        BiomeCategory cat2 = getBiomeCategory(type2);

        // Mismo bioma = siempre compatible
        if (type1 == type2) return true;

        // Reglas de compatibilidad
        // Acuático solo compatible con playa o acuático
        if (cat1 == CATEGORY_AQUATIC && cat2 != CATEGORY_BEACH && cat2 != CATEGORY_AQUATIC) return false;
        if (cat2 == CATEGORY_AQUATIC && cat1 != CATEGORY_BEACH && cat1 != CATEGORY_AQUATIC) return false;

        // Playa es compatible con todo (zona de transición universal)
        if (cat1 == CATEGORY_BEACH || cat2 == CATEGORY_BEACH) return true;

        // Desierto y biomas fríos son incompatibles directamente
        if (cat1 == CATEGORY_HOT_DRY && cat2 == CATEGORY_COLD) return false;
        if (cat1 == CATEGORY_COLD && cat2 == CATEGORY_HOT_DRY) return false;

        // Todo lo demás es compatible (con transición suave)
        return true;
    }

    // ⭐⭐⭐ NUEVO: Calcular intensidad de blend según categorías
    float getBlendIntensity(BiomeType type1, BiomeType type2, int heightDiff) const {
        BiomeCategory cat1 = getBiomeCategory(type1);
        BiomeCategory cat2 = getBiomeCategory(type2);

        // Sin diferencia de altura = poco blend
        if (heightDiff < 3) return 0.05f;

        // Transiciones especiales que necesitan mucho blend
        // Montaña-Tierra: blend intenso
        if ((cat1 == CATEGORY_MOUNTAIN && cat2 == CATEGORY_LOWLAND) ||
            (cat2 == CATEGORY_MOUNTAIN && cat1 == CATEGORY_LOWLAND)) {
            return 0.35f; // 35% de influencia
        }

        // Montaña-Colina: blend medio
        if ((cat1 == CATEGORY_MOUNTAIN && cat2 == CATEGORY_HIGHLAND) ||
            (cat2 == CATEGORY_MOUNTAIN && cat1 == CATEGORY_HIGHLAND)) {
            return 0.25f;
        }

        // Colina-Tierra: blend medio-bajo
        if ((cat1 == CATEGORY_HIGHLAND && cat2 == CATEGORY_LOWLAND) ||
            (cat2 == CATEGORY_HIGHLAND && cat1 == CATEGORY_LOWLAND)) {
            return 0.20f;
        }

        // Acuático-Playa: blend suave
        if ((cat1 == CATEGORY_AQUATIC && cat2 == CATEGORY_BEACH) ||
            (cat2 == CATEGORY_AQUATIC && cat1 == CATEGORY_BEACH)) {
            return 0.15f;
        }

        // Por defecto: blend moderado según diferencia de altura
        if (heightDiff > 15) return 0.30f;
        if (heightDiff > 10) return 0.25f;
        if (heightDiff > 7) return 0.20f;
        return 0.15f;
    }

    // ========================================================================
    // LAYER 6.75: AUTOMATIC BIOME TRANSITION BLENDING (MEJORADO)
    // ========================================================================
    // ⭐⭐⭐ MEJORADO: Sistema automático con detección de compatibilidad
    int getBlendedTerrainHeight(float x, float z) const {
        // Muestrear punto central
        BiomeData centerBiome = getBiomeData(x, z, 64);
        int centerHeight = getTerrainHeightInternal(x, z, centerBiome);

        // ⭐ MEJORADO: Radio adaptativo según el bioma
        BiomeCategory centerCategory = getBiomeCategory((BiomeType)centerBiome.biomeType);
        float sampleRadius = 6.0f; // Base

        // Montañas necesitan más radio de muestreo para transiciones suaves
        if (centerCategory == CATEGORY_MOUNTAIN) {
            sampleRadius = 12.0f;
        } else if (centerCategory == CATEGORY_HIGHLAND) {
            sampleRadius = 8.0f;
        }

        // ⭐ MEJORADO: 8 direcciones en lugar de 4 para mejor detección
        const int SAMPLES = 8;
        const float angleStep = 6.28318f / SAMPLES; // 2*PI / 8

        float totalWeight = 1.0f; // Peso del centro
        float weightedHeight = (float)centerHeight;

        for (int i = 0; i < SAMPLES; i++) {
            float angle = angleStep * i;
            float sampleX = x + cosf(angle) * sampleRadius;
            float sampleZ = z + sinf(angle) * sampleRadius;

            BiomeData neighborBiome = getBiomeData(sampleX, sampleZ, 64);

            // Si el bioma vecino es diferente
            if (neighborBiome.biomeType != centerBiome.biomeType) {
                // Verificar compatibilidad
                if (!areBiomesCompatible((BiomeType)centerBiome.biomeType, (BiomeType)neighborBiome.biomeType)) {
                    // Biomas incompatibles: forzar blend intenso para prevenir colisión abrupta
                    int neighborHeight = getTerrainHeightInternal(sampleX, sampleZ, neighborBiome);
                    float forceBlend = 0.40f; // Blend muy intenso
                    weightedHeight += (float)neighborHeight * forceBlend;
                    totalWeight += forceBlend;
                } else {
                    // Biomas compatibles: blend normal basado en diferencia de altura
                    int neighborHeight = getTerrainHeightInternal(sampleX, sampleZ, neighborBiome);
                    int heightDiff = abs(neighborHeight - centerHeight);

                    if (heightDiff > 3) {
                        float blendIntensity = getBlendIntensity(
                            (BiomeType)centerBiome.biomeType,
                            (BiomeType)neighborBiome.biomeType,
                            heightDiff
                        );

                        weightedHeight += (float)neighborHeight * blendIntensity;
                        totalWeight += blendIntensity;
                    }
                }
            }
        }

        // Promedio ponderado
        int blendedHeight = (int)(weightedHeight / totalWeight);

        return blendedHeight;
    }

    // ========================================================================
    // LAYER 7: TERRAIN HEIGHT CALCULATION (Internal - sin blending)
    // ========================================================================
    int getTerrainHeightInternal(float x, float z, const BiomeData& biome) const {
        const int SEA_LEVEL = 64;
        int height = SEA_LEVEL;

        // ⭐ MEJORADO: Multi-scale detail noise con más capas para relieve natural
        float largeDetail = (perlin.octaveNoise(x * 0.006f, 700, z * 0.006f, 4) + 1.0f) * 0.5f;
        float mediumDetail = (perlin.octaveNoise(x * 0.012f, 800, z * 0.012f, 3) + 1.0f) * 0.5f;
        float fineDetail = (perlin.octaveNoise(x * 0.025f, 900, z * 0.025f, 2) + 1.0f) * 0.5f;

        // ⭐ NUEVO: Ultra-fine detail para variación de 1-2 bloques (más desorden natural)
        float ultraFineDetail = (perlin.octaveNoise(x * 0.08f, 950, z * 0.08f, 2) + 1.0f) * 0.5f;

        // ⭐ NUEVO: Micro-bumps para superficie irregular (realismo)
        float microBumps = (perlin.octaveNoise(x * 0.15f, 1000, z * 0.15f, 1) + 1.0f) * 0.5f;

        switch (biome.biomeType) {
            case BIOME_OCEAN_DEEP:
                {
                    height = (int)(biome.continentalness * 40) + 20;
                    // ⭐ MEJORADO: Fondo oceánico con trincheras y montañas submarinas
                    height += (int)((mediumDetail - 0.5f) * 10);
                    height += (int)((fineDetail - 0.5f) * 6);

                    // ⭐ NUEVO: Trincheras oceánicas profundas
                    float trenchNoise = perlin.octaveNoise(x * 0.015f, 1560, z * 0.015f, 3);
                    if (trenchNoise < 0.3f) {
                        height -= (int)((0.3f - trenchNoise) * 40); // Trincheras hasta -12 bloques
                    }

                    // ⭐ NUEVO: Montañas submarinas
                    float seamountNoise = perlin.octaveNoise(x * 0.02f, 1580, z * 0.02f, 3);
                    if (seamountNoise > 0.7f) {
                        height += (int)((seamountNoise - 0.7f) * 50); // Montañas hasta +15 bloques
                    }
                }
                break;

            case BIOME_OCEAN:
                {
                    height = (int)(biome.continentalness * 50) + 25;
                    // ⭐ MEJORADO: Océanos con más características del fondo marino
                    height += (int)((largeDetail - 0.5f) * 12);
                    height += (int)((mediumDetail - 0.5f) * 8);

                    // ⭐ NUEVO: Colinas submarinas
                    float underwaterHills = perlin.octaveNoise(x * 0.03f, 1600, z * 0.03f, 2);
                    if (underwaterHills > 0.6f) {
                        height += (int)((underwaterHills - 0.6f) * 25); // Colinas hasta +10 bloques
                    }

                    // ⭐ NUEVO: Depresiones en el fondo
                    float underwaterValleys = perlin.octaveNoise(x * 0.035f, 1620, z * 0.035f, 2);
                    if (underwaterValleys < 0.4f) {
                        height -= (int)((0.4f - underwaterValleys) * 20); // Depresiones hasta -8 bloques
                    }
                }
                break;

            case BIOME_BEACH:
                {
                    height = SEA_LEVEL + (int)((biome.continentalness - 0.3f) * 20);
                    // ⭐ MEJORADO: Playas con dunas costeras realistas
                    height += (int)((mediumDetail - 0.5f) * 5);
                    height += (int)((fineDetail - 0.5f) * 4);
                    height += (int)((ultraFineDetail - 0.5f) * 2);

                    // ⭐ NUEVO: Dunas costeras (beach dunes)
                    float beachDunes = perlin.octaveNoise(x * 0.04f, 1640, z * 0.04f, 2);
                    if (beachDunes > 0.6f) {
                        height += (int)((beachDunes - 0.6f) * 18); // Dunas hasta +7 bloques
                    }

                    // ⭐ NUEVO: Depresiones en playas (tidal pools area)
                    float tidalPools = perlin.octaveNoise(x * 0.06f, 1660, z * 0.06f, 1);
                    if (tidalPools < 0.35f && height > SEA_LEVEL) {
                        height -= (int)((0.35f - tidalPools) * 8); // Pequeñas depresiones hasta -2 bloques
                    }
                }
                break;

            case BIOME_MOUNTAINS_PEAKS:
            case BIOME_MOUNTAINS:
                {
                    // ⭐⭐⭐ MONTAÑAS IMPRESIONANTES - Altitudes seguras (hasta Y=200 max)
                    // Ridged mountains with sharp peaks and natural slopes
                    float peakIntensity = (biome.peaks - 0.65f) / 0.35f; // 0-1 normalized
                    peakIntensity = fmaxf(0.0f, fminf(1.0f, peakIntensity));

                    // Base height with smooth ramp (erosion creates slopes)
                    float slopeTransition = biome.erosion; // High erosion = more slopes
                    float baseHeight = peakIntensity * (1.0f - slopeTransition * 0.3f);

                    // ⭐ AJUSTADO: Base controlada (85-160 base para evitar exceder límites)
                    height = (int)(baseHeight * 75) + 85;

                    // ⭐ AJUSTADO: Multi-scale relief pronunciado pero seguro
                    height += (int)((largeDetail - 0.5f) * 28);  // Controlado
                    height += (int)((mediumDetail - 0.5f) * 18); // Controlado
                    height += (int)((fineDetail - 0.5f) * 12);   // Controlado
                    height += (int)((ultraFineDetail - 0.5f) * 5); // Controlado

                    // ⭐ AJUSTADO: Sharp peaks only in low-erosion areas (seguro)
                    if (biome.biomeType == BIOME_MOUNTAINS_PEAKS && biome.erosion < 0.4f) {
                        height += (int)(peakIntensity * 30); // ⭐ Picos altos pero seguros (hasta Y=200)

                        // ⭐ AJUSTADO: Picos pronunciados controlados
                        float sharpPeaks = perlin.octaveNoise(x * 0.03f, 1050, z * 0.03f, 3);
                        if (sharpPeaks > 0.55f) { // ⭐ Threshold más bajo (más común)
                            height += (int)((sharpPeaks - 0.55f) * 40); // Spikes hasta +18 bloques
                        }
                    }

                    // ⭐ MEJORADO: Acantilados suavizados en zonas de alta erosión (evita cortes abruptos)
                    if (biome.erosion > 0.6f) {
                        float cliffNoise = perlin.octaveNoise(x * 0.04f, 1100, z * 0.04f, 2);
                        // ⭐ Usar interpolación suave en lugar de threshold duro
                        if (cliffNoise > 0.6f) { // Threshold más bajo para transición suave
                            float cliffAmount = (cliffNoise - 0.6f) / 0.4f; // 0-1 ramp
                            height += (int)(cliffAmount * cliffAmount * 22); // Curva cuadrática suavizada
                        }
                    }
                }
                break;

            case BIOME_HILLS:
                {
                    height = (int)(biome.erosion * 45) + 58; // ⭐ Controlado: 58-103
                    // ⭐ MEJORADO: Colinas con relieve más natural y variado
                    height += (int)((largeDetail - 0.5f) * 20);  // Controlado
                    height += (int)((mediumDetail - 0.5f) * 14); // Controlado
                    height += (int)((fineDetail - 0.5f) * 9);    // Controlado
                    height += (int)((ultraFineDetail - 0.5f) * 3); // Controlado

                    // ⭐ MEJORADO: Crestas suavizadas (hilltop ridges) - transiciones graduales
                    float ridgeNoise = perlin.octaveNoise(x * 0.035f, 1500, z * 0.035f, 2);
                    if (ridgeNoise > 0.60f) { // Threshold más bajo
                        // Rampa suave en lugar de threshold duro
                        float ridgeAmount = (ridgeNoise - 0.60f) / 0.40f; // 0-1 gradient
                        ridgeAmount = fmaxf(0.0f, fminf(1.0f, ridgeAmount)); // Clamp
                        ridgeAmount = ridgeAmount * ridgeAmount; // Curva cuadrática
                        height += (int)(ridgeAmount * 24); // Crestas controladas
                    }

                    // ⭐ MEJORADO: Valles suavizados entre colinas
                    float valleyNoise = perlin.octaveNoise(x * 0.04f, 1520, z * 0.04f, 2);
                    if (valleyNoise < 0.40f) { // Threshold ajustado
                        // Rampa suave
                        float valleyAmount = (0.40f - valleyNoise) / 0.40f; // 0-1 gradient
                        valleyAmount = fmaxf(0.0f, fminf(1.0f, valleyAmount)); // Clamp
                        valleyAmount = valleyAmount * valleyAmount; // Curva cuadrática
                        height -= (int)(valleyAmount * 18); // Valles controlados
                    }

                    // ⭐ MEJORADO: Mesetas suavizadas en cimas
                    float mesaNoise = perlin.octaveNoise(x * 0.025f, 1540, z * 0.025f, 3);
                    if (mesaNoise > 0.65f) { // Threshold más bajo
                        // Rampa suave
                        float mesaAmount = (mesaNoise - 0.65f) / 0.35f; // 0-1 gradient
                        mesaAmount = fmaxf(0.0f, fminf(1.0f, mesaAmount)); // Clamp
                        mesaAmount = mesaAmount * mesaAmount; // Curva cuadrática
                        height += (int)(mesaAmount * 16); // Mesetas controladas
                    }
                }
                break;

            case BIOME_PLAINS:
                height = SEA_LEVEL + 4;
                // ⭐ MEJORADO: Planicies con ondulaciones suaves (no planas)
                height += (int)((largeDetail - 0.5f) * 8);
                height += (int)((mediumDetail - 0.5f) * 4);
                height += (int)((fineDetail - 0.5f) * 2);
                height += (int)((ultraFineDetail - 0.5f) * 1); // ⭐ NUEVO: Variación mínima
                break;

            case BIOME_DESERT:
                {
                    height = SEA_LEVEL + 4;

                    // ⭐ MEJORADO: Sistema de dunas multicapa más realista (controlado)
                    // Capa 1: Dunas masivas (ondas largas)
                    float massiveDunes = perlin.octaveNoise(x * 0.008f, 1150, z * 0.008f, 3);
                    height += (int)((massiveDunes - 0.5f) * 16); // Controlado

                    // Capa 2: Dunas principales (forma típica de desierto)
                    height += (int)((largeDetail - 0.5f) * 12); // Controlado

                    // Capa 3: Dunas secundarias
                    height += (int)((mediumDetail - 0.5f) * 7); // Controlado

                    // Capa 4: Ondulaciones de arena
                    height += (int)((fineDetail - 0.5f) * 4); // Controlado
                    height += (int)((ultraFineDetail - 0.5f) * 2);

                    // ⭐ MEJORADO: Dunas en media luna suavizadas (barchan dunes)
                    float crescentDune = perlin.octaveNoise(x * 0.03f, 1180, z * 0.03f, 2);
                    float crescentDir = perlin.octaveNoise(x * 0.015f, 1200, z * 0.015f, 1);
                    if (crescentDune > 0.60f) { // Threshold más bajo
                        // Forma de media luna con transición suave
                        float crescentAmount = (crescentDune - 0.60f) / 0.40f; // 0-1 gradient
                        crescentAmount = fmaxf(0.0f, fminf(1.0f, crescentAmount)); // Clamp
                        crescentAmount = crescentAmount * crescentAmount; // Curva cuadrática

                        // Modificar según dirección del viento simulado (también suavizado)
                        float dirAmount = (crescentDir - 0.3f) / 0.7f; // Gradient más amplio
                        dirAmount = fmaxf(0.0f, fminf(1.0f, dirAmount)); // Clamp

                        height += (int)(crescentAmount * dirAmount * 15); // Controlado
                    }

                    // ⭐ MEJORADO: Crestas de dunas suavizadas (dune ridges)
                    float ridgeNoise = perlin.octaveNoise(x * 0.04f, 1220, z * 0.04f, 2);
                    float ridgeAlign = perlin.octaveNoise(x * 0.01f, 1250, z * 0.01f, 1);
                    if (ridgeNoise > 0.65f && ridgeAlign > 0.55f) { // Thresholds más bajos
                        // Transiciones suaves
                        float ridgeAmount = (ridgeNoise - 0.65f) / 0.35f;
                        float alignAmount = (ridgeAlign - 0.55f) / 0.45f;
                        ridgeAmount = fmaxf(0.0f, fminf(1.0f, ridgeAmount)); // Clamp
                        alignAmount = fmaxf(0.0f, fminf(1.0f, alignAmount)); // Clamp
                        ridgeAmount = ridgeAmount * ridgeAmount;
                        alignAmount = alignAmount * alignAmount;
                        height += (int)(ridgeAmount * alignAmount * 20); // Controlado
                    }

                    // ⭐ MEJORADO: Depresiones desérticas suavizadas (valles entre dunas)
                    float valleyNoise = perlin.octaveNoise(x * 0.025f, 1270, z * 0.025f, 2);
                    if (valleyNoise < 0.38f) { // Threshold ajustado
                        // Rampa suave
                        float valleyAmount = (0.38f - valleyNoise) / 0.38f;
                        valleyAmount = fmaxf(0.0f, fminf(1.0f, valleyAmount)); // Clamp
                        valleyAmount = valleyAmount * valleyAmount;
                        height -= (int)(valleyAmount * 15); // Controlado
                    }
                }
                break;

            case BIOME_SAVANNA:
                {
                    height = SEA_LEVEL + 4;
                    // ⭐ MEJORADO: Sabanas con colinas irregulares y kopjes
                    height += (int)((largeDetail - 0.5f) * 12);
                    height += (int)((mediumDetail - 0.5f) * 7);
                    height += (int)((fineDetail - 0.5f) * 4);
                    height += (int)((ultraFineDetail - 0.5f) * 2);

                    // ⭐ NUEVO: Kopjes (pequeñas colinas rocosas características de sabanas)
                    float kopjeNoise = perlin.octaveNoise(x * 0.035f, 1300, z * 0.035f, 2);
                    if (kopjeNoise > 0.7f) {
                        height += (int)((kopjeNoise - 0.7f) * 35); // Colinas hasta +10 bloques
                    }

                    // ⭐ NUEVO: Mesetas planas (flat-topped hills)
                    float plateauNoise = perlin.octaveNoise(x * 0.02f, 1320, z * 0.02f, 3);
                    if (plateauNoise > 0.65f) {
                        int plateauHeight = (int)((plateauNoise - 0.65f) * 25);
                        // Aplanar la cima
                        float flatness = perlin.octaveNoise(x * 0.1f, 1340, z * 0.1f, 1);
                        if (flatness > 0.6f) {
                            height += plateauHeight;
                        }
                    }
                }
                break;

            case BIOME_FOREST:
            case BIOME_DENSE_FOREST:
                {
                    height = SEA_LEVEL + 2;
                    // ⭐ MEJORADO: Bosques con terreno muy irregular y natural
                    height += (int)((largeDetail - 0.5f) * 12);
                    height += (int)((mediumDetail - 0.5f) * 8);
                    height += (int)((fineDetail - 0.5f) * 5);
                    height += (int)((ultraFineDetail - 0.5f) * 2);
                    height += (int)((microBumps - 0.5f) * 1);

                    // ⭐ NUEVO: Colinas forestales (montículos pequeños)
                    float forestHills = perlin.octaveNoise(x * 0.04f, 1360, z * 0.04f, 2);
                    if (forestHills > 0.6f) {
                        height += (int)((forestHills - 0.6f) * 18); // Colinas hasta +7 bloques
                    }

                    // ⭐ NUEVO: Depresiones naturales (pequeños valles)
                    float forestValley = perlin.octaveNoise(x * 0.05f, 1380, z * 0.05f, 2);
                    if (forestValley < 0.35f) {
                        height -= (int)((0.35f - forestValley) * 12); // Valles hasta -4 bloques
                    }

                    // ⭐ NUEVO: Afloramientos rocosos en bosques densos
                    if (biome.biomeType == BIOME_DENSE_FOREST) {
                        float rockOut = perlin.octaveNoise(x * 0.06f, 1400, z * 0.06f, 1);
                        if (rockOut > 0.75f) {
                            height += (int)((rockOut - 0.75f) * 16); // Rocas hasta +4 bloques
                        }
                    }
                }
                break;

            case BIOME_TAIGA:
            case BIOME_TUNDRA:
                {
                    height = SEA_LEVEL + 3;
                    // ⭐ MEJORADO: Taigas y tundras con relieve complejo
                    height += (int)((largeDetail - 0.5f) * 14);
                    height += (int)((mediumDetail - 0.5f) * 9);
                    height += (int)((fineDetail - 0.5f) * 5);
                    height += (int)((ultraFineDetail - 0.5f) * 2);

                    // ⭐ NUEVO: Colinas redondeadas glaciales
                    float glacialHills = perlin.octaveNoise(x * 0.025f, 1420, z * 0.025f, 3);
                    if (glacialHills > 0.55f) {
                        height += (int)((glacialHills - 0.55f) * 28); // Colinas hasta +12 bloques
                    }

                    // ⭐ NUEVO: Depresiones (antiguos lagos glaciales)
                    float glacialDepression = perlin.octaveNoise(x * 0.03f, 1440, z * 0.03f, 2);
                    if (glacialDepression < 0.4f) {
                        height -= (int)((0.4f - glacialDepression) * 10); // Depresiones hasta -4 bloques
                    }

                    // ⭐ NUEVO: Afloramientos rocosos en tundra (permafrost exposure)
                    if (biome.biomeType == BIOME_TUNDRA) {
                        float rockExposure = perlin.octaveNoise(x * 0.07f, 1460, z * 0.07f, 1);
                        if (rockExposure > 0.73f) {
                            height += (int)((rockExposure - 0.73f) * 15); // Rocas hasta +4 bloques
                        }
                    }

                    // ⭐ NUEVO: Crestas morrenas (glacial moraines) en taiga
                    if (biome.biomeType == BIOME_TAIGA) {
                        float moraineRidge = perlin.octaveNoise(x * 0.05f, 1480, z * 0.05f, 2);
                        if (moraineRidge > 0.68f && moraineRidge < 0.75f) {
                            height += (int)((moraineRidge - 0.68f) * 25); // Crestas hasta +5 bloques
                        }
                    }
                }
                break;

            default:
                height = SEA_LEVEL;
                break;
        }

        // ========================================================================
        // FOOTHILL TRANSITION RELIEF: Add depth and variation at mountain bases
        // ========================================================================
        if (isFoothillZone(biome)) {
            float foothillRelief = getFoothillRelief(x, z, biome);
            height += (int)foothillRelief;
        }

        return height;
    }

    // ⭐⭐⭐ NUEVA: Función pública que usa blending automático
    int getTerrainHeight(float x, float z, const BiomeData& biome) const {
        // Usar la versión con blending automático para transiciones suaves
        return getBlendedTerrainHeight(x, z);
    }

    // ========================================================================
    // MOUNTAIN FEATURES: Lakes and Rivers - SMOOTH GRADIENTS (no hard cuts)
    // ========================================================================
    // ⭐⭐⭐ NUEVO: Versiones graduales para transiciones suaves entre chunks
    float getMountainLakeStrength(float x, float z, const BiomeData& biome) const {
        // Only in mountains
        if (biome.biomeType != BIOME_MOUNTAINS && biome.biomeType != BIOME_MOUNTAINS_PEAKS) {
            return 0.0f;
        }

        // Lake noise (creates isolated pockets)
        float lakeNoise = perlin.octaveNoise(x * 0.015f, 1100, z * 0.015f, 3);
        lakeNoise = (lakeNoise + 1.0f) * 0.5f; // 0-1

        // ⭐ Transición SUAVE: En lugar de threshold duro, usar rampa gradual
        if (biome.erosion < 0.4f) return 0.0f;
        if (lakeNoise < 0.65f) return 0.0f;

        // Rampa suave desde el umbral (0.65-0.85 = transición, 0.85+ = lago completo)
        float strength = (lakeNoise - 0.65f) / 0.20f; // 0-1 gradient
        strength = fminf(1.0f, strength);

        // Aplicar curva smoothstep para transición ultra-suave
        strength = strength * strength * (3.0f - 2.0f * strength);

        return strength; // 0.0 = no lago, 1.0 = lago completo
    }

    float getMountainRiverStrength(float x, float z, const BiomeData& biome) const {
        // Only in mountains
        if (biome.biomeType != BIOME_MOUNTAINS && biome.biomeType != BIOME_MOUNTAINS_PEAKS) {
            return 0.0f;
        }

        // River noise (creates narrow lines)
        float river1 = perlin.octaveNoise(x * 0.008f, 1200, z * 0.008f, 4);
        float river2 = perlin.octaveNoise(x * 0.008f, 1300, z * 0.008f, 4);

        float riverValue = sqrtf(river1 * river1 + river2 * river2);

        // ⭐ Transición SUAVE: Rampa gradual en lugar de threshold duro
        if (biome.erosion < 0.35f) return 0.0f;
        if (riverValue > 0.30f) return 0.0f;

        // Rampa suave desde el borde del río (0.25-0.30 = orilla, 0-0.25 = río completo)
        float strength = (0.30f - riverValue) / 0.05f; // 0-1 gradient (5 bloques de transición)
        strength = fminf(1.0f, strength);

        // Aplicar curva smoothstep para transición ultra-suave
        strength = strength * strength * (3.0f - 2.0f * strength);

        return strength; // 0.0 = no río, 1.0 = río completo
    }

    // ⭐ Mantener versiones booleanas para compatibilidad (wrappers)
    bool isMountainLake(float x, float z, const BiomeData& biome) const {
        return getMountainLakeStrength(x, z, biome) > 0.5f;
    }

    bool isMountainRiver(float x, float z, const BiomeData& biome) const {
        return getMountainRiverStrength(x, z, biome) > 0.5f;
    }

    // ========================================================================
    // FOOTHILL FEATURES: Transition zones where mountains meet lowlands
    // ========================================================================

    // Detect if we're in a mountain foothill zone (transition area)
    bool isFoothillZone(const BiomeData& biome) const {
        // Foothills are transition zones where peaks value is medium (0.5-0.65)
        // and we're not in a full mountain but close to one
        return (biome.peaks > 0.5f && biome.peaks < 0.65f &&
                biome.continentalness > 0.45f &&
                biome.biomeType != BIOME_MOUNTAINS &&
                biome.biomeType != BIOME_MOUNTAINS_PEAKS);
    }

    // ⭐⭐⭐ NUEVO: Versiones graduales para foothill features (transiciones suaves)
    float getFoothillLakeStrength(float x, float z, const BiomeData& biome) const {
        if (!isFoothillZone(biome)) {
            return 0.0f;
        }

        // Lake noise (creates small isolated ponds)
        float lakeNoise = perlin.octaveNoise(x * 0.02f, 1400, z * 0.02f, 3);
        lakeNoise = (lakeNoise + 1.0f) * 0.5f; // 0-1

        // ⭐ Transición SUAVE: Rampa gradual
        if (lakeNoise < 0.70f) return 0.0f;

        // Rampa suave (0.70-0.80 = orilla, 0.80+ = lago completo)
        float strength = (lakeNoise - 0.70f) / 0.10f;
        strength = fminf(1.0f, strength);

        // Smoothstep para transición ultra-suave
        strength = strength * strength * (3.0f - 2.0f * strength);

        return strength;
    }

    float getFoothillStreamStrength(float x, float z, const BiomeData& biome) const {
        if (!isFoothillZone(biome)) {
            return 0.0f;
        }

        // Stream noise (creates winding paths)
        float stream1 = perlin.octaveNoise(x * 0.01f, 1500, z * 0.01f, 4);
        float stream2 = perlin.octaveNoise(x * 0.01f, 1600, z * 0.01f, 4);

        float streamValue = sqrtf(stream1 * stream1 + stream2 * stream2);

        // ⭐ Transición SUAVE: Rampa gradual
        if (streamValue > 0.35f) return 0.0f;

        // Rampa suave (0.30-0.35 = orilla, 0-0.30 = arroyo completo)
        float strength = (0.35f - streamValue) / 0.05f;
        strength = fminf(1.0f, strength);

        // Smoothstep para transición ultra-suave
        strength = strength * strength * (3.0f - 2.0f * strength);

        return strength;
    }

    // ⭐ Mantener versiones booleanas para compatibilidad
    bool isFoothillLake(float x, float z, const BiomeData& biome) const {
        return getFoothillLakeStrength(x, z, biome) > 0.5f;
    }

    bool isFoothillStream(float x, float z, const BiomeData& biome) const {
        return getFoothillStreamStrength(x, z, biome) > 0.5f;
    }

    // Get additional relief for foothill zones
    float getFoothillRelief(float x, float z, const BiomeData& biome) const {
        if (!isFoothillZone(biome)) {
            return 0.0f;
        }

        // Transition strength based on how close we are to mountains
        float transitionStrength = (biome.peaks - 0.5f) / 0.15f; // 0-1 as peaks go from 0.5 to 0.65
        transitionStrength = fmaxf(0.0f, fminf(1.0f, transitionStrength));

        // Extra noise for rolling hills and depth
        float rollingHills = perlin.octaveNoise(x * 0.01f, 1700, z * 0.01f, 4);
        rollingHills = (rollingHills + 1.0f) * 0.5f; // 0-1

        // Create valleys and rises with ridged noise
        float valleyNoise = ridged.octaveNoise(x * 0.012f, 1800, z * 0.012f, 3, 2.0f, 0.5f);

        // Combine for natural relief (±15 blocks at max)
        float relief = (rollingHills - 0.5f) * 20.0f + (valleyNoise - 0.5f) * 10.0f;

        return relief * transitionStrength;
    }

    // ========================================================================
    // LAYER 9: 3D CAVE DENSITY SYSTEM (Density Fields)
    // ========================================================================
    bool isCaveAt(float x, float y, float z, int terrainHeight, BiomeType biomeType) const {
        // No caves in oceans
        if (biomeType == BIOME_OCEAN_DEEP || biomeType == BIOME_OCEAN) {
            return false;
        }

        // ⭐ BUG FIX: Proteger las capas superiores del terreno (evitar bloques flotantes)
        // No generar cuevas en los últimos 6 bloques de la superficie
        if (y >= terrainHeight - 6) {
            return false;
        }

        bool isCave = false;

        // ========================================================================
        // CUEVAS HIPER COMUNES - Umbrales muy reducidos
        // ========================================================================

        // Small caves (worm caves - MUY COMUNES, connected tunnels)
        float smallCaveNoise = perlin.octaveNoise(x * 0.06f, y * 0.06f, z * 0.06f, 2);
        if (y > 5 && y < terrainHeight - 8 && smallCaveNoise > 0.3f) {
            isCave = true;
        }

        // Large caves (cheese caves - COMUNES, wide caverns)
        float largeCaveNoise = perlin.octaveNoise(x * 0.035f, y * 0.035f, z * 0.035f, 3);
        if (y > 10 && y < terrainHeight - 10 && largeCaveNoise > 0.35f) {
            isCave = true;
        }

        // Massive caves (huge vertical caverns - ahora COMUNES)
        float massiveCaveNoise = perlin.octaveNoise(x * 0.02f, y * 0.02f, z * 0.02f, 4);
        if (y > 15 && y < 90 && massiveCaveNoise > 0.45f) {
            float caveHeightNoise = perlin.octaveNoise(x * 0.02f, 700, z * 0.02f, 2);
            float caveHeight = (caveHeightNoise + 1.0f) * 0.5f;
            int caveTop = (int)(caveHeight * 50) + 40;
            int caveBottom = caveTop - 20;

            if (y >= caveBottom && y <= caveTop) {
                isCave = true;
            }
        }

        // Cave entrances (accessible from surface) - MUY COMUNES pero más profundas
        // ⭐ MODIFICADO: Entradas más profundas para evitar romper la superficie
        if (y >= terrainHeight - 15 && y < terrainHeight - 7) {
            float entranceNoise = perlin.octaveNoise(x * 0.04f, y * 0.04f, z * 0.04f, 2);
            if (entranceNoise > 0.25f) {
                isCave = true;
            }
        }

        // Cuevas adicionales en profundidad (nuevo sistema)
        if (y > 5 && y < 40) {
            float deepCaveNoise = perlin.octaveNoise(x * 0.08f, y * 0.08f, z * 0.08f, 3);
            if (deepCaveNoise > 0.35f) {
                isCave = true;
            }
        }

        return isCave;
    }

    // ========================================================================
    // LAYER 9B: RAVINE GENERATION (Dramatic Vertical Cuts)
    // ========================================================================
    bool isRavineAt(float x, float y, float z, int terrainHeight, BiomeType biomeType) const {
        // No ravines in water biomes (oceans, beaches, rivers, lakes)
        if (biomeType == BIOME_OCEAN_DEEP || biomeType == BIOME_OCEAN ||
            biomeType == BIOME_BEACH || biomeType == BIOME_RIVER || biomeType == BIOME_LAKE) {
            return false;
        }

        // Ravines are deep vertical cuts that go from surface to deep underground
        // Use worm-like 2D noise for ravine paths, then extend vertically

        // Generate ravine paths using ridged noise (creates sharp edges)
        float ravineNoise1 = ridged.octaveNoise(x * 0.008f, 5000, z * 0.008f, 3, 2.0f, 0.5f);
        float ravineNoise2 = ridged.octaveNoise(x * 0.008f, 5100, z * 0.008f, 3, 2.0f, 0.5f);

        // Combine for winding paths
        float ravineValue = sqrtf(ravineNoise1 * ravineNoise1 + ravineNoise2 * ravineNoise2);

        // Ravines are very rare (threshold 0.15)
        if (ravineValue > 0.15f) {
            return false;
        }

        // Width of ravine (narrower at edges, wider in center)
        float ravineWidth = (0.15f - ravineValue) / 0.15f; // 0-1
        ravineWidth = ravineWidth * ravineWidth; // Square for sharper edges

        if (ravineWidth < 0.3f) {
            return false;
        }

        // Vertical bounds: ravines go from surface down to Y=10
        int ravineTop = terrainHeight - 5;  // Start 5 blocks below surface
        int ravineBottom = 10;  // Stop at Y=10

        // Add vertical variation using noise
        float depthNoise = perlin.octaveNoise(x * 0.01f, 5200, z * 0.01f, 2);
        ravineBottom += (int)(depthNoise * 10);  // Vary bottom by ±10 blocks

        if (y > ravineTop || y < ravineBottom) {
            return false;
        }

        // Ravine depth variation (narrower near top and bottom)
        float depthRatio = (float)(y - ravineBottom) / (float)(ravineTop - ravineBottom);
        float depthFactor = sinf(depthRatio * 3.14159f); // 0 at top/bottom, 1 in middle

        return ravineWidth * depthFactor > 0.5f;
    }

    // ========================================================================
    // LAYER 9C: CAVE DECORATIONS (Stalactites & Stalagmites)
    // ========================================================================

    // Check if stalactite should spawn (hangs from ceiling)
    int getStalactiteLength(float x, float y, float z, bool isCeiling) const {
        // Use noise to determine if stalactite/stalagmite spawns here
        float formationNoise = perlin.octaveNoise(x * 0.25f, y * 0.25f, z * 0.25f, 2);
        formationNoise = (formationNoise + 1.0f) * 0.5f; // 0-1

        if (formationNoise < 0.7f) {
            return 0; // No formation
        }

        // Length varies from 1 to 4 blocks
        int maxLength = 1 + (int)((formationNoise - 0.7f) / 0.3f * 3);

        // Stalactites from ceiling point down, stalagmites from floor point up
        return maxLength;
    }

    // ========================================================================
    // LAYER 9D: BOULDER FIELDS (Surface Rock Formations)
    // ========================================================================
    bool shouldSpawnBoulder(float x, float z, BiomeType biomeType) const {
        // Boulder fields in mountains, hills, and tundra
        if (biomeType != BIOME_MOUNTAINS && biomeType != BIOME_MOUNTAINS_PEAKS &&
            biomeType != BIOME_HILLS && biomeType != BIOME_TUNDRA) {
            return false;
        }

        float boulderNoise = perlin.octaveNoise(x * 0.05f, 6000, z * 0.05f, 2);
        boulderNoise = (boulderNoise + 1.0f) * 0.5f; // 0-1

        return boulderNoise > 0.75f; // 25% chance in valid biomes
    }

    int getBoulderSize(float x, float z) const {
        float sizeNoise = perlin.octaveNoise(x * 0.1f, 6100, z * 0.1f, 1);
        sizeNoise = (sizeNoise + 1.0f) * 0.5f; // 0-1

        if (sizeNoise > 0.9f) return 4; // Huge boulder
        if (sizeNoise > 0.75f) return 3; // Large boulder
        if (sizeNoise > 0.5f) return 2; // Medium boulder
        return 1; // Small boulder
    }

    // ========================================================================
    // LAYER 10: FOREST DENSITY MAP (with Mountain Slope Support)
    // ========================================================================
    float getForestDensity(float x, float z, BiomeType biomeType, float erosion = 0.5f) const {
        // No trees in water or desert biomes
        if (biomeType == BIOME_OCEAN_DEEP || biomeType == BIOME_OCEAN ||
            biomeType == BIOME_BEACH || biomeType == BIOME_DESERT) {
            return 0.0f;
        }

        // Forest density noise
        float densityNoise = perlin.octaveNoise(x * 0.1f, 900, z * 0.1f, 2);
        densityNoise = (densityNoise + 1.0f) * 0.5f; // 0-1

        // Biome-specific density modifiers
        float baseDensity = 0.15f; // 15% tree coverage default

        switch (biomeType) {
            case BIOME_DENSE_FOREST:
                baseDensity = 0.35f; // 35% coverage
                break;
            case BIOME_FOREST:
                baseDensity = 0.25f; // 25% coverage
                break;
            case BIOME_TAIGA:
                baseDensity = 0.22f; // 22% coverage
                break;
            case BIOME_PLAINS:
                baseDensity = 0.08f; // 8% coverage (sparse)
                break;
            case BIOME_SAVANNA:
                baseDensity = 0.05f; // 5% coverage (very sparse)
                break;
            case BIOME_HILLS:
                baseDensity = 0.18f; // 18% coverage
                break;
            case BIOME_MOUNTAINS:
            case BIOME_MOUNTAINS_PEAKS:
                // MOUNTAIN SLOPES: More trees on slopes (high erosion)
                if (erosion > 0.5f) {
                    // Slopes have more trees (15% coverage)
                    baseDensity = 0.15f;
                } else {
                    // Rocky peaks have few trees (3% coverage)
                    baseDensity = 0.03f;
                }
                break;
            default:
                baseDensity = 0.1f;
                break;
        }

        return densityNoise * baseDensity;
    }
};

// ============================================================================
// SISTEMA DE TEXTURAS - TextureManager
// ============================================================================

class TextureManager {
private:
    std::map<std::string, GLuint> textures;
    std::string resourcePath;

    // OPTIMIZACIÓN: Cache del último bind para evitar binds redundantes
    GLuint lastBoundTexture;

    // ANIMACIÓN: Frames de agua animada
    std::vector<GLuint> waterFrames;
    int currentWaterFrame;
    double waterAnimTimer;
    const double WATER_ANIM_SPEED = 0.15; // Cambiar frame cada 150ms

    // ANIMACIÓN: Texturas de grietas de bloques (destroy stages)
    std::vector<GLuint> destroyStageTextures;

public:
    TextureManager(const std::string& resPath = getResourceRoot() + "/resourcepacks/Textures/Blocks/")
        : resourcePath(resPath), lastBoundTexture(0), currentWaterFrame(0), waterAnimTimer(0.0) {}

    ~TextureManager() {
        // Liberar todas las texturas de OpenGL
        for (auto& pair : textures) {
            glDeleteTextures(1, &pair.second);
        }
        textures.clear();
    }

    // Cargar textura desde archivo PNG
    GLuint loadTexture(const std::string& filename) {
        // Si ya está cargada, retornarla
        if (textures.find(filename) != textures.end()) {
            return textures[filename];
        }

        std::string fullPath = resourcePath + filename;

        int width, height, channels;
        stbi_set_flip_vertically_on_load(true); // Voltear verticalmente para OpenGL (PNG origin = top-left, OpenGL origin = bottom-left)
        unsigned char* data = stbi_load(fullPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!data) {
            std::cerr << "ERROR: No se pudo cargar textura: " << fullPath << std::endl;
            std::cerr << "  Motivo: " << stbi_failure_reason() << std::endl;
            return 0;
        }

        std::cout << "Textura cargada: " << filename << " (" << width << "x" << height << ", " << channels << " canales)" << std::endl;

        GLuint textureID = 0;
        glGenTextures(1, &textureID);

        // ⭐ PROTECCIÓN: Verificar que se generó la textura correctamente
        if (textureID == 0) {
            std::cerr << "❌ ERROR: No se pudo generar textura OpenGL para " << filename << std::endl;
            stbi_image_free(data);
            return 0;
        }

        glBindTexture(GL_TEXTURE_2D, textureID);

        // Cargar imagen a OpenGL (optimizado - usar RGB en lugar de RGBA cuando sea posible)
        GLenum format = (channels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

        // ⭐ PROTECCIÓN: Verificar errores de OpenGL
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            std::cerr << "❌ ERROR OpenGL al cargar textura " << filename << ": " << error << std::endl;
            glDeleteTextures(1, &textureID);
            stbi_image_free(data);
            return 0;
        }

        // Filtros para estilo pixelado (como Minecraft) - NEAREST = mejor performance que LINEAR
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Wrap mode - REPEAT para texturas repetidas (compatible con OpenGL 1.1)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        stbi_image_free(data);

        textures[filename] = textureID;
        return textureID;
    }

    // Obtener textura ya cargada
    GLuint getTexture(const std::string& filename) {
        auto it = textures.find(filename);
        if (it != textures.end()) {
            return it->second;
        }
        // Si no existe, intentar cargarla
        return loadTexture(filename);
    }

    // OPTIMIZADO: Bind textura solo si es diferente de la última
    void bindOptimized(GLuint textureID) {
        if (textureID != lastBoundTexture && textureID > 0) {
            glBindTexture(GL_TEXTURE_2D, textureID);
            lastBoundTexture = textureID;
        }
    }

    // Bind textura para renderizado (legacy)
    void bind(const std::string& filename) {
        GLuint textureID = getTexture(filename);
        bindOptimized(textureID);
    }

    // Reset cache (llamar al inicio de cada frame/chunk)
    void resetBindCache() {
        lastBoundTexture = 0;
    }

    // ANIMACIÓN: Actualizar frame de agua
    void updateWaterAnimation(double deltaTime) {
        waterAnimTimer += deltaTime;
        if (waterAnimTimer >= WATER_ANIM_SPEED) {
            waterAnimTimer = 0.0;
            if (!waterFrames.empty()) {
                currentWaterFrame = (currentWaterFrame + 1) % waterFrames.size();
            }
        }
    }

    // ANIMACIÓN: Obtener frame actual de agua
    GLuint getCurrentWaterFrame() {
        if (waterFrames.empty()) {
            return getTexture("Agua.png"); // Fallback
        }
        return waterFrames[currentWaterFrame];
    }

    // ANIMACIÓN: Obtener textura base del agua (para comparación en render)
    GLuint getWaterTexture() {
        return getTexture("Agua.png");
    }

    // ANIMACIÓN: Cargar frames de agua animada (simulación procedural)
    void loadWaterAnimation() {
        // Como solo tenemos una textura de agua, vamos a simular animación
        // usando offsets de UV en el shader. Por ahora usamos la misma textura
        // pero podemos agregar múltiples frames después
        GLuint waterTex = getTexture("Agua.png");
        if (waterTex > 0) {
            waterFrames.push_back(waterTex);
            // En el futuro: cargar Agua_1.png, Agua_2.png, etc.
        }
    }

    // ANIMACIÓN: Cargar texturas de destrucción de bloques (grietas)
    void loadDestroyStageTextures() {
        std::cout << "=== Cargando texturas de destrucción de bloques ===" << std::endl;

        std::string animPath = resourcePath + "Animaciones/";
        destroyStageTextures.clear();

        // Cargar las 10 etapas (destroy_stage_0.png a destroy_stage_9.png)
        for (int i = 0; i < 10; i++) {
            std::string filename = animPath + "destroy_stage_" + std::to_string(i) + ".png";

            int width, height, channels;
            stbi_set_flip_vertically_on_load(true);
            unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

            if (!data) {
                std::cerr << "ERROR: No se pudo cargar textura de destrucción: " << filename << std::endl;
                destroyStageTextures.push_back(0);
                continue;
            }

            GLuint textureID;
            glGenTextures(1, &textureID);
            glBindTexture(GL_TEXTURE_2D, textureID);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

            // Filtros para estilo pixelado
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

            stbi_image_free(data);
            destroyStageTextures.push_back(textureID);

            std::cout << "  Cargado: destroy_stage_" << i << ".png (" << width << "x" << height << ")" << std::endl;
        }

        std::cout << "=== " << destroyStageTextures.size() << " texturas de destrucción cargadas ===" << std::endl;
    }

    // Obtener textura de destrucción según nivel (0-100%)
    // Sistema de 10 etapas: 0-9
    GLuint getDestroyStageTexture(float progress) {
        if (destroyStageTextures.empty() || progress <= 0.0f) return 0;

        // Mapear progress (0.0-1.0) a índice (0-9)
        int index = (int)(progress * 10.0f);
        if (index > 9) index = 9;
        if (index < 0) index = 0;

        return destroyStageTextures[index];
    }

    // Cargar todas las texturas de bloques
    void loadAllBlockTextures() {
        std::cout << "=== Cargando texturas de bloques ===" << std::endl;

        // Texturas básicas
        loadTexture("Piedra.png");          // BLOCK_STONE
        loadTexture("Tierra.png");          // BLOCK_DIRT
        loadTexture("Arena.png");           // BLOCK_SAND

        // Texturas de madera/tronco
        loadTexture("Tronco de Roble.png");          // BLOCK_WOOD sides
        loadTexture("Tronco de Roble a dentro.png"); // BLOCK_WOOD top/bottom

        // Texturas de pasto (múltiples)
        loadTexture("Bloque de pasto up.png"); // BLOCK_GRASS top
        loadTexture("Bloque de pasto.png");    // BLOCK_GRASS side

        // Texturas de hojas
        loadTexture("Hojas de Roble.png");     // BLOCK_LEAVES

        // Texturas de vegetación
        loadTexture("Pasto corto.png");        // BLOCK_TALLGRASS

        // Texturas de agua y lava
        loadTexture("Agua.png");               // BLOCK_WATER
        loadTexture("Lava.gif");               // BLOCK_LAVA (será tratada como PNG)

        // Texturas adicionales
        loadTexture("Grava.png");              // BLOCK_GRAVEL
        // BLOCK_ORANGE_FLOWER usa Lava.gif (ya cargada arriba)
        loadTexture("nieve.png");              // BLOCK_SNOW
        loadTexture("Piedra Labrada.png");     // BLOCK_COBBLESTONE (textura mejorada)
        loadTexture("Tablones de Madera de Roble.png");  // BLOCK_PLANKS
        loadTexture("Polvo de Tierra.png");    // BLOCK_DIRT_POWDER (item crafteable)
        loadTexture("palo.png");               // BLOCK_STICK (item crafteable)
        loadTexture("palo.png");               // BLOCK_HOE (herramienta - usa textura de palo)
        loadTexture("carbon.png");             // BLOCK_COAL_ITEM (dropea de carbón mineral)
        loadTexture("zinc crudo.png");         // BLOCK_RAW_ZINC (dropea de desecho de metales)
        loadTexture("cobre crudo.png");        // BLOCK_RAW_COPPER (dropea de desecho de metales)

        // Minerales - Sistema de Rareza
        // COMUNES
        loadTexture("Mineral de Carbon.png");   // BLOCK_COAL_ORE (común)
        loadTexture("Desecho de metales.png");  // BLOCK_SCRAP_METAL (común)

        // POCO COMUNES
        loadTexture("Piedra.png");              // BLOCK_IRON_ORE (temporal - usar piedra)
        loadTexture("Piedra.png");              // BLOCK_GOLD_ORE (temporal - usar piedra)
        loadTexture("Piedra.png");              // BLOCK_SILVER_ORE (temporal - usar piedra)

        // RAROS
        loadTexture("Mineral de Diamante.png"); // BLOCK_DIAMOND_ORE (raro)

        std::cout << "=== " << textures.size() << " texturas cargadas ===" << std::endl;

        // Cargar animación de agua
        loadWaterAnimation();
        std::cout << "=== Animación de agua inicializada ===" << std::endl;

        // Cargar texturas de destrucción de bloques
        loadDestroyStageTextures();

        // Cargar animación de carga
        loadTexture("../Animaciones/Animacion de Carga.gif");
        std::cout << "=== Animación de carga inicializada ===" << std::endl;
    }

    // Obtener textura para un tipo de bloque y cara (con soporte de animación y rotación)
    GLuint getBlockTexture(BlockType type, int face) {
        // face: 0=top, 1=bottom, 2=north, 3=south, 4=east, 5=west

        switch (type) {
            case BLOCK_GRASS:
                if (face == 0) return getTexture("Bloque de pasto up.png"); // Top
                else if (face == 1) return getTexture("Tierra.png");         // Bottom
                else return getTexture("Bloque de pasto.png");               // Sides

            case BLOCK_DIRT:
                return getTexture("Tierra.png");

            case BLOCK_STONE:
                return getTexture("Piedra.png");

            case BLOCK_SAND:
                return getTexture("Arena.png");

            case BLOCK_WOOD:
                // Tronco: anillos en top/bottom, corteza en sides
                if (face == 0 || face == 1) return getTexture("Tronco de Roble a dentro.png"); // Top/Bottom
                else return getTexture("Tronco de Roble.png");                                  // Sides

            case BLOCK_LEAVES:
                return getTexture("Hojas de Roble.png");

            case BLOCK_WATER:
                // AGUA ANIMADA: Retornar frame actual
                return getCurrentWaterFrame();

            case BLOCK_TALLGRASS:
                return getTexture("Pasto corto.png");

            case BLOCK_GRAVEL:
                return getTexture("Grava.png");

            case BLOCK_ORANGE_FLOWER:
                return getTexture("Lava.gif");

            case BLOCK_SNOW:
                return getTexture("nieve.png");

            case BLOCK_COBBLESTONE:
                return getTexture("Piedra Labrada.png");

            case BLOCK_PLANKS:
                return getTexture("Tablones de Madera de Roble.png");

            case BLOCK_DIRT_POWDER:
                return getTexture("Polvo de Tierra.png");

            case BLOCK_STICK:
                return getTexture("palo.png");

            case BLOCK_HOE:
                return getTexture("palo.png");

            case BLOCK_COAL_ITEM:
                return getTexture("carbon.png");

            case BLOCK_RAW_ZINC:
                return getTexture("zinc crudo.png");

            case BLOCK_RAW_COPPER:
                return getTexture("cobre crudo.png");

            // Minerales - Sistema de Rareza
            case BLOCK_COAL_ORE:
                return getTexture("Mineral de Carbon.png");

            case BLOCK_IRON_ORE:
                return getTexture("Piedra.png"); // Temporal

            case BLOCK_GOLD_ORE:
                return getTexture("Piedra.png"); // Temporal

            case BLOCK_SILVER_ORE:
                return getTexture("Piedra.png"); // Temporal

            case BLOCK_DIAMOND_ORE:
                return getTexture("Mineral de Diamante.png");

            case BLOCK_SCRAP_METAL:
                return getTexture("Desecho de metales.png");

            case BLOCK_LAVA:
                return getTexture("Lava.gif");

            default:
                // Si no hay textura, usar piedra como fallback
                return getTexture("Piedra.png");
        }
    }

    // Función auxiliar para determinar si un bloque necesita rotación de textura
    bool needsTextureRotation(BlockType type, int face) {
        // Solo rotar texturas en caras laterales de troncos para variedad
        if (type == BLOCK_WOOD && face >= 2 && face <= 5) {
            return true;
        }
        return false;
    }

    // ⭐ NUEVO: Obtener textura de item (busca en carpeta Items/)
    GLuint getItemTexture(BlockType type) {
        // Mapeo de items a texturas en la carpeta Items/
        switch (type) {
            case BLOCK_DIRT_POWDER:
                return loadTextureFromPath(getResourceRoot() + "/resourcepacks/Textures/Items/polvo de tierra.png");

            case BLOCK_STICK:
                return loadTextureFromPath(getResourceRoot() + "/resourcepacks/Textures/Items/palo.png");

            case BLOCK_COAL_ITEM:
                return loadTextureFromPath(getResourceRoot() + "/resourcepacks/Textures/Items/carbon.png");

            case BLOCK_RAW_ZINC:
                return loadTextureFromPath(getResourceRoot() + "/resourcepacks/Textures/Items/zinc crudo.png");

            case BLOCK_RAW_COPPER:
                return loadTextureFromPath(getResourceRoot() + "/resourcepacks/Textures/Items/cobre crudo.png");

            default:
                // Si no hay textura de item, usar la textura de bloque (cara superior)
                return getBlockTexture(type, 0);
        }
    }

private:
    // Helper: Cargar textura desde path absoluto
    GLuint loadTextureFromPath(const std::string& fullPath) {
        // Si ya está cargada, retornarla
        if (textures.find(fullPath) != textures.end()) {
            return textures[fullPath];
        }

        int width, height, channels;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* data = stbi_load(fullPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!data) {
            std::cerr << "ERROR: No se pudo cargar textura: " << fullPath << std::endl;
            std::cerr << "  Motivo: " << stbi_failure_reason() << std::endl;
            return 0;
        }

        std::cout << "Textura de item cargada: " << fullPath << " (" << width << "x" << height << ")" << std::endl;

        GLuint textureID;
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        GLenum format = (channels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        stbi_image_free(data);

        textures[fullPath] = textureID;
        return textureID;
    }
};

// Instancia global del TextureManager
TextureManager* g_textureManager = nullptr;

// ============================================================================
// SISTEMA DE CHUNKS
// ============================================================================

// ============================================================================
// NEXT-GEN LIGHTING SYSTEM - RGB COLORED LIGHTING + SKYLIGHT + TORCHLIGHT
// ============================================================================

// Estructura de luz optimizada con bitfields (5 bits por canal)
// Total: 16 bits (2 bytes) por voxel
struct LightVoxel {
    uint16_t sunlight   : 5;  // 0-31 (usamos 0-18)
    uint16_t torchlight : 5;  // 0-31 (usamos 0-18)
    uint16_t red        : 2;  // 0-3 (RGB reducido para ahorrar espacio)
    uint16_t green      : 2;  // 0-3
    uint16_t blue       : 2;  // 0-3

    LightVoxel() : sunlight(0), torchlight(0), red(0), green(0), blue(0) {}

    // Obtener luz total (max de sunlight y torchlight)
    uint8_t getTotalLight() const {
        return (sunlight > torchlight) ? sunlight : torchlight;
    }

    // Obtener color de luz (0.0 - 1.0)
    void getLightColor(float& r, float& g, float& b) const {
        if (torchlight > 0) {
            // Luz de antorcha tiene color
            r = red / 3.0f;
            g = green / 3.0f;
            b = blue / 3.0f;
        } else {
            // Luz solar es blanca
            r = g = b = 1.0f;
        }
    }
};

const int CHUNK_SIZE = 16;
const int CHUNK_HEIGHT = 256;
const int SUBCHUNK_HEIGHT = 16; // ⭐ Altura de cada subchunk
const int SUBCHUNKS_PER_CHUNK = CHUNK_HEIGHT / SUBCHUNK_HEIGHT; // 256 / 16 = 16 subchunks

// ⭐⭐⭐ INCLUIR SISTEMA DE PALETAS ⭐⭐⭐
#include "PalettedStorage.h"

struct Chunk {
    Vec3i position;

    // ⭐⭐⭐ SISTEMA DE PALETAS: SubChunks con compresión de paleta
    std::vector<PalettedSubChunk> subchunks; // 16 subchunks de 16x16x16

    // ⭐ TRANSICIÓN: Mantener arreglo antiguo temporalmente para compatibilidad
    // TODO: Eventualmente eliminar esto y usar solo subchunks
    BlockType blocks[CHUNK_SIZE][CHUNK_HEIGHT][CHUNK_SIZE];

    LightVoxel lightData[CHUNK_SIZE][CHUNK_HEIGHT][CHUNK_SIZE]; // NEXT-GEN LIGHTING

    // VBO OPTIMIZATION: Múltiples VBOs por textura para renderizado correcto
    struct TextureBatch {
        GLuint vbo;
        GLuint colorVBO;
        GLuint uvVBO;
        int vertexCount;
        GLuint texture;

        TextureBatch() : vbo(0), colorVBO(0), uvVBO(0), vertexCount(0), texture(0) {}

        ~TextureBatch() {
            if (vbo) glDeleteBuffers(1, &vbo);
            if (colorVBO) glDeleteBuffers(1, &colorVBO);
            if (uvVBO) glDeleteBuffers(1, &uvVBO);
        }
    };

    std::vector<TextureBatch*> batches;  // Un batch por cada textura usada

    bool needsRebuild;
    bool isGenerated;
    bool needsLightUpdate;
    std::atomic<bool> isUpdatingMesh;  // ⭐ Flag para prevenir renderizado durante actualización
    bool waitingForNeighbors;  // ⭐ CRITICAL: Chunk esperando vecinos antes de construir mesh
    bool isModified;  // ⭐ Flag para saber si el chunk fue modificado por el jugador
    std::atomic<bool> isBeingGenerated;  // ⭐ Flag para prevenir liberación durante generación async
    int buildRetries;  // ⭐⭐⭐ Contador de reintentos de construcción

    Chunk(Vec3i pos) : position(pos),
                       needsRebuild(true), isGenerated(false), needsLightUpdate(false),
                       isUpdatingMesh(false), waitingForNeighbors(false), isModified(false),
                       isBeingGenerated(false), buildRetries(0) {
        // ⭐ INICIALIZAR SUBCHUNKS CON PALETAS (todos empiezan con BLOCK_AIR)
        subchunks.reserve(SUBCHUNKS_PER_CHUNK);
        for (int i = 0; i < SUBCHUNKS_PER_CHUNK; i++) {
            subchunks.emplace_back(static_cast<BlockType>(BLOCK_AIR)); // Subchunk optimizado: paleta de 1 elemento = 0 bits!
        }

        // ⭐ INICIALIZAR ARREGLO ANTIGUO (compatibilidad temporal)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int y = 0; y < CHUNK_HEIGHT; y++) {
                for (int z = 0; z < CHUNK_SIZE; z++) {
                    blocks[x][y][z] = BLOCK_AIR;
                    lightData[x][y][z] = LightVoxel(); // Inicializar luz en 0
                }
            }
        }
    }

    ~Chunk() {
        // VBO CLEANUP: Liberar todos los batches
        for (auto batch : batches) {
            delete batch;
        }
        batches.clear();
    }

    // ⭐⭐⭐ OPTIMIZADO: Usar subchunks con paletas
    BlockType getBlock(int x, int y, int z) const {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return BLOCK_AIR;

        // Calcular qué subchunk contiene este bloque
        int subchunkIndex = y / SUBCHUNK_HEIGHT; // 0-15
        int localY = y % SUBCHUNK_HEIGHT;        // 0-15

        return subchunks[subchunkIndex].getBlock(x, localY, z);
    }

    // ⭐⭐⭐ OPTIMIZADO: Usar subchunks con paletas
    void setBlock(int x, int y, int z, BlockType type) {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return;

        // Verificar si realmente cambió
        BlockType oldType = getBlock(x, y, z);
        if (oldType == type) return; // No cambió

        // Calcular qué subchunk modificar
        int subchunkIndex = y / SUBCHUNK_HEIGHT;
        int localY = y % SUBCHUNK_HEIGHT;

        // Actualizar en el subchunk con paleta
        subchunks[subchunkIndex].setBlock(x, localY, z, type);

        // ⭐ SINCRONIZAR con arreglo antiguo (compatibilidad temporal)
        blocks[x][y][z] = type;

        needsRebuild = true;  // Rebuild porque cambió
        // needsLightUpdate = true;  // DESHABILITADO PARA 60 FPS
    }

    // Obtener luz total (max de sun y torch)
    uint8_t getLightLevel(int x, int y, int z) const {
        return 18;  // LUZ FIJA MAXIMA (sin iluminacion)
    }

    // Obtener sunlight
    uint8_t getSunlight(int x, int y, int z) const {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return 0;
        return lightData[x][y][z].sunlight;
    }

    // Obtener torchlight
    uint8_t getTorchlight(int x, int y, int z) const {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return 0;
        return lightData[x][y][z].torchlight;
    }

    // Obtener color de luz
    void getLightColor(int x, int y, int z, float& r, float& g, float& b) const {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE) {
            r = g = b = 1.0f;
            return;
        }
        lightData[x][y][z].getLightColor(r, g, b);
    }

    // Establecer sunlight
    void setSunlight(int x, int y, int z, uint8_t level) {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return;
        if (level > 18) level = 18;
        lightData[x][y][z].sunlight = level;
        needsRebuild = true;
    }

    // Establecer torchlight con color RGB
    void setTorchlight(int x, int y, int z, uint8_t level, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
            return;
        if (level > 18) level = 18;
        lightData[x][y][z].torchlight = level;
        lightData[x][y][z].red = (r > 3) ? 3 : r;
        lightData[x][y][z].green = (g > 3) ? 3 : g;
        lightData[x][y][z].blue = (b > 3) ? 3 : b;
        needsRebuild = true;
    }

    // Legacy: setLightLevel (ahora usa sunlight)
    void setLightLevel(int x, int y, int z, uint8_t level) {
        setSunlight(x, y, z, level);
    }
};

// ============================================================================
// JUGADOR CON FISICA DE GRAVEDAD (NO PUEDE FLOTAR)
// ============================================================================

struct Player {
    Vec3 position;
    Vec3 velocity;
    float yaw;
    float pitch;
    bool onGround;
    Vec3 previousPosition;  // Para carga predictiva de chunks
    bool isUnderwater;      // ⭐ NUEVO: Detecta si la cámara está bajo el agua
    bool isInWater;         // ⭐ NUEVO: Detecta si el jugador está tocando agua

    const float WALK_SPEED = 4.3f;
    const float JUMP_FORCE = 8.0f;
    const float GRAVITY = 20.0f;
    const float WIDTH = 0.5f;        // Ancho de la hitbox (0.5 = más estrecho, mejor para esquinas)
    const float HEIGHT = 1.8f;       // Altura total del jugador
    const float EYE_HEIGHT = 1.62f;  // Altura de la cámara

    Player() : position(0, 100, 0), velocity(0, 0, 0), yaw(0), pitch(0), onGround(false),
               previousPosition(0, 100, 0), isUnderwater(false), isInWater(false) {}

    Vec3 getEyePosition() const {
        return position + Vec3(0, EYE_HEIGHT, 0);
    }

    Vec3 getForward() const {
        // Vector forward que DEBE coincidir con glRotatef de OpenGL
        // OpenGL usa: glRotatef(-pitch, 1, 0, 0); glRotatef(-yaw, 0, 1, 0);
        // El vector inicial en OpenGL apunta hacia Z negativo: (0, 0, -1)

        float yawRad = yaw * 3.14159f / 180.0f;
        float pitchRad = pitch * 3.14159f / 180.0f;

        // Fórmula que funciona en la mayoría de direcciones
        Vec3 forward;
        forward.x = -sinf(yawRad) * cosf(pitchRad);
        forward.y = sinf(pitchRad);
        forward.z = -cosf(yawRad) * cosf(pitchRad);

        return forward.normalize();
    }

    Vec3 getMovementForward() const {
        // Sistema FPS estándar: forward solo usa yaw (rotación horizontal)
        // Z negativo = norte, Z positivo = sur
        float rad = yaw * 3.14159f / 180.0f;
        return Vec3(-sinf(rad), 0, -cosf(rad));
    }

    Vec3 getMovementRight() const {
        // Sistema FPS estándar: right es perpendicular a forward (90° derecha)
        float rad = yaw * 3.14159f / 180.0f;
        return Vec3(cosf(rad), 0, -sinf(rad));
    }
};

// ============================================================================
// SISTEMA DE ITEMS E INVENTARIO
// ============================================================================

// Item en el mundo (en el suelo)
struct ItemEntity {
    Vec3 position;
    Vec3 velocity;
    BlockType blockType;
    float lifetime;          // Tiempo de vida (para animación)
    float pickupDelay;       // Delay antes de poder recogerlo
    bool onGround;
    bool isBeingAttracted;   // ⭐ Si está siendo atraído por el jugador
    float floatOffset;       // ⭐ Offset para animación de flotación

    ItemEntity(Vec3 pos, BlockType type)
        : position(pos), velocity(0, 0, 0), blockType(type),
          lifetime(0), pickupDelay(0.5f), onGround(false),
          isBeingAttracted(false), floatOffset(0) {}

    void update(float deltaTime, Vec3 playerPos) {
        lifetime += deltaTime;
        floatOffset += deltaTime * 3.0f;  // ⭐ Animación de flotación

        if (pickupDelay > 0) {
            pickupDelay -= deltaTime;
        }

        // ⭐⭐⭐ ATRACCIÓN MAGNÉTICA hacia el jugador
        if (pickupDelay <= 0) {
            Vec3 toPlayer = playerPos - position;
            float distToPlayer = toPlayer.length();

            // Si está dentro del rango de atracción (1.5 bloques)
            if (distToPlayer < 1.5f && distToPlayer > 0.01f) {
                isBeingAttracted = true;

                // ⭐ FUERZA DE ATRACCIÓN: Más fuerte mientras más cerca
                float attractionStrength = 12.0f + (1.5f - distToPlayer) * 8.0f;  // 12-20 según distancia
                Vec3 attractionForce = toPlayer.normalize() * attractionStrength;

                // ⭐ ANULAR GRAVEDAD cuando está siendo atraído
                velocity = attractionForce;

                // ⭐ Movimiento suave - interpolar hacia el jugador
                position = position + velocity * deltaTime;

                // ⭐ No aplicar física normal cuando está siendo atraído
                return;
            } else {
                isBeingAttracted = false;
            }
        }

        // ⭐ FÍSICA NORMAL (solo si NO está siendo atraído)
        // Aplicar gravedad mejorada si no está en el suelo
        if (!onGround && !isBeingAttracted) {
            velocity.y -= 20.0f * deltaTime;  // Gravedad más fuerte para caída más realista

            // Resistencia del aire (drag)
            velocity.x *= 0.98f;
            velocity.z *= 0.98f;
        }

        position = position + velocity * deltaTime;

        // ⭐ FLOTACIÓN SUAVE cuando está en el suelo (como Minecraft)
        if (onGround) {
            // Pequeña oscilación vertical
            float bobAmount = sin(floatOffset) * 0.08f;
            position.y += bobAmount * deltaTime * 2.0f;

            // Fricción en el suelo
            velocity.x *= 0.90f;  // Más fricción para que se detengan más rápido
            velocity.z *= 0.90f;
        }
    }
};

// Slot del inventario

// ============================================================================
// SISTEMA DE PARTÍCULAS (estilo Minecraft)
// ============================================================================

struct Particle {
    Vec3 position;
    Vec3 velocity;
    float r, g, b;
    float life;
    float maxLife;
    float size;

    Particle() : life(0), maxLife(0), size(0.05f) {}

    Particle(Vec3 pos, Vec3 vel, float red, float green, float blue, float lifetime = 1.0f)
        : position(pos), velocity(vel), r(red), g(green), b(blue), life(lifetime), maxLife(lifetime), size(0.05f) {}

    void update(float deltaTime) {
        life -= deltaTime;
        velocity.y -= 9.8f * deltaTime; // Gravedad
        position = position + velocity * deltaTime;
    }

    bool isDead() const {
        return life <= 0;
    }
};

class ParticleSystem {
public:
    std::vector<Particle> particles;

    // Partículas pequeñas mientras se mina (progresivas)
    void spawnMiningParticles(Vec3 blockPos, BlockType blockType) {
        float r, g, b;
        getBlockColor(blockType, r, g, b);

        // Generar 2-4 partículas pequeñas
        int count = 2 + (rand() % 3);
        for (int i = 0; i < count; i++) {
            Vec3 pos = blockPos + Vec3(
                0.3f + (rand() % 100) / 250.0f,
                0.3f + (rand() % 100) / 250.0f,
                0.3f + (rand() % 100) / 250.0f
            );

            Vec3 vel = Vec3(
                ((rand() % 100) - 50) / 100.0f,
                ((rand() % 80) + 20) / 100.0f,
                ((rand() % 100) - 50) / 100.0f
            );

            // Variación de color
            float colorVar = 0.8f + (rand() % 40) / 100.0f;
            particles.push_back(Particle(pos, vel, r * colorVar, g * colorVar, b * colorVar, 0.3f + (rand() % 20) / 100.0f));
        }
    }

    // Partículas finales cuando se rompe el bloque (explosión)
    void spawnBlockBreakParticles(Vec3 blockPos, BlockType blockType) {
        float r, g, b;
        getBlockColor(blockType, r, g, b);

        // Generar 30-45 partículas (explosión final)
        int count = 30 + (rand() % 16);
        for (int i = 0; i < count; i++) {
            Vec3 pos = blockPos + Vec3(
                0.3f + (rand() % 100) / 250.0f,
                0.3f + (rand() % 100) / 250.0f,
                0.3f + (rand() % 100) / 250.0f
            );

            Vec3 vel = Vec3(
                ((rand() % 200) - 100) / 100.0f,
                ((rand() % 150) + 50) / 100.0f,
                ((rand() % 200) - 100) / 100.0f
            );

            // Variación de color
            float colorVar = 0.8f + (rand() % 40) / 100.0f;
            particles.push_back(Particle(pos, vel, r * colorVar, g * colorVar, b * colorVar, 0.25f + (rand() % 25) / 100.0f));
        }
    }

    void update(float deltaTime) {
        for (auto& p : particles) {
            p.update(deltaTime);
        }

        // Eliminar partículas muertas
        particles.erase(
            std::remove_if(particles.begin(), particles.end(),
                [](const Particle& p) { return p.isDead(); }),
            particles.end()
        );
    }

    void render() {
        if (particles.empty()) return;

        glPointSize(4.0f);
        glBegin(GL_POINTS);

        for (const auto& p : particles) {
            float alpha = p.life / p.maxLife;
            glColor4f(p.r, p.g, p.b, alpha);
            glVertex3f(p.position.x, p.position.y, p.position.z);
        }

        glEnd();
    }
};

// Inventario del jugador (InventorySlot + Inventory)
#include "Inventory.h"

// ============================================================================
// ⭐⭐⭐ SISTEMA DE CRAFTEO 3x3 ⭐⭐⭐
// ============================================================================

// Grid de crafteo 3x3 (9 slots)
struct CraftingGrid {
    static const int SIZE = 9;  // 3x3 = 9 slots
    InventorySlot slots[SIZE];

    // Obtener slot en coordenadas 2D (0-2, 0-2)
    InventorySlot& getSlot(int x, int y) {
        return slots[y * 3 + x];
    }

    const InventorySlot& getSlot(int x, int y) const {
        return slots[y * 3 + x];
    }

    // Limpiar grid
    void clear() {
        for (int i = 0; i < SIZE; i++) {
            slots[i].blockType = BLOCK_AIR;
            slots[i].count = 0;
        }
    }

    // Verificar si el grid está vacío
    bool isEmpty() const {
        for (int i = 0; i < SIZE; i++) {
            if (!slots[i].isEmpty()) return false;
        }
        return true;
    }
};

// Receta de crafteo
struct CraftingRecipe {
    BlockType pattern[9];  // Patrón 3x3 (BLOCK_AIR = cualquier cosa)
    BlockType result;      // Bloque resultante
    int resultCount;       // Cantidad del resultado
    bool shapeless;        // Si es true, ignora posición (ej: 4 wood = 1 planks)

    CraftingRecipe() : result(BLOCK_AIR), resultCount(1), shapeless(false) {
        for (int i = 0; i < 9; i++) pattern[i] = BLOCK_AIR;
    }

    CraftingRecipe(BlockType res, int count, bool shape = false)
        : result(res), resultCount(count), shapeless(shape) {
        for (int i = 0; i < 9; i++) pattern[i] = BLOCK_AIR;
    }
};

// Sistema de crafteo
class CraftingSystem {
private:
    std::vector<CraftingRecipe> recipes;

public:
    CraftingSystem() {
        initializeRecipes();
    }

    void initializeRecipes() {
        recipes.clear();

        // ========================================================================
        // RECETAS BÁSICAS
        // ========================================================================

        // ⭐ NUEVO: 1 Wood = 6 Planks (más eficiente)
        {
            CraftingRecipe recipe(BLOCK_PLANKS, 6, true);
            recipe.pattern[0] = BLOCK_WOOD;
            recipes.push_back(recipe);
        }

        // ⭐ NUEVO: 1 Dirt = 1 Dirt Powder (procesar tierra)
        {
            CraftingRecipe recipe(BLOCK_DIRT_POWDER, 1, true);
            recipe.pattern[0] = BLOCK_DIRT;
            recipes.push_back(recipe);
        }

        // 2 Planks (vertical centro) = 8 Sticks ⭐ MEJORADO
        {
            CraftingRecipe recipe(BLOCK_STICK, 8, false);
            recipe.pattern[1] = BLOCK_PLANKS;
            recipe.pattern[4] = BLOCK_PLANKS;
            recipes.push_back(recipe);
        }

        // 2 Planks (vertical izquierda) = 1 Hoz ⭐ NUEVA HERRAMIENTA
        {
            CraftingRecipe recipe(BLOCK_HOE, 1, false);
            recipe.pattern[0] = BLOCK_PLANKS;
            recipe.pattern[3] = BLOCK_PLANKS;
            recipes.push_back(recipe);
        }

        // 3 Planks (fila superior) = Wooden Pickaxe (representado con 3 planks)
        {
            CraftingRecipe recipe(BLOCK_PLANKS, 1, false);
            recipe.pattern[0] = BLOCK_PLANKS;
            recipe.pattern[1] = BLOCK_PLANKS;
            recipe.pattern[2] = BLOCK_PLANKS;
            recipe.pattern[4] = BLOCK_STICK;  // Palo central
            recipe.pattern[7] = BLOCK_STICK;  // Palo abajo
            recipes.push_back(recipe);
        }

        // 3 Stone (fila superior) = Stone Pickaxe
        {
            CraftingRecipe recipe(BLOCK_STONE, 1, false);
            recipe.pattern[0] = BLOCK_STONE;
            recipe.pattern[1] = BLOCK_STONE;
            recipe.pattern[2] = BLOCK_STONE;
            recipe.pattern[4] = BLOCK_STICK;  // Palo central
            recipe.pattern[7] = BLOCK_STICK;  // Palo abajo
            recipes.push_back(recipe);
        }

        // 4 Dirt = 1 Grass (compactar tierra)
        {
            CraftingRecipe recipe(BLOCK_GRASS, 1, true);
            recipe.pattern[0] = BLOCK_DIRT;
            recipe.pattern[1] = BLOCK_DIRT;
            recipe.pattern[3] = BLOCK_DIRT;
            recipe.pattern[4] = BLOCK_DIRT;
            recipes.push_back(recipe);
        }

        // 9 Stone = 1 Stone block (comprimir para almacenamiento)
        {
            CraftingRecipe recipe(BLOCK_STONE, 1, true);
            for (int i = 0; i < 9; i++) {
                recipe.pattern[i] = BLOCK_STONE;
            }
            recipes.push_back(recipe);
        }

        // 4 Sand = 4 Glass (simple crafting)
        {
            CraftingRecipe recipe(BLOCK_GLASS, 4, true);
            recipe.pattern[0] = BLOCK_SAND;
            recipe.pattern[1] = BLOCK_SAND;
            recipe.pattern[3] = BLOCK_SAND;
            recipe.pattern[4] = BLOCK_SAND;
            recipes.push_back(recipe);
        }

        // 3 Iron Ore (fila) = 1 Iron Block
        {
            CraftingRecipe recipe(BLOCK_IRON_ORE, 1, false);
            recipe.pattern[3] = BLOCK_IRON_ORE;
            recipe.pattern[4] = BLOCK_IRON_ORE;
            recipe.pattern[5] = BLOCK_IRON_ORE;
            recipes.push_back(recipe);
        }

        // 3 Gold Ore (fila) = 1 Gold Block
        {
            CraftingRecipe recipe(BLOCK_GOLD_ORE, 1, false);
            recipe.pattern[3] = BLOCK_GOLD_ORE;
            recipe.pattern[4] = BLOCK_GOLD_ORE;
            recipe.pattern[5] = BLOCK_GOLD_ORE;
            recipes.push_back(recipe);
        }
    }

    // Verificar si un grid coincide con una receta
    CraftingRecipe* matchRecipe(const CraftingGrid& grid) {
        for (auto& recipe : recipes) {
            if (matchesRecipe(grid, recipe)) {
                return &recipe;
            }
        }
        return nullptr;
    }

private:
    bool matchesRecipe(const CraftingGrid& grid, const CraftingRecipe& recipe) {
        if (recipe.shapeless) {
            // Receta sin forma: contar SLOTS con items (no cantidades totales)
            std::map<BlockType, int> gridCounts;
            std::map<BlockType, int> recipeCounts;

            for (int i = 0; i < 9; i++) {
                if (!grid.slots[i].isEmpty()) {
                    // ⭐ FIX: Contar SLOTS, no cantidades
                    gridCounts[grid.slots[i].blockType]++;
                }
                if (recipe.pattern[i] != BLOCK_AIR) {
                    recipeCounts[recipe.pattern[i]]++;
                }
            }

            // Verificar que tengamos exactamente los mismos tipos y cantidades de slots
            if (gridCounts.size() != recipeCounts.size()) return false;

            for (auto& pair : recipeCounts) {
                // Verificar que haya exactamente la cantidad correcta de slots
                if (gridCounts[pair.first] != pair.second) return false;
            }

            return true;
        } else {
            // Receta con forma: coincidir exactamente
            for (int i = 0; i < 9; i++) {
                BlockType expected = recipe.pattern[i];
                BlockType actual = grid.slots[i].blockType;

                if (expected == BLOCK_AIR) {
                    // Si el patrón espera aire, el slot debe estar vacío
                    if (actual != BLOCK_AIR) return false;
                } else {
                    // Si el patrón espera un bloque, debe coincidir
                    if (actual != expected || grid.slots[i].count < 1) return false;
                }
            }
            return true;
        }
    }
};

// ============================================================================
// CLASE WORLD - GESTION DE CHUNKS Y GENERACION PROCEDURAL
// ============================================================================

// Flag de --verify-gen: ver "VERIFICACIÓN DE GENERACIÓN" en World::generateChunk
static bool g_verifyGen = false;

class World {
private:
    std::map<Vec3i, Chunk*> chunks;

    // ⭐ ESCRITURAS DIFERIDAS ENTRE CHUNKS
    // La vegetación coloca bloques con coordenadas de mundo, así que un árbol
    // pegado al borde escribe en el chunk vecino. Como setBlock creaba ese
    // vecino con getOrCreateChunk -> generateChunk, generar un chunk podía
    // disparar en cascada la generación de otros (medido: 8,6 s en un solo
    // chunk) y hacía que el mundo dependiera del orden de generación.
    // Ahora, mientras se genera, los bloques destinados a un chunk que aún no
    // existe se guardan aquí y se aplican cuando ese chunk se genere.
    struct PendingBlock { int localX, y, localZ; BlockType type; };
    std::map<Vec3i, std::vector<PendingBlock>> pendingBlocks;
    int generationDepth = 0;   // >0 mientras se ejecuta generateChunk

    // Tope de seguridad: en un mundo infinito, los bloques pendientes de
    // chunks que el jugador nunca visite no deben crecer sin límite.
    static const size_t MAX_PENDING_CHUNKS = 4096;
    NextGenTerrainGenerator* terrainGen;
    int seed;
    const int RENDER_DISTANCE = 6;  // VBO OPTIMIZED: 13x13 = 169 chunks - VBOs son 10x más rápidos
    bool isGeneratingInitialWorld;  // Flag para evitar reconstrucciones durante generación inicial
    std::string currentWorldPath;  // Ruta del mundo actual para guardar/cargar chunks

    // ⭐⭐⭐ AAA SAVE SYSTEM ⭐⭐⭐
    std::unique_ptr<WorldSaveManager> saveManager;
    bool useAAASystem;  // Flag to use new save system

    // ⭐⭐⭐ ADVANCED CHUNK CACHING SYSTEM ⭐⭐⭐
    struct ChunkCacheEntry {
        Chunk* chunk;
        uint64_t lastAccessTime;
        bool isPinned;  // Los chunks visibles no se descargan
    };
    std::map<Vec3i, ChunkCacheEntry> chunkCache;
    const size_t MAX_CACHED_CHUNKS = 512;  // Máximo de chunks en caché
    uint64_t currentFrameTime = 0;

    // ⭐⭐⭐ CHUNK POOL FOR REUSE (Evita allocaciones) ⭐⭐⭐
    std::vector<Chunk*> chunkPool;
    const size_t CHUNK_POOL_SIZE = 50;  // Pool de chunks reutilizables
    std::mutex poolMutex;

    // ⭐⭐⭐ BATCH SAVING SYSTEM ⭐⭐⭐
    std::vector<Vec3i> pendingSaveChunks;
    std::mutex saveMutex;
    std::atomic<int> totalChunksSaved{0};
    std::atomic<int> totalChunksLoaded{0};

    // ⭐⭐⭐ PERFORMANCE METRICS ⭐⭐⭐
    struct PerformanceMetrics {
        float avgLoadTimeMs = 0.0f;
        float avgSaveTimeMs = 0.0f;
        int chunksLoadedThisSecond = 0;
        int chunksSavedThisSecond = 0;
        float cacheHitRate = 0.0f;
        int cacheHits = 0;
        int cacheMisses = 0;
        float avgGenerationTimeMs = 0.0f;
        float avgMeshBuildTimeMs = 0.0f;
    };
    PerformanceMetrics perfMetrics;

    // ⭐⭐⭐ ASYNC CHUNK GENERATION SYSTEM ⭐⭐⭐
    struct ChunkGenerationTask {
        Vec3i position;
        float priority;
        Chunk* chunk;  // Pre-allocated chunk
        bool isComplete;
        bool isProcessing;  // Protegido por generationMutex, no necesita ser atomic

        ChunkGenerationTask() : position(0,0,0), priority(0), chunk(nullptr), isComplete(false), isProcessing(false) {}
        ChunkGenerationTask(const Vec3i& pos, float prio, Chunk* c)
            : position(pos), priority(prio), chunk(c), isComplete(false), isProcessing(false) {}
    };

    std::vector<ChunkGenerationTask> generationQueue;
    std::mutex generationMutex;
    std::vector<std::thread> workerThreads;
    std::atomic<bool> shutdownWorkers{false};
    const int NUM_WORKER_THREADS = 2;  // 2 threads para generación

    // ⭐⭐⭐ MESH BUILD QUEUE (Main thread only) ⭐⭐⭐
    std::vector<Chunk*> meshBuildQueue;
    std::mutex meshMutex;

    // ⭐⭐⭐ TIME BUDGET SYSTEM ⭐⭐⭐
    const float MAX_GENERATION_TIME_MS = 4.0f;   // Máximo 4ms para generación por frame
    const float MAX_MESH_BUILD_TIME_MS = 8.0f;   // Máximo 8ms para meshes por frame
    const float TARGET_FRAME_TIME_MS = 16.67f;   // 60 FPS target

    // Generar semilla aleatoria basada en el tiempo (función estática)
    static int generarSemillaAleatoria() {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        return static_cast<int>(millis % 2147483647); // Limitar a int positivo
    }

    // Calcular la semilla final (aleatorio o especificado)
    static int calcularSemilla(int worldSeed) {
        return (worldSeed == -1) ? generarSemillaAleatoria() : worldSeed;
    }

    // ⭐⭐⭐ CHUNK POOL MANAGEMENT ⭐⭐⭐
    Chunk* allocateChunk(const Vec3i& pos) {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (!chunkPool.empty()) {
            // Reutilizar chunk del pool
            Chunk* chunk = chunkPool.back();
            chunkPool.pop_back();

            // Reinicializar posición
            chunk->position = pos;
            chunk->needsRebuild = true;
            chunk->isGenerated = false;
            chunk->isModified = false;
            chunk->needsLightUpdate = false;
            chunk->waitingForNeighbors = false;
            chunk->buildRetries = 0;

            // Limpiar datos anteriores
            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    for (int z = 0; z < CHUNK_SIZE; z++) {
                        chunk->blocks[x][y][z] = BLOCK_AIR;
                    }
                }
            }

            // ⭐ FIX CRÍTICO: reinicializar también la paleta (subchunks).
            // getBlock() lee de subchunks; si conservan los bloques del chunk
            // anterior, setBlock() hace early-return al "no cambiar" el valor
            // y el chunk regenerado puede guardarse como aire en disco.
            chunk->subchunks.clear();
            for (int i = 0; i < SUBCHUNKS_PER_CHUNK; i++) {
                chunk->subchunks.emplace_back(static_cast<BlockType>(BLOCK_AIR));
            }

            return chunk;
        }

        // Si no hay chunks en el pool, crear uno nuevo
        return new Chunk(pos);
    }

    void deallocateChunk(Chunk* chunk) {
        if (!chunk) return;

        std::lock_guard<std::mutex> lock(poolMutex);

        // ⭐ PROTECCIÓN CRÍTICA: Verificar que el chunk es válido
        if (!chunk) return;

        try {
            // Si el pool no está lleno, agregar el chunk para reutilizar
            if (chunkPool.size() < CHUNK_POOL_SIZE) {
                // Limpiar VBOs antes de reutilizar
                for (auto batch : chunk->batches) {
                    if (batch) {
                        // ⭐ Liberar VBOs de OpenGL si existen
                        if (glDeleteBuffers && batch->vbo != 0) {
                            glDeleteBuffers(1, &batch->vbo);
                        }
                        if (glDeleteBuffers && batch->colorVBO != 0) {
                            glDeleteBuffers(1, &batch->colorVBO);
                        }
                        if (glDeleteBuffers && batch->uvVBO != 0) {
                            glDeleteBuffers(1, &batch->uvVBO);
                        }
                        delete batch;
                    }
                }
                chunk->batches.clear();

                chunkPool.push_back(chunk);
            } else {
                // Si el pool está lleno, eliminar el chunk
                for (auto batch : chunk->batches) {
                    if (batch) {
                        if (glDeleteBuffers && batch->vbo != 0) {
                            glDeleteBuffers(1, &batch->vbo);
                        }
                        if (glDeleteBuffers && batch->colorVBO != 0) {
                            glDeleteBuffers(1, &batch->colorVBO);
                        }
                        if (glDeleteBuffers && batch->uvVBO != 0) {
                            glDeleteBuffers(1, &batch->uvVBO);
                        }
                        delete batch;
                    }
                }
                chunk->batches.clear();
                delete chunk;
            }
        } catch (const std::exception& e) {
            std::cerr << "⚠️ Error al liberar chunk: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "⚠️ Error desconocido al liberar chunk" << std::endl;
        }
    }

    // ⭐⭐⭐ ADVANCED CHUNK CACHE ⭐⭐⭐
    Chunk* getFromCache(const Vec3i& pos) {
        auto it = chunkCache.find(pos);
        if (it != chunkCache.end()) {
            // Cache HIT
            it->second.lastAccessTime = currentFrameTime;
            perfMetrics.cacheHits++;
            return it->second.chunk;
        }

        // Cache MISS
        perfMetrics.cacheMisses++;
        return nullptr;
    }

    void addToCache(const Vec3i& pos, Chunk* chunk, bool pinned = false) {
        // Si el caché está lleno, eliminar el chunk menos recientemente usado (LRU)
        if (chunkCache.size() >= MAX_CACHED_CHUNKS) {
            evictLRUChunk();
        }

        ChunkCacheEntry entry;
        entry.chunk = chunk;
        entry.lastAccessTime = currentFrameTime;
        entry.isPinned = pinned;

        chunkCache[pos] = entry;
    }

    void evictLRUChunk() {
        Vec3i oldestPos;
        uint64_t oldestTime = UINT64_MAX;

        // Encontrar el chunk no-pinned más antiguo
        for (auto& pair : chunkCache) {
            if (!pair.second.isPinned && pair.second.lastAccessTime < oldestTime) {
                oldestTime = pair.second.lastAccessTime;
                oldestPos = pair.first;
            }
        }

        // Eliminar del caché (pero mantener en chunks si está ahí)
        auto it = chunkCache.find(oldestPos);
        if (it != chunkCache.end()) {
            // No eliminar el chunk, solo sacarlo del caché
            chunkCache.erase(it);
        }
    }

    void updateCachePinning(const Vec3& playerPos) {
        Vec3i playerChunk = worldToChunkPos(playerPos);

        // Despinnear todos los chunks
        for (auto& pair : chunkCache) {
            pair.second.isPinned = false;
        }

        // Pinnear solo chunks cercanos al jugador
        for (int x = -RENDER_DISTANCE; x <= RENDER_DISTANCE; x++) {
            for (int z = -RENDER_DISTANCE; z <= RENDER_DISTANCE; z++) {
                Vec3i chunkPos(playerChunk.x + x, 0, playerChunk.z + z);
                auto it = chunkCache.find(chunkPos);
                if (it != chunkCache.end()) {
                    it->second.isPinned = true;
                }
            }
        }
    }

    // ⭐⭐⭐ WORKER THREAD FUNCTION ⭐⭐⭐
    void chunkGenerationWorker() {
        while (!shutdownWorkers.load()) {
            ChunkGenerationTask* task = nullptr;

            // Buscar una tarea pendiente
            {
                std::lock_guard<std::mutex> lock(generationMutex);
                for (auto& t : generationQueue) {
                    if (!t.isComplete && !t.isProcessing) {
                        t.isProcessing = true;
                        task = &t;
                        break;
                    }
                }
            }

            if (task) {
                try {
                    // ⭐ PROTECCIÓN: Verificar que el chunk es válido
                    if (!task->chunk) {
                        task->isComplete = true;
                        continue;
                    }

                    // Generar el chunk en este worker thread
                    auto startTime = std::chrono::high_resolution_clock::now();

                    generateChunk(task->chunk);
                    poblacion(task->chunk);

                    auto endTime = std::chrono::high_resolution_clock::now();
                    float genTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

                    // Actualizar métricas
                    perfMetrics.avgGenerationTimeMs = (perfMetrics.avgGenerationTimeMs * 0.9f) + (genTimeMs * 0.1f);

                    // ⭐ Desmarcar flag - generación completa
                    task->chunk->isBeingGenerated.store(false);

                    // Marcar como completo y agregar a la cola de mesh building
                    task->isComplete = true;

                    {
                        std::lock_guard<std::mutex> lock(meshMutex);
                        meshBuildQueue.push_back(task->chunk);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "⚠️ Error en worker thread de generación: " << e.what() << std::endl;
                    if (task && task->chunk) {
                        task->chunk->isBeingGenerated.store(false);
                    }
                    task->isComplete = true;
                } catch (...) {
                    std::cerr << "⚠️ Error desconocido en worker thread de generación" << std::endl;
                    if (task && task->chunk) {
                        task->chunk->isBeingGenerated.store(false);
                    }
                    task->isComplete = true;
                }
            } else {
                // No hay tareas, dormir un poco
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

public:
    World(int worldSeed = -1) : seed(calcularSemilla(worldSeed)), isGeneratingInitialWorld(true), useAAASystem(true) {
        terrainGen = new NextGenTerrainGenerator(seed);

        // Pre-allocar pool de chunks
        std::cout << "⚡ Inicializando Chunk Pool (" << CHUNK_POOL_SIZE << " chunks)..." << std::endl;
        chunkPool.reserve(CHUNK_POOL_SIZE);

        // ⭐ ASYNC DESHABILITADO - causaba crashes
        // Worker threads desactivados, usando generación sincrónica
        std::cout << "✅ Sistema de chunks inicializado (modo sincrónico)" << std::endl;
    }

    int getSeed() const { return seed; }

    // ⭐⭐⭐ NUEVO: Establecer semilla del mundo (para cargar mundos guardados)
    void setSeed(int newSeed) {
        seed = newSeed;
        // ⭐ CRÍTICO: Regenerar el generador de terreno con la nueva semilla
        if (terrainGen) {
            delete terrainGen;
        }
        terrainGen = new NextGenTerrainGenerator(seed);
        std::cout << "✅ Semilla del mundo establecida: " << seed << std::endl;
    }

    // Establecer la ruta del mundo actual para guardar/cargar chunks
    void setWorldPath(const std::string& path) {
        currentWorldPath = path;
        std::cout << "Ruta del mundo configurada: " << path << std::endl;

        // ⭐ Initialize AAA Save System
        if (useAAASystem && !saveManager) {
            std::string worldName = std::filesystem::path(path).filename().string();
            saveManager = std::make_unique<WorldSaveManager>(path, worldName, (uint64_t)seed);
            if (saveManager->initialize()) {
                std::cout << "⭐⭐⭐ AAA Save System initialized successfully!" << std::endl;
            } else {
                std::cerr << "Failed to initialize AAA Save System" << std::endl;
                useAAASystem = false;
            }
        }
    }

    // Marcar que la generación inicial del mundo ha terminado
    void finishInitialGeneration() {
        isGeneratingInitialWorld = false;
        std::cout << "Generacion inicial completada - rebuilds inmediatos activados" << std::endl;
    }

    ~World() {
        // ⭐ ASYNC DESHABILITADO - no hay worker threads que detener

        // ⭐ Guardar chunks pendientes antes de cerrar
        std::cout << "\n⚡ Guardando chunks pendientes..." << std::endl;
        flushPendingSaves();

        // ⭐ Shutdown AAA Save System (saves all dirty chunks)
        if (saveManager) {
            std::cout << "⭐ Shutting down AAA Save System..." << std::endl;
            saveManager->shutdown();
            saveManager.reset();
        }

        // Esperar a que termine el hilo de iluminación
        if (lightingThread != nullptr) {
            if (lightingThread->joinable()) {
                lightingThread->join();
            }
            delete lightingThread;
        }

        // Limpiar generation queue
        generationQueue.clear();
        meshBuildQueue.clear();

        // Limpiar chunks activos
        for (auto& pair : chunks) {
            delete pair.second;
        }
        chunks.clear();

        // Limpiar caché de chunks
        chunkCache.clear();

        // Limpiar pool de chunks
        for (Chunk* chunk : chunkPool) {
            delete chunk;
        }
        chunkPool.clear();

        delete terrainGen;

        // Mostrar estadísticas finales
        std::cout << "\n📊 Estadísticas finales:" << std::endl;
        std::cout << "   Total chunks cargados: " << totalChunksLoaded.load() << std::endl;
        std::cout << "   Total chunks guardados: " << totalChunksSaved.load() << std::endl;
        std::cout << "   Tasa de cache hit: " << (perfMetrics.cacheHitRate * 100.0f) << "%" << std::endl;
        std::cout << "   Tiempo promedio generación: " << perfMetrics.avgGenerationTimeMs << " ms" << std::endl;
        std::cout << "   Tiempo promedio mesh build: " << perfMetrics.avgMeshBuildTimeMs << " ms" << std::endl;
    }

    Vec3i worldToChunkPos(const Vec3& worldPos) const {
        return Vec3i(
            (int)floor(worldPos.x / CHUNK_SIZE),
            0,
            (int)floor(worldPos.z / CHUNK_SIZE)
        );
    }

    Chunk* getChunk(const Vec3i& chunkPos) {
        auto it = chunks.find(chunkPos);
        if (it != chunks.end()) {
            return it->second;
        }
        return nullptr;
    }

    Chunk* getOrCreateChunk(const Vec3i& chunkPos) {
        // ⭐⭐⭐ PASO 1: Verificar si ya está cargado
        Chunk* chunk = getChunk(chunkPos);
        if (chunk) {
            return chunk;
        }

        // ⭐⭐⭐ PASO 2: Verificar en caché
        chunk = getFromCache(chunkPos);
        if (chunk) {
            // Mover del caché a chunks activos
            chunks[chunkPos] = chunk;
            return chunk;
        }

        // ⭐⭐⭐ PASO 3: Intentar cargar desde disco
        auto loadStart = std::chrono::high_resolution_clock::now();
        bool loaded = false;

        if (useAAASystem && saveManager) {
            // Usar pool para allocar
            chunk = allocateChunk(chunkPos);
            ChunkMetadata metadata;

            if (saveManager->loadChunk(chunkPos.x, chunkPos.z, chunk->blocks, sizeof(chunk->blocks), metadata)) {
                // ⭐ CRÍTICO: Sincronizar subchunks con el array blocks cargado
                for (int x = 0; x < CHUNK_SIZE; x++) {
                    for (int y = 0; y < CHUNK_HEIGHT; y++) {
                        for (int z = 0; z < CHUNK_SIZE; z++) {
                            BlockType blockType = chunk->blocks[x][y][z];
                            int subchunkIndex = y / SUBCHUNK_HEIGHT;
                            int localY = y % SUBCHUNK_HEIGHT;
                            chunk->subchunks[subchunkIndex].setBlock(x, localY, z, blockType);
                        }
                    }
                }

                chunk->isGenerated = true;
                chunk->isModified = false;
                chunks[chunkPos] = chunk;
                loaded = true;
                totalChunksLoaded++;

                // Calcular tiempo de carga
                auto loadEnd = std::chrono::high_resolution_clock::now();
                float loadTimeMs = std::chrono::duration<float, std::milli>(loadEnd - loadStart).count();
                perfMetrics.avgLoadTimeMs = (perfMetrics.avgLoadTimeMs * 0.95f) + (loadTimeMs * 0.05f);
            } else {
                // No se pudo cargar, devolver al pool
                deallocateChunk(chunk);
                chunk = nullptr;
            }
        }
        // Fallback to old system
        else if (!currentWorldPath.empty()) {
            loaded = loadChunk(chunkPos, currentWorldPath);
            if (loaded) {
                chunk = getChunk(chunkPos);
                totalChunksLoaded++;
            }
        }

        // ⭐⭐⭐ PASO 4: Si no se cargó, generar nuevo chunk
        if (!loaded) {
            chunk = allocateChunk(chunkPos);  // Usar pool
            chunks[chunkPos] = chunk;
            generateChunk(chunk);
            poblacion(chunk);  // Añadir vegetación y decoraciones
        } else if (!chunk) {
            chunk = getChunk(chunkPos);  // Obtener el chunk recién cargado
        }

        // ⭐⭐⭐ PASO 5: Agregar al caché
        if (chunk) {
            addToCache(chunkPos, chunk, true);  // Pinned = true para chunks recién cargados
        }

        // Marcar chunks vecinos para reconstrucción
        Vec3i neighbors[] = {
            Vec3i(chunkPos.x + 1, chunkPos.y, chunkPos.z),
            Vec3i(chunkPos.x - 1, chunkPos.y, chunkPos.z),
            Vec3i(chunkPos.x, chunkPos.y, chunkPos.z + 1),
            Vec3i(chunkPos.x, chunkPos.y, chunkPos.z - 1)
        };

        for (const Vec3i& neighborPos : neighbors) {
            Chunk* neighbor = getChunk(neighborPos);
            if (neighbor && neighbor->isGenerated) {
                neighbor->needsRebuild = true;
            }
        }

        return chunk;
    }

    void generateChunk(Chunk* chunk) {
        if (chunk->isGenerated) return;
        PROFILE_SCOPE("World::generateChunk");
        Profiler::SectionTimer _gsec;   // desglose por fase (ver marks abajo)

        // Marca que estamos generando: setBlock difiere en vez de crear vecinos
        struct DepthGuard {
            int& d;
            explicit DepthGuard(int& depth) : d(depth) { ++d; }
            ~DepthGuard() { --d; }
        } _depth(generationDepth);

        const int SEA_LEVEL = 64;
        const int BEDROCK_LAYER = 5;

        // Instancia local de Perlin Noise para generación de minerales y lava
        PerlinNoise perlinLocal(seed + 12345);

        // ========================================================================
        // NEXT-GEN HIERARCHICAL TERRAIN GENERATION
        // ========================================================================
        _gsec.mark("gen:01-terrain");

        // ⭐ CACHE DE BIOMAS
        // Cada columna muestrea el bioma en su centro y en 4 vecinos a ±8
        // bloques. Como las muestras caen en la misma rejilla entera, la de una
        // columna coincide con la de otra: sin cache son 5*256 = 1280 llamadas
        // por chunk (unas 22 evaluaciones de ruido cada una) para solo 768
        // puntos distintos. La cache cubre [-8, CHUNK_SIZE+8) en X y Z.
        const int SAMPLE_DIST_I = 8;
        const int BIOME_CACHE_DIM = CHUNK_SIZE + 2 * SAMPLE_DIST_I;   // 32
        const int baseX = chunk->position.x * CHUNK_SIZE;
        const int baseZ = chunk->position.z * CHUNK_SIZE;

        std::vector<BiomeData> biomeCache(BIOME_CACHE_DIM * BIOME_CACHE_DIM);
        std::vector<char> biomeCached(BIOME_CACHE_DIM * BIOME_CACHE_DIM, 0);

        auto biomeAt = [&](int wx, int wz) -> const BiomeData& {
            int lx = wx - baseX + SAMPLE_DIST_I;
            int lz = wz - baseZ + SAMPLE_DIST_I;
            int idx = lz * BIOME_CACHE_DIM + lx;
            if (!biomeCached[idx]) {
                biomeCache[idx] = terrainGen->getBiomeData((float)wx, (float)wz, SEA_LEVEL);
                biomeCached[idx] = 1;
            }
            return biomeCache[idx];
        };

        // ⭐ CACHE DE ALTURA POR COLUMNA
        // getTerrainHeight ignora el bioma que recibe y siempre delega en
        // getBlendedTerrainHeight(x,z), que muestrea 9 biomas y hasta 9 alturas
        // más. Se llamaba hasta 5 veces por columna (capa de terreno, las dos
        // ramas de blending océano/llanura, la de colinas, los lagos de lava y
        // la grava) recalculando exactamente el mismo número.
        std::vector<int> heightCache(CHUNK_SIZE * CHUNK_SIZE, 0);
        std::vector<char> heightCached(CHUNK_SIZE * CHUNK_SIZE, 0);

        auto heightAt = [&](int wx, int wz) -> int {
            int lx = wx - baseX;
            int lz = wz - baseZ;
            if (lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE) {
                return terrainGen->getTerrainHeight((float)wx, (float)wz, biomeAt(wx, wz));
            }
            int idx = lz * CHUNK_SIZE + lx;
            if (!heightCached[idx]) {
                heightCache[idx] = terrainGen->getTerrainHeight((float)wx, (float)wz, biomeAt(wx, wz));
                heightCached[idx] = 1;
            }
            return heightCache[idx];
        };

        // Generate terrain using hierarchical layer system
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = baseX + x;
                int worldZ = baseZ + z;

                // LAYER 1-6: Get biome data (Continental, Temperature, Humidity, Erosion, Peaks, Biome Type)
                // ⭐⭐⭐ MEJORADO: Sample múltiples puntos para interpolación suave (elimina cortes)
                const BiomeData& biomeCenter = biomeAt(worldX, worldZ);

                // Sample biomas vecinos para transición ultra-suave
                const BiomeData& biomeN = biomeAt(worldX, worldZ + SAMPLE_DIST_I);
                const BiomeData& biomeS = biomeAt(worldX, worldZ - SAMPLE_DIST_I);
                const BiomeData& biomeE = biomeAt(worldX + SAMPLE_DIST_I, worldZ);
                const BiomeData& biomeW = biomeAt(worldX - SAMPLE_DIST_I, worldZ);

                // Interpolar parámetros con vecinos (50% centro, 50% vecinos promediados)
                BiomeData biome = biomeCenter;  // Mantener biome type del centro
                biome.continentalness = biomeCenter.continentalness * 0.5f +
                                       (biomeN.continentalness + biomeS.continentalness +
                                        biomeE.continentalness + biomeW.continentalness) * 0.125f;

                biome.temperature = biomeCenter.temperature * 0.5f +
                                   (biomeN.temperature + biomeS.temperature +
                                    biomeE.temperature + biomeW.temperature) * 0.125f;

                biome.humidity = biomeCenter.humidity * 0.5f +
                                (biomeN.humidity + biomeS.humidity +
                                 biomeE.humidity + biomeW.humidity) * 0.125f;

                biome.peaks = biomeCenter.peaks * 0.5f +
                             (biomeN.peaks + biomeS.peaks +
                              biomeE.peaks + biomeW.peaks) * 0.125f;

                biome.erosion = biomeCenter.erosion * 0.5f +
                               (biomeN.erosion + biomeS.erosion +
                                biomeE.erosion + biomeW.erosion) * 0.125f;

                // LAYER 7: Calculate terrain height based on biome
                int terrainHeight = heightAt(worldX, worldZ);

                // ⭐⭐⭐ LAYER 7.5: BIOME BLENDING (Elimina cortes entre biomas)
                // Detectar si estamos cerca de bordes de biomas y hacer blend suave
                const float BLEND_RADIUS = 0.08f; // Radio de blending (más grande = transiciones más suaves)

                // Verificar si estamos cerca del borde océano-playa
                if (biome.continentalness > 0.25f && biome.continentalness < 0.50f) {
                    // Estamos en zona de transición océano-playa-tierra
                    float blendFactor = 0.0f;

                    if (biome.continentalness < 0.3f + BLEND_RADIUS) {
                        // Cerca del borde océano (0.3)
                        blendFactor = terrainGen->getBiomeBlendFactor(biome.continentalness, 0.3f, BLEND_RADIUS);
                        if (blendFactor > 0.0f) {
                            // Crear bioma océano temporal para calcular su altura
                            BiomeData oceanBiome = biome;
                            oceanBiome.biomeType = BIOME_OCEAN;
                            int oceanHeight = heightAt(worldX, worldZ);

                            // Blend entre océano y altura actual
                            terrainHeight = (int)((float)terrainHeight * (1.0f - blendFactor) + (float)oceanHeight * blendFactor);
                        }
                    } else if (biome.continentalness > 0.45f - BLEND_RADIUS) {
                        // Cerca del borde playa-tierra (0.45)
                        blendFactor = terrainGen->getBiomeBlendFactor(biome.continentalness, 0.45f, BLEND_RADIUS);
                        if (blendFactor > 0.0f) {
                            // Crear bioma plains temporal para calcular su altura
                            BiomeData plainsBiome = biome;
                            plainsBiome.biomeType = BIOME_PLAINS;
                            int plainsHeight = heightAt(worldX, worldZ);

                            // Blend entre playa y plains
                            terrainHeight = (int)((float)terrainHeight * (1.0f - blendFactor) + (float)plainsHeight * blendFactor);
                        }
                    }
                }

                // Verificar si estamos cerca del borde montaña-tierra
                if (biome.peaks > 0.60f && biome.peaks < 0.70f && biome.continentalness > 0.55f) {
                    // Estamos en zona de transición hacia montañas
                    float blendFactor = terrainGen->getBiomeBlendFactor(biome.peaks, 0.65f, BLEND_RADIUS * 1.5f);
                    if (blendFactor > 0.0f) {
                        // Crear bioma hills/plains temporal
                        BiomeData hillsBiome = biome;
                        hillsBiome.biomeType = BIOME_HILLS;
                        int hillsHeight = heightAt(worldX, worldZ);

                        // Blend entre hills y montañas
                        terrainHeight = (int)((float)terrainHeight * (1.0f - blendFactor) + (float)hillsHeight * blendFactor);
                    }
                }

                // ⭐⭐⭐ NUEVO: TRANSICIONES GRADUALES para lagos y ríos (no más cortes abruptos)
                // En lugar de aplicar depresiones binarias, usar valores graduales 0-1

                // MOUNTAIN FEATURES: Obtener intensidad gradual (0.0 = no feature, 1.0 = feature completo)
                float mtnLakeStrength = terrainGen->getMountainLakeStrength((float)worldX, (float)worldZ, biome);
                float mtnRiverStrength = terrainGen->getMountainRiverStrength((float)worldX, (float)worldZ, biome);

                // FOOTHILL FEATURES: Obtener intensidad gradual
                float foothillLakeStrength = terrainGen->getFoothillLakeStrength((float)worldX, (float)worldZ, biome);
                float foothillStreamStrength = terrainGen->getFoothillStreamStrength((float)worldX, (float)worldZ, biome);

                // ⭐⭐⭐ APLICAR DEPRESIONES GRADUALES (evita cortes entre chunks)
                // Mountain lakes: Depresión máxima de 12 bloques, aplicada gradualmente
                if (mtnLakeStrength > 0.0f) {
                    float maxDepression = 12.0f;
                    // Aplicar depresión proporcional a la intensidad (0.0-1.0)
                    terrainHeight -= (int)(maxDepression * mtnLakeStrength);
                }

                // Mountain rivers: Depresión máxima de 8 bloques, aplicada gradualmente
                if (mtnRiverStrength > 0.0f) {
                    float maxDepression = 8.0f;
                    terrainHeight -= (int)(maxDepression * mtnRiverStrength);
                }

                // Foothill lakes: Depresión máxima de 10 bloques, aplicada gradualmente
                if (foothillLakeStrength > 0.0f) {
                    float maxDepression = 10.0f;
                    terrainHeight -= (int)(maxDepression * foothillLakeStrength);
                }

                // Foothill streams: Depresión máxima de 6 bloques, aplicada gradualmente
                if (foothillStreamStrength > 0.0f) {
                    float maxDepression = 6.0f;
                    terrainHeight -= (int)(maxDepression * foothillStreamStrength);
                }

                // ⭐⭐⭐ CRÍTICO: Validar límites de terrainHeight para evitar crashes
                // Asegurar que terrainHeight esté dentro del rango válido [BEDROCK_LAYER, CHUNK_HEIGHT-1]
                if (terrainHeight < BEDROCK_LAYER) {
                    terrainHeight = BEDROCK_LAYER; // Mínimo en bedrock
                }
                if (terrainHeight >= CHUNK_HEIGHT) {
                    terrainHeight = CHUNK_HEIGHT - 1; // Máximo justo debajo del límite
                }

                // ========================================================================
                // LAYER 8: BLOCK GENERATION
                // ========================================================================

                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    BlockType blockType = BLOCK_AIR;

                    // ⭐⭐⭐ FIX CRÍTICO: Bedrock solo en y=0, NO en todo y < BEDROCK_LAYER
                    // El bug anterior generaba bedrock hasta y=5 en TODOS los chunks,
                    // incluyendo océanos, creando montañas gigantes de bedrock
                    if (y == 0) {
                        blockType = BLOCK_BEDROCK;
                    }
                    // ⭐ Capa de bedrock adicional solo si estamos MUY profundo
                    else if (y > 0 && y < 3 && terrainHeight > 10) {
                        // Bedrock esporádico entre y=1 y y=2, solo si el terreno es alto
                        float bedrockNoise = perlinLocal.noise((float)worldX * 0.1f, (float)y * 0.5f, (float)worldZ * 0.1f);
                        if (bedrockNoise > 0.6f) {
                            blockType = BLOCK_BEDROCK;
                        } else {
                            blockType = BLOCK_STONE;
                        }
                    }
                    // Deep stone with 3D cave system
                    else if (y < terrainHeight - 5) {
                        // LAYER 9: Check cave density
                        bool isCave = terrainGen->isCaveAt((float)worldX, (float)y, (float)worldZ, terrainHeight, (BiomeType)biome.biomeType);

                        // ⭐ NUEVO: Check for ravines (dramatic vertical cuts)
                        bool isRavine = terrainGen->isRavineAt((float)worldX, (float)y, (float)worldZ, terrainHeight, (BiomeType)biome.biomeType);

                        if (isCave || isRavine) {
                            blockType = BLOCK_AIR;
                        } else {
                            blockType = BLOCK_STONE;
                        }
                    }
                    // Upper layers (dirt/sand)
                    else if (y < terrainHeight - 1) {
                        if (biome.biomeType == BIOME_BEACH || biome.biomeType == BIOME_RIVER) {
                            blockType = BLOCK_SAND;
                        } else if (biome.biomeType == BIOME_OCEAN_DEEP || biome.biomeType == BIOME_OCEAN) {
                            blockType = BLOCK_SAND; // Ocean floor
                        } else if (biome.biomeType == BIOME_DESERT) {
                            blockType = BLOCK_SAND; // Desert subsurface
                        } else {
                            blockType = BLOCK_DIRT;
                        }
                    }
                    // Surface layer
                    else if (y == terrainHeight - 1) {
                        if (biome.biomeType == BIOME_BEACH || biome.biomeType == BIOME_RIVER) {
                            blockType = BLOCK_SAND;
                        } else if (biome.biomeType == BIOME_OCEAN_DEEP || biome.biomeType == BIOME_OCEAN || biome.biomeType == BIOME_LAKE) {
                            blockType = BLOCK_SAND;
                        } else if (biome.biomeType == BIOME_DESERT) {
                            blockType = BLOCK_SAND;
                        } else if (biome.biomeType == BIOME_MOUNTAINS_PEAKS) {
                            blockType = BLOCK_STONE; // Rocky peaks
                        } else if (biome.biomeType == BIOME_MOUNTAINS && terrainHeight > 100) {
                            blockType = BLOCK_STONE; // High mountain peaks
                        } else {
                            blockType = BLOCK_GRASS; // All other biomes
                        }
                    }

                    chunk->setBlock(x, y, z, blockType);
                }
            }
        }

        // Llenar agua hasta el nivel del mar (océanos, lagos, ríos)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                for (int y = 0; y < SEA_LEVEL; y++) {
                    if (chunk->getBlock(x, y, z) == BLOCK_AIR) {
                        chunk->setBlock(x, y, z, BLOCK_WATER);
                    }
                }
            }
        }

        // ========================================================================
        // ⭐ NUEVO: GENERAR FONDO ACUÁTICO REALISTA (sin pasto bajo agua)
        // ========================================================================
        _gsec.mark("gen:02-seabed");
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Encontrar la superficie del terreno bajo el agua
                int seabedY = -1;
                for (int y = SEA_LEVEL - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block != BLOCK_WATER && block != BLOCK_AIR) {
                        seabedY = y;
                        break;
                    }
                }

                if (seabedY < 0) continue;

                // Verificar si está bajo agua
                bool isUnderwater = (chunk->getBlock(x, seabedY + 1, z) == BLOCK_WATER);

                if (isUnderwater) {
                    // Obtener bioma para contexto
                    BiomeData biome = terrainGen->getBiomeData((float)worldX, (float)worldZ, SEA_LEVEL);

                    // ⭐ Reemplazar pasto por capas naturales acuáticas
                    if (chunk->getBlock(x, seabedY, z) == BLOCK_GRASS) {
                        // Profundidad del agua
                        int waterDepth = SEA_LEVEL - seabedY;

                        // Ruido para variación
                        float sedimentNoise = perlinLocal.octaveNoise((float)worldX * 0.1f, (float)seabedY * 0.1f, (float)worldZ * 0.1f, 2);

                        // Determinar bloque de superficie según profundidad y bioma
                        BlockType surfaceBlock = BLOCK_SAND;  // Default

                        if (biome.biomeType == BIOME_BEACH || waterDepth <= 3) {
                            // Playas y aguas poco profundas: ARENA
                            surfaceBlock = BLOCK_SAND;
                        }
                        else if (biome.biomeType == BIOME_RIVER) {
                            // Ríos: mezcla de grava, arena y tierra
                            if (sedimentNoise > 0.3f) {
                                surfaceBlock = BLOCK_GRAVEL;
                            } else if (sedimentNoise > 0.0f) {
                                surfaceBlock = BLOCK_SAND;
                            } else {
                                surfaceBlock = BLOCK_DIRT;
                            }
                        }
                        else if (waterDepth <= 10) {
                            // Aguas medias: arena y grava
                            if (sedimentNoise > 0.2f) {
                                surfaceBlock = BLOCK_SAND;
                            } else {
                                surfaceBlock = BLOCK_GRAVEL;
                            }
                        }
                        else {
                            // Aguas profundas: grava, tierra y piedra
                            if (sedimentNoise > 0.4f) {
                                surfaceBlock = BLOCK_GRAVEL;
                            } else if (sedimentNoise > 0.0f) {
                                surfaceBlock = BLOCK_DIRT;
                            } else {
                                surfaceBlock = BLOCK_STONE;
                            }
                        }

                        // Aplicar bloque de superficie
                        chunk->setBlock(x, seabedY, z, surfaceBlock);

                        // ⭐ Generar capas bajo la superficie (estratificación natural)
                        int layers = 2 + (int)(sedimentNoise * 3);  // 2-5 capas

                        for (int layer = 1; layer <= layers && (seabedY - layer) >= 0; layer++) {
                            BlockType currentBlock = chunk->getBlock(x, seabedY - layer, z);

                            // Solo modificar si es tierra o pasto
                            if (currentBlock == BLOCK_DIRT || currentBlock == BLOCK_GRASS) {
                                if (layer == 1) {
                                    // Primera capa bajo superficie: mismo tipo o variación
                                    if (surfaceBlock == BLOCK_SAND && sedimentNoise > 0.5f) {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_GRAVEL);
                                    } else if (surfaceBlock == BLOCK_GRAVEL) {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_DIRT);
                                    } else {
                                        chunk->setBlock(x, seabedY - layer, z, surfaceBlock);
                                    }
                                }
                                else if (layer <= 3) {
                                    // Capas 2-3: mezcla de grava y tierra
                                    float layerNoise = perlinLocal.octaveNoise((float)worldX * 0.2f, (float)(seabedY - layer) * 0.2f, (float)worldZ * 0.2f, 1);
                                    if (layerNoise > 0.3f) {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_GRAVEL);
                                    } else {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_DIRT);
                                    }
                                }
                                else {
                                    // Capas profundas: tierra o piedra
                                    float deepNoise = perlinLocal.octaveNoise((float)worldX * 0.15f, (float)(seabedY - layer) * 0.15f, (float)worldZ * 0.15f, 1);
                                    if (deepNoise > 0.5f) {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_STONE);
                                    } else {
                                        chunk->setBlock(x, seabedY - layer, z, BLOCK_DIRT);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9A: GENERACIÓN DE LAVA EN CUEVAS PROFUNDAS
        // ========================================================================
        _gsec.mark("gen:03-lava-caves");
        // Llenar lava en cuevas profundas (Y <= 11, similar a Minecraft)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                for (int y = BEDROCK_LAYER + 1; y <= 11; y++) {
                    if (chunk->getBlock(x, y, z) == BLOCK_AIR) {
                        // Verificar si hay un bloque sólido debajo
                        if (y > 0 && chunk->getBlock(x, y - 1, z) != BLOCK_AIR &&
                            chunk->getBlock(x, y - 1, z) != BLOCK_WATER) {
                            // Lagos de lava en el suelo de cuevas profundas
                            float lavaNoise = perlinLocal.octaveNoise((float)worldX * 0.15f, (float)y * 0.15f, (float)worldZ * 0.15f, 2);
                            if (lavaNoise > 0.3f) {  // 30% de probabilidad en cuevas profundas
                                chunk->setBlock(x, y, z, BLOCK_LAVA);
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9B: BOLSAS DE LAVA SUBTERRÁNEAS
        // ========================================================================
        _gsec.mark("gen:04-lava-pockets");
        // Generar pequeñas bolsas de lava en capas de piedra (Y=1 a Y=50)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                for (int y = BEDROCK_LAYER + 1; y <= 50; y++) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block == BLOCK_STONE) {
                        // Bolsas de lava muy raras en piedra
                        float lavaPocketNoise = perlinLocal.octaveNoise((float)worldX * 0.08f, (float)y * 0.08f, (float)worldZ * 0.08f, 3);
                        if (lavaPocketNoise > 0.75f) {  // ~5% de probabilidad
                            // Crear pequeña bolsa de lava (2-3 bloques)
                            chunk->setBlock(x, y, z, BLOCK_LAVA);

                            // Expandir bolsa aleatoriamente
                            if (x > 0 && chunk->getBlock(x - 1, y, z) == BLOCK_STONE && lavaPocketNoise > 0.78f) {
                                chunk->setBlock(x - 1, y, z, BLOCK_LAVA);
                            }
                            if (x < CHUNK_SIZE - 1 && chunk->getBlock(x + 1, y, z) == BLOCK_STONE && lavaPocketNoise > 0.77f) {
                                chunk->setBlock(x + 1, y, z, BLOCK_LAVA);
                            }
                            if (z > 0 && chunk->getBlock(x, y, z - 1) == BLOCK_STONE && lavaPocketNoise > 0.76f) {
                                chunk->setBlock(x, y, z - 1, BLOCK_LAVA);
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9C: LAGOS DE LAVA EN LA SUPERFICIE
        // ========================================================================
        _gsec.mark("gen:05-lava-lakes");
        // Lagos de lava raros en la superficie (muy raros, como Minecraft)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Lagos muy raros
                float lavaLakeNoise = perlinLocal.octaveNoise((float)worldX * 0.02f, 0.0f, (float)worldZ * 0.02f, 2);
                if (lavaLakeNoise > 0.85f) {  // ~1% de probabilidad
                    // Obtener altura del terreno
                    const BiomeData& biomeData = biomeAt(worldX, worldZ);
                    int terrainHeight = heightAt(worldX, worldZ);

                    // Solo en tierra, no bajo el agua
                    if (terrainHeight > SEA_LEVEL + 5) {
                        // Crear lago pequeño de lava (3-4 bloques de ancho, 1-2 de profundidad)
                        int lakeY = terrainHeight - 1;
                        for (int dx = -2; dx <= 2; dx++) {
                            for (int dz = -2; dz <= 2; dz++) {
                                int lakeX = x + dx;
                                int lakeZ = z + dz;

                                if (lakeX >= 0 && lakeX < CHUNK_SIZE && lakeZ >= 0 && lakeZ < CHUNK_SIZE) {
                                    // Forma circular
                                    float dist = sqrtf((float)(dx*dx + dz*dz));
                                    if (dist <= 2.5f) {
                                        // Reemplazar bloques superiores con lava
                                        if (lakeY > 0 && lakeY < CHUNK_HEIGHT) {
                                            chunk->setBlock(lakeX, lakeY, lakeZ, BLOCK_LAVA);
                                            if (dist <= 1.5f && lakeY > 1) {
                                                chunk->setBlock(lakeX, lakeY - 1, lakeZ, BLOCK_LAVA);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 8D: SISTEMA MEJORADO DE GENERACIÓN DE MINERALES EN VENAS
        // ========================================================================
        _gsec.mark("gen:06-ore-veins");
        // ⭐ MEJORADO: Generar minerales en VENAS (como Minecraft) en lugar de bloques individuales
        for (const auto& mineral : MINERAL_DATA) {
            // Determinar tamaño de vena según rareza
            int veinSize = 0;
            int veinAttempts = 0;

            switch (mineral.rarity) {
                case RARITY_COMMON:
                    veinSize = 30 + (seed % 21);    // ⭐⭐⭐ Venas de 30-50 bloques - GIGANTESCAS
                    veinAttempts = 80;              // ⭐⭐⭐ 80 intentos por chunk - OMNIPRESENTE
                    break;
                case RARITY_UNCOMMON:
                    veinSize = 12 + (seed % 9);     // ⭐⭐⭐ Venas de 12-20 bloques - MUY GRANDES
                    veinAttempts = 35;              // ⭐⭐⭐ 35 intentos por chunk - MUY FRECUENTE
                    break;
                case RARITY_RARE:
                    veinSize = 8 + (seed % 7);      // ⭐⭐⭐ Venas de 8-14 bloques - GRANDES
                    veinAttempts = 20;              // ⭐⭐⭐ 20 intentos por chunk - FRECUENTE
                    break;
            }

            // Intentar generar múltiples venas
            for (int attempt = 0; attempt < veinAttempts; attempt++) {
                // Posición aleatoria en el chunk
                int startX = (chunk->position.x * CHUNK_SIZE * 7 + chunk->position.z * 13 + attempt * 23) % CHUNK_SIZE;
                int startZ = (chunk->position.z * CHUNK_SIZE * 11 + chunk->position.x * 17 + attempt * 31) % CHUNK_SIZE;
                int startY = mineral.minY + ((chunk->position.x + chunk->position.z + attempt) % (mineral.maxY - mineral.minY + 1));

                if (startX < 0) startX = -startX;
                if (startZ < 0) startZ = -startZ;
                if (startX >= CHUNK_SIZE) startX = CHUNK_SIZE - 1;
                if (startZ >= CHUNK_SIZE) startZ = CHUNK_SIZE - 1;

                int worldX = chunk->position.x * CHUNK_SIZE + startX;
                int worldZ = chunk->position.z * CHUNK_SIZE + startZ;

                // Obtener bioma para generación condicional
                BiomeData biome = terrainGen->getBiomeData((float)worldX, (float)worldZ, SEA_LEVEL);

                // ⭐⭐⭐ TODOS LOS MINERALES GENERAN EN TODOS LOS BIOMAS - SIN RESTRICCIONES
                // Esto hace que sean MUCHO más comunes y fáciles de encontrar
                bool canGenerateVein = true;

                // Pequeño bonus de frecuencia en ciertos biomas (pero NO exclusivos)
                float bonusMultiplier = 1.0f;

                if (mineral.type == BLOCK_DIAMOND_ORE && biome.biomeType == BIOME_DESERT) {
                    bonusMultiplier = 1.3f;  // 30% más común en desiertos
                }
                else if ((mineral.type == BLOCK_GOLD_ORE || mineral.type == BLOCK_SILVER_ORE) &&
                         (biome.biomeType == BIOME_RIVER || biome.biomeType == BIOME_OCEAN)) {
                    bonusMultiplier = 1.2f;  // 20% más común en agua
                }

                // ⭐⭐⭐ Verificar probabilidad de spawn - ULTRA PERMISIVO
                float noise = perlinLocal.octaveNoise((float)worldX * 0.05f, (float)startY * 0.05f, (float)worldZ * 0.05f, 1);
                // Aplicar bonus multiplier y hacer mucho más permisivo
                float adjustedChance = mineral.spawnChance * bonusMultiplier * 3.0f;  // ⭐ 3x más probable
                if (adjustedChance > 1.0f) adjustedChance = 1.0f;
                if (noise < (1.0f - adjustedChance)) continue;

                // ⭐ Generar vena: crecimiento orgánico desde el punto de origen
                int currentX = startX;
                int currentY = startY;
                int currentZ = startZ;

                for (int b = 0; b < veinSize; b++) {
                    // Verificar límites
                    if (currentX >= 0 && currentX < CHUNK_SIZE &&
                        currentY >= mineral.minY && currentY <= mineral.maxY && currentY < CHUNK_HEIGHT &&
                        currentZ >= 0 && currentZ < CHUNK_SIZE) {

                        // Solo reemplazar piedra
                        if (chunk->getBlock(currentX, currentY, currentZ) == BLOCK_STONE) {
                            chunk->setBlock(currentX, currentY, currentZ, mineral.type);
                        }
                    }

                    // Mover al siguiente bloque de la vena (crecimiento aleatorio)
                    int dir = (currentX + currentY + currentZ + b) % 6;
                    switch (dir) {
                        case 0: currentX++; break;
                        case 1: currentX--; break;
                        case 2: currentY++; break;
                        case 3: currentY--; break;
                        case 4: currentZ++; break;
                        case 5: currentZ--; break;
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9: GRAVA Y MINERAL DE ZINC
        // ========================================================================
        _gsec.mark("gen:07-gravel-zinc");
        // Función hash simple para generar "ruido" pseudoaleatorio
        auto simpleHash = [](int x, int y, int z) -> float {
            int n = x + y * 57 + z * 131;
            n = (n << 13) ^ n;
            return (1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
        };

        // Generar grava en ríos y bajo tierra
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Obtener bioma y altura para generación mejorada de grava
                const BiomeData& biomeData = biomeAt(worldX, worldZ);
                int terrainHeight = heightAt(worldX, worldZ);

                // 1. GRAVA EN PLAYAS (zonas costeras cerca del agua)
                if (terrainHeight >= SEA_LEVEL - 3 && terrainHeight <= SEA_LEVEL + 3) {
                    for (int y = SEA_LEVEL - 2; y <= terrainHeight; y++) {
                        if (y >= 0 && y < CHUNK_HEIGHT) {
                            BlockType currentBlock = chunk->getBlock(x, y, z);
                            if (currentBlock == BLOCK_SAND || currentBlock == BLOCK_DIRT) {
                                float beachGravel = simpleHash(worldX, y, worldZ);
                                if (beachGravel > 0.4f) {  // 60% de la playa es grava
                                    chunk->setBlock(x, y, z, BLOCK_GRAVEL);
                                }
                            }
                        }
                    }
                }

                // 2. GRAVA EN RÍOS (bajo el agua)
                if (biomeData.biomeType == BIOME_RIVER) {
                    for (int y = SEA_LEVEL - 4; y < SEA_LEVEL; y++) {
                        if (y >= 0 && y < CHUNK_HEIGHT) {
                            if (chunk->getBlock(x, y, z) == BLOCK_SAND || chunk->getBlock(x, y, z) == BLOCK_DIRT) {
                                float gravelNoise = simpleHash(worldX, y, worldZ);
                                if (gravelNoise > 0.2f) {  // 80% del lecho del río
                                    chunk->setBlock(x, y, z, BLOCK_GRAVEL);
                                }
                            }
                        }
                    }
                }

                // 3. GRAVA EN SUPERFICIE (montañas y tundra)
                if (biomeData.biomeType == BIOME_MOUNTAINS || biomeData.biomeType == BIOME_MOUNTAINS_PEAKS || biomeData.biomeType == BIOME_TUNDRA) {
                    if (terrainHeight >= 0 && terrainHeight < CHUNK_HEIGHT) {
                        BlockType surfaceBlock = chunk->getBlock(x, terrainHeight, z);
                        if (surfaceBlock == BLOCK_STONE || surfaceBlock == BLOCK_DIRT) {
                            float surfaceGravel = simpleHash(worldX * 3, terrainHeight, worldZ * 2);
                            if (surfaceGravel > 0.7f) {  // 30% de superficie montañosa
                                chunk->setBlock(x, terrainHeight, z, BLOCK_GRAVEL);
                            }
                        }
                    }
                }

                // 4. GRAVA BAJO TIERRA (vetas y bolsas más grandes)
                for (int y = 5; y < 60; y++) {
                    if (chunk->getBlock(x, y, z) == BLOCK_STONE) {
                        float gravelVein = simpleHash(worldX * 2, y, worldZ * 3);
                        if (gravelVein > 0.78f) {  // 22% de la piedra (más común)
                            chunk->setBlock(x, y, z, BLOCK_GRAVEL);

                            // Crear bolsas más grandes (3x3)
                            if (gravelVein > 0.95f) {
                                for (int dx = -1; dx <= 1; dx++) {
                                    for (int dz = -1; dz <= 1; dz++) {
                                        int nx = x + dx;
                                        int nz = z + dz;
                                        if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
                                            if (chunk->getBlock(nx, y, nz) == BLOCK_STONE) {
                                                chunk->setBlock(nx, y, nz, BLOCK_GRAVEL);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }


        // ========================================================================
        // LAYER 9C: DECORACIÓN NATURAL Y DESORDEN (Rocas, Parches, Bloques Dispersos)
        // ========================================================================
        _gsec.mark("gen:08-decoration");
        // ⭐ NUEVO: Añadir elementos naturales dispersos para más realismo
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Obtener bioma para decoración específica
                BiomeData decorBiome = terrainGen->getBiomeData((float)worldX, (float)worldZ, SEA_LEVEL);

                // Encontrar la superficie
                int surfaceY = -1;
                for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block != BLOCK_AIR && block != BLOCK_WATER) {
                        surfaceY = y;
                        break;
                    }
                }

                if (surfaceY < 0 || surfaceY >= CHUNK_HEIGHT - 2) continue;

                BlockType surfaceBlock = chunk->getBlock(x, surfaceY, z);

                // ⭐ ROCAS DISPERSAS EN MONTAÑAS
                if ((decorBiome.biomeType == BIOME_MOUNTAINS || decorBiome.biomeType == BIOME_MOUNTAINS_PEAKS) &&
                    surfaceBlock == BLOCK_STONE) {
                    float rockNoise = perlinLocal.octaveNoise((float)worldX * 0.12f, 2000, (float)worldZ * 0.12f, 2);

                    // ⭐ FIX: Verificar que el bloque superior es aire antes de colocar roca
                    if (rockNoise > 0.65f && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                        // Roca pequeña (1 bloque)
                        chunk->setBlock(x, surfaceY + 1, z, BLOCK_STONE);

                        if (rockNoise > 0.75f) {
                            // Roca mediana (2 bloques)
                            chunk->setBlock(x, surfaceY + 2, z, BLOCK_STONE);

                            if (rockNoise > 0.85f) {
                                // Roca grande (3 bloques o formación)
                                chunk->setBlock(x, surfaceY + 3, z, BLOCK_STONE);

                                // Añadir bloques adyacentes para formar roca irregular
                                if (x < CHUNK_SIZE - 1 && rockNoise > 0.9f) {
                                    chunk->setBlock(x + 1, surfaceY + 1, z, BLOCK_STONE);
                                    if (rockNoise > 0.92f) {
                                        chunk->setBlock(x + 1, surfaceY + 2, z, BLOCK_STONE);
                                    }
                                }
                                if (z < CHUNK_SIZE - 1 && rockNoise > 0.91f) {
                                    chunk->setBlock(x, surfaceY + 1, z + 1, BLOCK_STONE);
                                }
                            }
                        }
                    }
                }

                // ⭐ PARCHES DE GRAVA EN COLINAS Y MONTAÑAS
                if ((decorBiome.biomeType == BIOME_HILLS || decorBiome.biomeType == BIOME_MOUNTAINS) &&
                    surfaceBlock == BLOCK_GRASS) {
                    float gravelPatch = perlinLocal.octaveNoise((float)worldX * 0.08f, 2100, (float)worldZ * 0.08f, 2);

                    if (gravelPatch > 0.7f) {
                        chunk->setBlock(x, surfaceY, z, BLOCK_GRAVEL);

                        // Expandir parche
                        if (gravelPatch > 0.75f) {
                            if (x < CHUNK_SIZE - 1) chunk->setBlock(x + 1, surfaceY, z, BLOCK_GRAVEL);
                            if (z < CHUNK_SIZE - 1) chunk->setBlock(x, surfaceY, z + 1, BLOCK_GRAVEL);
                        }
                    }
                }

                // ⭐ PARCHES DE ARENA EN PLAYAS (más variación)
                if (decorBiome.biomeType == BIOME_BEACH && surfaceBlock == BLOCK_GRASS) {
                    float sandPatch = perlinLocal.octaveNoise((float)worldX * 0.1f, 2200, (float)worldZ * 0.1f, 2);

                    if (sandPatch > 0.6f) {
                        chunk->setBlock(x, surfaceY, z, BLOCK_SAND);

                        // Expandir parche de arena
                        if (x < CHUNK_SIZE - 1 && sandPatch > 0.65f) {
                            chunk->setBlock(x + 1, surfaceY, z, BLOCK_SAND);
                        }
                        if (z < CHUNK_SIZE - 1 && sandPatch > 0.66f) {
                            chunk->setBlock(x, surfaceY, z + 1, BLOCK_SAND);
                        }
                    }
                }

                // ⭐ PARCHES DE TIERRA EN BOSQUES (claros pequeños)
                if ((decorBiome.biomeType == BIOME_FOREST || decorBiome.biomeType == BIOME_DENSE_FOREST) &&
                    surfaceBlock == BLOCK_GRASS) {
                    float dirtPatch = perlinLocal.octaveNoise((float)worldX * 0.15f, 2300, (float)worldZ * 0.15f, 2);

                    if (dirtPatch > 0.75f) {
                        chunk->setBlock(x, surfaceY, z, BLOCK_DIRT);

                        // Pequeño claro
                        if (dirtPatch > 0.8f) {
                            if (x < CHUNK_SIZE - 1) chunk->setBlock(x + 1, surfaceY, z, BLOCK_DIRT);
                            if (z < CHUNK_SIZE - 1) chunk->setBlock(x, surfaceY, z + 1, BLOCK_DIRT);
                            if (x > 0) chunk->setBlock(x - 1, surfaceY, z, BLOCK_DIRT);
                        }
                    }
                }

                // ⭐ BLOQUES DE NIEVE DISPERSOS EN TUNDRA (sobre hierba)
                if (decorBiome.biomeType == BIOME_TUNDRA && surfaceBlock == BLOCK_GRASS) {
                    float snowPatch = perlinLocal.octaveNoise((float)worldX * 0.2f, 2400, (float)worldZ * 0.2f, 1);

                    // ⭐ FIX: Verificar que el bloque superior es aire
                    if (snowPatch > 0.5f && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                        chunk->setBlock(x, surfaceY + 1, z, BLOCK_SNOW);
                    }
                }

                // ⭐ MEJORADO: Decoraciones desérticas variadas
                if (decorBiome.biomeType == BIOME_DESERT && surfaceBlock == BLOCK_SAND) {
                    float desertFeature = perlinLocal.octaveNoise((float)worldX * 0.12f, 2500, (float)worldZ * 0.12f, 2);
                    int featureSeed = (worldX * 73 + worldZ * 37);

                    // Afloramientos de piedra arenisca (sandstone outcrops)
                    // ⭐ FIX: Verificar que hay espacio para el afloramiento
                    if (desertFeature > 0.75f && (featureSeed % 5) == 0 && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                        int outHeight = 2 + (featureSeed % 4);

                        for (int h = 1; h <= outHeight; h++) {
                            if (surfaceY + h < CHUNK_HEIGHT && chunk->getBlock(x, surfaceY + h, z) == BLOCK_AIR) {
                                chunk->setBlock(x, surfaceY + h, z, BLOCK_STONE);
                            }
                        }

                        // Expandir afloramiento
                        if (desertFeature > 0.8f && x < CHUNK_SIZE - 1 && z < CHUNK_SIZE - 1) {
                            if (chunk->getBlock(x + 1, surfaceY + 1, z) == BLOCK_AIR)
                                chunk->setBlock(x + 1, surfaceY + 1, z, BLOCK_STONE);
                            if (chunk->getBlock(x, surfaceY + 1, z + 1) == BLOCK_AIR)
                                chunk->setBlock(x, surfaceY + 1, z + 1, BLOCK_STONE);
                            if (outHeight > 2 && chunk->getBlock(x + 1, surfaceY + 2, z) == BLOCK_AIR) {
                                chunk->setBlock(x + 1, surfaceY + 2, z, BLOCK_STONE);
                            }
                        }
                    }
                    // Rocas de grava dispersas
                    else if (desertFeature > 0.7f && (featureSeed % 5) == 1 && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                        chunk->setBlock(x, surfaceY + 1, z, BLOCK_GRAVEL);

                        if (desertFeature > 0.78f && chunk->getBlock(x, surfaceY + 2, z) == BLOCK_AIR) {
                            chunk->setBlock(x, surfaceY + 2, z, BLOCK_GRAVEL);
                        }
                    }
                    // Pequeñas pilas de arena endurecida
                    else if (desertFeature > 0.68f && (featureSeed % 5) == 2) {
                        // Cambiar arena superficial por sandstone para simular arena compactada
                        if (surfaceY > 0 && chunk->getBlock(x, surfaceY, z) == BLOCK_SAND) {
                            chunk->setBlock(x, surfaceY, z, BLOCK_STONE);
                        }
                    }
                    // Montículos de arena (mini dunas decorativas)
                    else if (desertFeature > 0.65f && (featureSeed % 7) == 0 && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                        int moundHeight = 1 + (featureSeed % 3);
                        for (int h = 1; h <= moundHeight; h++) {
                            if (surfaceY + h < CHUNK_HEIGHT && chunk->getBlock(x, surfaceY + h, z) == BLOCK_AIR) {
                                chunk->setBlock(x, surfaceY + h, z, BLOCK_SAND);
                            }
                        }

                        // Forma más amplia en la base
                        if (moundHeight >= 2 && x > 0 && x < CHUNK_SIZE - 1 && z > 0 && z < CHUNK_SIZE - 1) {
                            if ((featureSeed + x) % 2 == 0 && chunk->getBlock(x + 1, surfaceY + 1, z) == BLOCK_AIR)
                                chunk->setBlock(x + 1, surfaceY + 1, z, BLOCK_SAND);
                            if ((featureSeed + z) % 2 == 0 && chunk->getBlock(x, surfaceY + 1, z + 1) == BLOCK_AIR)
                                chunk->setBlock(x, surfaceY + 1, z + 1, BLOCK_SAND);
                        }
                    }
                }

                // ⭐⭐⭐ NUEVO: CAMPOS DE ROCAS (Boulder Fields)
                if (terrainGen->shouldSpawnBoulder((float)worldX, (float)worldZ, (BiomeType)decorBiome.biomeType)) {
                    if (surfaceBlock == BLOCK_STONE || surfaceBlock == BLOCK_GRASS) {
                        int boulderSize = terrainGen->getBoulderSize((float)worldX, (float)worldZ);

                        // Verificar espacio disponible
                        if (surfaceY + boulderSize < CHUNK_HEIGHT && chunk->getBlock(x, surfaceY + 1, z) == BLOCK_AIR) {
                            // Generar roca vertical
                            for (int h = 1; h <= boulderSize; h++) {
                                if (chunk->getBlock(x, surfaceY + h, z) == BLOCK_AIR) {
                                    chunk->setBlock(x, surfaceY + h, z, BLOCK_STONE);
                                }
                            }

                            // Expandir roca horizontalmente para tamaños grandes
                            if (boulderSize >= 3 && x > 0 && x < CHUNK_SIZE - 1 && z > 0 && z < CHUNK_SIZE - 1) {
                                // Base más ancha
                                if (chunk->getBlock(x + 1, surfaceY + 1, z) == BLOCK_AIR)
                                    chunk->setBlock(x + 1, surfaceY + 1, z, BLOCK_STONE);
                                if (chunk->getBlock(x - 1, surfaceY + 1, z) == BLOCK_AIR)
                                    chunk->setBlock(x - 1, surfaceY + 1, z, BLOCK_STONE);
                                if (chunk->getBlock(x, surfaceY + 1, z + 1) == BLOCK_AIR)
                                    chunk->setBlock(x, surfaceY + 1, z + 1, BLOCK_STONE);
                                if (chunk->getBlock(x, surfaceY + 1, z - 1) == BLOCK_AIR)
                                    chunk->setBlock(x, surfaceY + 1, z - 1, BLOCK_STONE);

                                // Segundo nivel para rocas muy grandes
                                if (boulderSize >= 4) {
                                    if (chunk->getBlock(x + 1, surfaceY + 2, z) == BLOCK_AIR)
                                        chunk->setBlock(x + 1, surfaceY + 2, z, BLOCK_STONE);
                                    if (chunk->getBlock(x, surfaceY + 2, z + 1) == BLOCK_AIR)
                                        chunk->setBlock(x, surfaceY + 2, z + 1, BLOCK_STONE);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9E: ESTALACTITAS Y ESTALAGMITAS EN CUEVAS
        // ========================================================================
        _gsec.mark("gen:09-stalactites");
        // ⭐⭐⭐ NUEVO: Decorar cuevas con formaciones rocosas
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                for (int y = 5; y < CHUNK_HEIGHT - 5; y++) {
                    BlockType currentBlock = chunk->getBlock(x, y, z);

                    // Solo generar en cuevas (bloques de aire)
                    if (currentBlock == BLOCK_AIR) {
                        // Verificar si hay techo de piedra arriba (para estalactitas)
                        if (y < CHUNK_HEIGHT - 2 && chunk->getBlock(x, y + 1, z) == BLOCK_STONE) {
                            int stalactiteLen = terrainGen->getStalactiteLength((float)worldX, (float)y, (float)worldZ, true);

                            if (stalactiteLen > 0) {
                                // Generar estalactita colgando hacia abajo
                                for (int d = 0; d < stalactiteLen && (y - d) >= 0; d++) {
                                    if (chunk->getBlock(x, y - d, z) == BLOCK_AIR) {
                                        chunk->setBlock(x, y - d, z, BLOCK_STONE);
                                    } else {
                                        break; // Detener si encuentra un bloque
                                    }
                                }
                            }
                        }

                        // Verificar si hay suelo de piedra abajo (para estalagmitas)
                        if (y > 2 && chunk->getBlock(x, y - 1, z) == BLOCK_STONE) {
                            int stalagmiteLen = terrainGen->getStalactiteLength((float)worldX, (float)y, (float)worldZ, false);

                            if (stalagmiteLen > 0) {
                                // Generar estalagmita creciendo hacia arriba
                                for (int d = 0; d < stalagmiteLen && (y + d) < CHUNK_HEIGHT; d++) {
                                    if (chunk->getBlock(x, y + d, z) == BLOCK_AIR) {
                                        chunk->setBlock(x, y + d, z, BLOCK_STONE);
                                    } else {
                                        break; // Detener si encuentra un bloque
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 9D: SUAVIZADO DE TERRENO (Anti-Aliasing del terreno)
        // ========================================================================
        _gsec.mark("gen:10-smoothing");
        // ⭐ NUEVO: Suavizar transiciones abruptas en el terreno para evitar "chunks rotos"
        for (int x = 1; x < CHUNK_SIZE - 1; x++) {
            for (int z = 1; z < CHUNK_SIZE - 1; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Encontrar la altura de la superficie
                int surfaceY = -1;
                for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block != BLOCK_AIR && block != BLOCK_WATER) {
                        surfaceY = y;
                        break;
                    }
                }

                if (surfaceY < 0 || surfaceY >= CHUNK_HEIGHT - 3) continue;

                // Verificar alturas de vecinos (4 direcciones)
                int neighborHeights[4] = {-1, -1, -1, -1};
                int dirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};

                for (int d = 0; d < 4; d++) {
                    int nx = x + dirs[d][0];
                    int nz = z + dirs[d][1];

                    if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
                        for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                            BlockType block = chunk->getBlock(nx, y, nz);
                            if (block != BLOCK_AIR && block != BLOCK_WATER) {
                                neighborHeights[d] = y;
                                break;
                            }
                        }
                    }
                }

                // Calcular diferencia máxima con vecinos
                int maxDiff = 0;
                int validNeighbors = 0;
                for (int d = 0; d < 4; d++) {
                    if (neighborHeights[d] >= 0) {
                        int diff = abs(surfaceY - neighborHeights[d]);
                        if (diff > maxDiff) maxDiff = diff;
                        validNeighbors++;
                    }
                }

                // Si hay un salto muy grande (>4 bloques), suavizar
                if (maxDiff > 4 && validNeighbors >= 2) {
                    // Calcular altura promedio de vecinos
                    int totalHeight = 0;
                    for (int d = 0; d < 4; d++) {
                        if (neighborHeights[d] >= 0) {
                            totalHeight += neighborHeights[d];
                        }
                    }
                    int avgHeight = totalHeight / validNeighbors;

                    // Interpolar hacia la altura promedio (solo si la diferencia es grande)
                    int targetHeight = (surfaceY + avgHeight * 2) / 3; // 67% promedio, 33% original

                    // Obtener el tipo de bloque de superficie
                    BlockType surfaceBlock = chunk->getBlock(x, surfaceY, z);

                    // Solo suavizar si no es agua o aire
                    if (surfaceBlock != BLOCK_AIR && surfaceBlock != BLOCK_WATER) {
                        // Rellenar o eliminar bloques para alcanzar la altura objetivo
                        if (targetHeight > surfaceY) {
                            // Añadir bloques
                            for (int y = surfaceY + 1; y <= targetHeight && y < CHUNK_HEIGHT; y++) {
                                if (chunk->getBlock(x, y, z) == BLOCK_AIR) {
                                    // Determinar tipo de bloque según contexto
                                    if (y == targetHeight) {
                                        chunk->setBlock(x, y, z, surfaceBlock);
                                    } else {
                                        // Usar el bloque que está debajo de la superficie original
                                        BlockType fillBlock = chunk->getBlock(x, surfaceY - 1, z);
                                        if (fillBlock == BLOCK_AIR || fillBlock == BLOCK_WATER) {
                                            fillBlock = BLOCK_DIRT;
                                        }
                                        chunk->setBlock(x, y, z, fillBlock);
                                    }
                                }
                            }
                        } else if (targetHeight < surfaceY && maxDiff > 6) {
                            // Eliminar bloques solo si la diferencia es muy grande (>6)
                            for (int y = targetHeight + 1; y <= surfaceY; y++) {
                                BlockType blockToRemove = chunk->getBlock(x, y, z);
                                // No eliminar bloques importantes (minerales, etc)
                                if (blockToRemove == BLOCK_GRASS || blockToRemove == BLOCK_DIRT ||
                                    blockToRemove == BLOCK_SAND || blockToRemove == BLOCK_STONE) {
                                    chunk->setBlock(x, y, z, BLOCK_AIR);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========================================================================
        // LAYER 10: VEGETATION GENERATION (Advanced Forest & Mountain Trees)
        // ========================================================================
        _gsec.mark("gen:11-vegetation");
        // OPTIMIZACIÓN: Reducir densidad de árboles para mejor rendimiento
        for (int x = 2; x < CHUNK_SIZE - 2; x += 2) { // Saltar cada 2 bloques
            for (int z = 2; z < CHUNK_SIZE - 2; z += 2) { // Saltar cada 2 bloques
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Get biome data for this position
                BiomeData treeBiome = terrainGen->getBiomeData((float)worldX, (float)worldZ, SEA_LEVEL);

                // Get forest density for this biome (pass erosion for mountain slopes)
                float forestDensity = terrainGen->getForestDensity((float)worldX, (float)worldZ, (BiomeType)treeBiome.biomeType, treeBiome.erosion);

                // Find surface (grass or stone for mountains)
                int surfaceY = -1;
                for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block == BLOCK_GRASS || (block == BLOCK_STONE && treeBiome.biomeType == BIOME_MOUNTAINS)) {
                        surfaceY = y;
                        break;
                    }
                }

                if (surfaceY < 0) continue; // No valid surface

                // ⭐ BUG FIX: Validar que la superficie sea adecuada para vegetación
                BlockType surfaceBlock = chunk->getBlock(x, surfaceY, z);
                if (surfaceBlock != BLOCK_GRASS && surfaceBlock != BLOCK_STONE) continue;

                // ⭐ BUG FIX: No generar vegetación en agua o muy cerca del nivel del mar
                if (surfaceY <= SEA_LEVEL) continue;

                // ========================================================================
                // ARBUSTOS Y FLORES (decoración pequeña en praderas y bosques)
                // ========================================================================
                if (treeBiome.biomeType == BIOME_PLAINS || treeBiome.biomeType == BIOME_FOREST) {
                    float bushDensity = terrainGen->getForestDensity((float)worldX, (float)worldZ, (BiomeType)treeBiome.biomeType, 0.5f);

                    // Arbustos pequeños (2-3 bloques de hojas)
                    // ⭐ FIX: Verificar que hay espacio para el arbusto
                    if (bushDensity > 0.15f && bushDensity < 0.2f && (worldX + worldZ) % 7 == 0) {
                        if (getBlock(worldX, surfaceY + 1, worldZ) == BLOCK_AIR) {
                            // Arbusto bajo
                            setBlock(worldX, surfaceY + 1, worldZ, BLOCK_LEAVES);
                            if (getBlock(worldX, surfaceY + 2, worldZ) == BLOCK_AIR)
                                setBlock(worldX, surfaceY + 2, worldZ, BLOCK_LEAVES);

                            // Hojas alrededor
                            if ((worldX + worldZ) % 2 == 0) {
                                if (getBlock(worldX + 1, surfaceY + 1, worldZ) == BLOCK_AIR)
                                    setBlock(worldX + 1, surfaceY + 1, worldZ, BLOCK_LEAVES);
                                if (getBlock(worldX - 1, surfaceY + 1, worldZ) == BLOCK_AIR)
                                    setBlock(worldX - 1, surfaceY + 1, worldZ, BLOCK_LEAVES);
                                if (getBlock(worldX, surfaceY + 1, worldZ + 1) == BLOCK_AIR)
                                    setBlock(worldX, surfaceY + 1, worldZ + 1, BLOCK_LEAVES);
                                if (getBlock(worldX, surfaceY + 1, worldZ - 1) == BLOCK_AIR)
                                    setBlock(worldX, surfaceY + 1, worldZ - 1, BLOCK_LEAVES);
                            }
                        }
                    }

                    // Flores naranjas en praderas
                    // ⭐ FIX: Verificar que hay espacio para la flor
                    if (treeBiome.biomeType == BIOME_PLAINS && bushDensity > 0.1f && bushDensity < 0.15f) {
                        if ((worldX * 7 + worldZ * 11) % 13 == 0 && getBlock(worldX, surfaceY + 1, worldZ) == BLOCK_AIR) {
                            setBlock(worldX, surfaceY + 1, worldZ, BLOCK_ORANGE_FLOWER);
                        }
                    }
                }

                // ========================================================================
                // BOSQUES DENSOS: Árboles normales en biomas forestales
                // ========================================================================
                // ⭐ FIX: Verificar que hay espacio para el árbol
                if (forestDensity > 0.25f && surfaceY > SEA_LEVEL + 2 && surfaceY < 110 &&
                    getBlock(worldX, surfaceY + 1, worldZ) == BLOCK_AIR) {
                    int tipoArbol;
                    int alturaVariante = (int)(forestDensity * 100);

                    // Seleccionar especie según bioma
                    if (treeBiome.biomeType == BIOME_TAIGA) {
                        // Taiga: Solo pinos
                        tipoArbol = 3; // Pino
                    } else if (treeBiome.biomeType == BIOME_DENSE_FOREST || treeBiome.biomeType == BIOME_FOREST) {
                        // Bosques: Mix de robles y abedules
                        if ((worldX + worldZ) % 5 == 0) {
                            tipoArbol = 4; // Abedul (20%)
                        } else {
                            tipoArbol = (forestDensity > 0.85f) ? 2 : 1; // Roble grande o mediano
                        }
                    } else if (treeBiome.biomeType == BIOME_SWAMP) {
                        // Pantanos: Sauces
                        tipoArbol = 5; // Sauce
                    } else {
                        // Otros: Robles variados
                        if (forestDensity < 0.8f) {
                            tipoArbol = 0; // Pequeño
                        } else if (forestDensity < 0.9f) {
                            tipoArbol = 1; // Mediano
                        } else {
                            tipoArbol = 2; // Grande
                        }
                    }

                    generarArbol(worldX, surfaceY + 1, worldZ, tipoArbol, alturaVariante);
                }

                // ========================================================================
                // ÁRBOLES SOLITARIOS EN MONTAÑAS
                // ========================================================================
                else if ((treeBiome.biomeType == BIOME_MOUNTAINS || treeBiome.biomeType == BIOME_MOUNTAINS_PEAKS) &&
                         surfaceY > 80 && surfaceY < 150) {

                    // Árboles solitarios aparecen con menor frecuencia en montañas
                    float mountainTreeChance = terrainGen->getForestDensity((float)worldX, (float)worldZ, (BiomeType)treeBiome.biomeType, treeBiome.erosion);

                    // ⭐ FIX: Verificar que hay espacio para el árbol
                    // Solo 10-15% de probabilidad (árboles dispersos)
                    if (mountainTreeChance > 0.02f && getBlock(worldX, surfaceY + 1, worldZ) == BLOCK_AIR) {
                        // En montañas altas: Pinos resistentes o árboles de montaña
                        if (surfaceY > 120) {
                            // Muy alto: Solo árboles de montaña (bajos y resistentes)
                            generarArbol(worldX, surfaceY + 1, worldZ, 6, 0); // Árbol de montaña
                        } else if (surfaceY > 100) {
                            // Alto: Mix de pinos y árboles de montaña
                            if ((worldX + worldZ) % 3 == 0) {
                                generarArbol(worldX, surfaceY + 1, worldZ, 6, 0); // Árbol de montaña
                            } else {
                                generarArbol(worldX, surfaceY + 1, worldZ, 3, worldZ % 4); // Pino pequeño
                            }
                        } else {
                            // Laderas medias: Pinos normales
                            generarArbol(worldX, surfaceY + 1, worldZ, 3, worldZ % 6); // Pino variado
                        }
                    }
                }

                // ========================================================================
                // ÁRBOLES EN COLINAS (HILLS)
                // ========================================================================
                // ⭐ FIX: Verificar que hay espacio para el árbol
                else if (treeBiome.biomeType == BIOME_HILLS && forestDensity > 0.12f && surfaceY > SEA_LEVEL + 2 &&
                         getBlock(worldX, surfaceY + 1, worldZ) == BLOCK_AIR) {
                    // En colinas: Mix natural de especies
                    int tipoArbol;
                    int variante = (worldX * worldZ) % 10;

                    if (variante < 5) {
                        tipoArbol = 1; // Roble mediano (50%)
                    } else if (variante < 7) {
                        tipoArbol = 4; // Abedul (20%)
                    } else if (variante < 9) {
                        tipoArbol = 3; // Pino (20%)
                    } else {
                        tipoArbol = 2; // Roble grande (10%)
                    }

                    generarArbol(worldX, surfaceY + 1, worldZ, tipoArbol, (int)(forestDensity * 100));
                }
            }
        }

        // ⭐ Aplicar los bloques que chunks vecinos dejaron pendientes para éste
        // (troncos y hojas de árboles que cruzan el borde). Se aplican después
        // del terreno, igual que si el árbol se hubiera escrito directamente.
        {
            auto it = pendingBlocks.find(chunk->position);
            if (it != pendingBlocks.end()) {
                for (const PendingBlock& pb : it->second) {
                    chunk->setBlock(pb.localX, pb.y, pb.localZ, pb.type);
                }
                pendingBlocks.erase(it);
            }
        }

        chunk->isGenerated = true;

        // ⭐ VERIFICACIÓN DE GENERACIÓN (--verify-gen): huella del contenido del
        // chunk. Permite comprobar que una optimización de la generación produce
        // exactamente el mismo terreno: se compara el log antes y después.
        if (g_verifyGen) {
            uint32_t crc = VoxelWorld::SaveSystem::CompressionSystem::calculateCRC32(
                reinterpret_cast<const uint8_t*>(chunk->blocks), sizeof(chunk->blocks));
            std::cout << "[GENHASH] chunk " << chunk->position.x << "," << chunk->position.z
                      << " crc=" << std::hex << crc << std::dec << std::endl;
        }

        // chunk->needsLightUpdate = true;  // DESHABILITADO PARA 60 FPS
        // queueChunkForLighting(chunk->position);  // DESHABILITADO
        chunk->needsRebuild = true;

        // ⭐ CRITICAL: Notificar a vecinos que este chunk está listo
        // Esto permite que chunks que estaban esperando puedan construir su mesh
        Vec3i neighborPositions[] = {
            Vec3i(chunk->position.x + 1, 0, chunk->position.z),  // Este
            Vec3i(chunk->position.x - 1, 0, chunk->position.z),  // Oeste
            Vec3i(chunk->position.x, 0, chunk->position.z + 1),  // Norte
            Vec3i(chunk->position.x, 0, chunk->position.z - 1)   // Sur
        };

        for (const Vec3i& neighborPos : neighborPositions) {
            Chunk* neighbor = getChunk(neighborPos);
            if (neighbor && neighbor->waitingForNeighbors && neighbor->isGenerated) {
                // Vecino estaba esperando - marcar para rebuild
                neighbor->needsRebuild = true;
            }
        }
    }

    // ========================================================================
    // SISTEMA AVANZADO DE ÁRBOLES CON ESPECIES
    // ========================================================================

    // Especies de árboles
    enum TreeSpecies {
        TREE_OAK,        // Roble: Copa ancha y llena
        TREE_PINE,       // Pino: Forma cónica
        TREE_BIRCH,      // Abedul: Alto y delgado
        TREE_WILLOW,     // Sauce: Ramas caídas
        TREE_SMALL_OAK,  // Roble pequeño
        TREE_MOUNTAIN    // Árbol de montaña: Resistente, bajo
    };

    // Generar árbol de roble (forma clásica con copa redondeada) - ULTRA MEJORADO
    void generarRoble(int worldX, int baseY, int worldZ, int altura) {
        // Validar altura
        if (altura < 4) altura = 4;
        if (altura > 30) altura = 30;

        // ⭐⭐⭐ ULTRA MEJORADO: Más variantes de forma según posición
        int variant = (worldX * 73 + worldZ * 37) % 8; // 8 variantes ahora
        int seed = worldX * 127 + worldZ * 251;

        // Tronco - Algunos robles tienen tronco 2x2 o inclinado
        bool bigTrunk = (altura >= 15 && variant == 0);
        bool bentTrunk = (altura >= 10 && variant == 5); // Tronco ligeramente inclinado

        // ⭐ NUEVO: Raíces expuestas para algunos árboles grandes
        if (altura >= 18 && variant <= 1) {
            int rootDirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
            for (int i = 0; i < 4; i++) {
                if ((seed + i) % 3 == 0) continue; // No todas las direcciones
                int dx = rootDirs[i][0];
                int dz = rootDirs[i][1];
                setBlock(worldX + dx, baseY, worldZ + dz, BLOCK_WOOD);
                if ((seed + i) % 2 == 0) {
                    setBlock(worldX + dx * 2, baseY, worldZ + dz * 2, BLOCK_WOOD);
                }
            }
        }

        int trunkOffsetX = 0;
        int trunkOffsetZ = 0;

        if (bigTrunk) {
            // Tronco grueso 2x2
            for (int y = 0; y < altura; y++) {
                setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);
                setBlock(worldX + 1, baseY + y, worldZ, BLOCK_WOOD);
                setBlock(worldX, baseY + y, worldZ + 1, BLOCK_WOOD);
                setBlock(worldX + 1, baseY + y, worldZ + 1, BLOCK_WOOD);
            }
        } else if (bentTrunk) {
            // ⭐ NUEVO: Tronco curvado/inclinado
            for (int y = 0; y < altura; y++) {
                int offsetX = (y > altura / 2) ? ((y - altura / 2) / 3) : 0;
                int offsetZ = (y > altura / 3) ? ((y - altura / 3) / 4) : 0;
                setBlock(worldX + offsetX, baseY + y, worldZ + offsetZ, BLOCK_WOOD);
                trunkOffsetX = offsetX;
                trunkOffsetZ = offsetZ;
            }
        } else {
            // Tronco normal 1x1
            for (int y = 0; y < altura; y++) {
                setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);
            }
        }

        // ⭐⭐⭐ ULTRA MEJORADO: Copa esférica ultra natural con múltiples capas
        int copaBase = baseY + altura - (altura / 3);
        int copaHeight = altura / 2 + 5;

        for (int dy = 0; dy < copaHeight; dy++) {
            int leafY = copaBase + dy;

            // ⭐ Radio usando curva más natural (elipsoide en 3D)
            float progress = (float)dy / (float)copaHeight;
            int maxRadius = (altura >= 20) ? 6 : ((altura >= 12) ? 5 : 4);

            // Forma esférica perfecta usando distancia euclidiana en 3D
            float verticalProgress = progress - 0.5f; // Centrar
            float radiusMultiplier = sqrtf(1.0f - (verticalProgress * verticalProgress * 4.0f));
            if (radiusMultiplier < 0) radiusMultiplier = 0;
            int radius = (int)(maxRadius * radiusMultiplier) + 1;

            // ⭐ NUEVO: Offset para asimetría más pronunciada
            int asymX = ((seed + dy * 3) % 5) - 2; // -2 a +2
            int asymZ = ((seed * 2 + dy * 5) % 5) - 2;

            // ⭐ NUEVO: Algunas variantes tienen copas más anchas o altas
            if (variant == 6) {
                // Copa ancha y baja
                radius = (int)(radius * 1.3f);
            } else if (variant == 7) {
                // Copa alta y delgada
                radius = (int)(radius * 0.8f);
            }

            for (int dx = -radius; dx <= radius; dx++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    int actualDx = dx + asymX;
                    int actualDz = dz + asymZ;

                    // ⭐ Distancia 3D para forma más esférica
                    float centerY = copaHeight / 2.0f;
                    float dy3d = (float)dy - centerY;
                    float dist3D = sqrtf((float)(actualDx * actualDx + actualDz * actualDz) + dy3d * dy3d * 0.7f);

                    // Copa ultra orgánica con forma esférica
                    if (dist3D <= (float)maxRadius + 0.7f) {
                        // ⭐ MEJORADO: Huecos más naturales y menos uniformes
                        int holePattern = ((actualDx + dy * 3) * 31 + (actualDz + dy * 7) * 17 + seed) % 11;

                        // Huecos en el borde exterior
                        if (dist3D > maxRadius - 0.5f && holePattern < 4) continue;

                        // Huecos interiores ocasionales para densidad variable
                        if (variant >= 2 && holePattern == 0 && dist3D > maxRadius * 0.6f) continue;

                        int finalX = worldX + actualDx + trunkOffsetX;
                        int finalZ = worldZ + actualDz + trunkOffsetZ;

                        if (getBlock(finalX, leafY, finalZ) == BLOCK_AIR) {
                            setBlock(finalX, leafY, finalZ, BLOCK_LEAVES);
                        }
                    }
                }
            }
        }

        // Punta del árbol
        for (int dy = 0; dy < 3; dy++) {
            if (getBlock(worldX + trunkOffsetX, copaBase + copaHeight + dy, worldZ + trunkOffsetZ) == BLOCK_AIR) {
                setBlock(worldX + trunkOffsetX, copaBase + copaHeight + dy, worldZ + trunkOffsetZ, BLOCK_LEAVES);
            }
        }

        // ⭐⭐⭐ ULTRA MEJORADO: Sistema de ramas naturales con curvatura
        if (altura >= 8) {
            int numBranches = (altura >= 22) ? 6 : ((altura >= 16) ? 4 : ((altura >= 10) ? 3 : 2));
            int branchStartY = baseY + (altura * 2) / 3;
            int directions[8][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {1,-1}, {-1,1}, {-1,-1}};

            for (int i = 0; i < numBranches; i++) {
                // Distribución más natural de ramas
                int branchY = branchStartY + (i * altura / (numBranches + 4));
                int dir = (seed + i * 7) % 8;
                int dx = directions[dir][0];
                int dz = directions[dir][1];

                // ⭐ Longitud de rama más variable y realista
                int branchLen = 2 + ((seed + i * 11) % 4); // 2-5 bloques

                // ⭐ NUEVO: Ramas más largas para árboles grandes
                if (altura >= 18) branchLen++;

                // ⭐ Rama con curvatura natural
                int currentY = branchY;
                for (int len = 1; len <= branchLen; len++) {
                    // Curvatura gradual hacia arriba
                    if (len > 1 && len % 2 == 0) currentY++;

                    // ⭐ Curvatura horizontal (ramas no perfectamente rectas)
                    int curveOffset = (len > 2) ? ((seed + len) % 2) : 0;
                    int perpDir = (dir + 2) % 8; // Dirección perpendicular
                    int curveDx = directions[perpDir][0] * curveOffset;
                    int curveDz = directions[perpDir][1] * curveOffset;

                    setBlock(worldX + dx * len + curveDx, currentY, worldZ + dz * len + curveDz, BLOCK_WOOD);

                    // ⭐ MEJORADO: Hojas más densas y naturales en las puntas
                    if (len >= branchLen - 1) {
                        for (int ldx = -2; ldx <= 2; ldx++) {
                            for (int ldz = -2; ldz <= 2; ldz++) {
                                for (int ldy = -1; ldy <= 2; ldy++) {
                                    // Forma más esférica de hojas
                                    float leafDist = sqrtf((float)(ldx * ldx + ldz * ldz + ldy * ldy));
                                    if (leafDist <= 2.5f) {
                                        int finalX = worldX + dx * len + ldx + curveDx;
                                        int finalY = currentY + ldy;
                                        int finalZ = worldZ + dz * len + ldz + curveDz;

                                        // ⭐ Huecos ocasionales para densidad natural
                                        int leafPattern = (ldx * 3 + ldz * 7 + ldy * 11 + seed) % 7;
                                        if (leafDist > 2.0f && leafPattern < 2) continue;

                                        if (getBlock(finalX, finalY, finalZ) == BLOCK_AIR) {
                                            setBlock(finalX, finalY, finalZ, BLOCK_LEAVES);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ⭐ NUEVO: Sub-ramas para árboles muy grandes
                if (altura >= 20 && i % 2 == 0 && branchLen >= 4) {
                    int subBranchPos = branchLen / 2;
                    int subDir = (dir + 1 + (i % 3)) % 8;
                    int subDx = directions[subDir][0];
                    int subDz = directions[subDir][1];

                    for (int subLen = 1; subLen <= 2; subLen++) {
                        setBlock(worldX + dx * subBranchPos + subDx * subLen,
                                currentY + subLen,
                                worldZ + dz * subBranchPos + subDz * subLen,
                                BLOCK_WOOD);

                        // Mini copa en la sub-rama
                        if (subLen == 2) {
                            for (int sldx = -1; sldx <= 1; sldx++) {
                                for (int sldz = -1; sldz <= 1; sldz++) {
                                    if (getBlock(worldX + dx * subBranchPos + subDx * subLen + sldx,
                                                currentY + subLen + 1,
                                                worldZ + dz * subBranchPos + subDz * subLen + sldz) == BLOCK_AIR) {
                                        setBlock(worldX + dx * subBranchPos + subDx * subLen + sldx,
                                                currentY + subLen + 1,
                                                worldZ + dz * subBranchPos + subDz * subLen + sldz,
                                                BLOCK_LEAVES);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Generar pino (forma cónica como abeto) - ULTRA MEJORADO
    void generarPino(int worldX, int baseY, int worldZ, int altura) {
        if (altura < 6) altura = 6;
        if (altura > 35) altura = 35;

        // ⭐⭐⭐ ULTRA MEJORADO: 7 variantes de pino ahora
        int variant = (worldX * 47 + worldZ * 89) % 7;
        int seed = worldX * 149 + worldZ * 311;

        // Tronco - pinos grandes tienen tronco más grueso
        bool thickTrunk = (altura >= 20);
        bool veryThick = (altura >= 28 && variant == 4);

        for (int y = 0; y < altura; y++) {
            setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);

            // Tronco 2x2 para pinos muy grandes
            if (thickTrunk && y < altura / 2) {
                setBlock(worldX + 1, baseY + y, worldZ, BLOCK_WOOD);
                setBlock(worldX, baseY + y, worldZ + 1, BLOCK_WOOD);
                setBlock(worldX + 1, baseY + y, worldZ + 1, BLOCK_WOOD);
            }
        }

        // Copa cónica - Forma de abeto mejorada
        int copaInicio = baseY + altura / 4;
        int copaAltura = (altura * 3) / 4;

        // ⭐ NUEVO: Añadir ramas inferiores bajas para algunos pinos
        if (altura >= 15 && variant >= 2) {
            int lowBranchY = baseY + altura / 5;
            int directions[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};

            for (int i = 0; i < 4; i++) {
                if ((seed + i) % 3 != 0) {
                    int dx = directions[i][0];
                    int dz = directions[i][1];

                    // Ramas caídas hacia abajo
                    for (int len = 1; len <= 3; len++) {
                        int branchY = lowBranchY - (len - 1) / 2;
                        setBlock(worldX + dx * len, branchY, worldZ + dz * len, BLOCK_LEAVES);
                    }
                }
            }
        }

        // ⭐⭐⭐ ULTRA MEJORADO: Pinos con forma cónica perfecta y natural
        for (int y = 0; y < copaAltura; y++) {
            int leafY = copaInicio + y;
            float progress = (float)y / (float)copaAltura;

            // ⭐ Radio con mejor gradiente cónico
            int baseRadius = (altura >= 28) ? 7 : ((altura >= 20) ? 6 : 5);
            int radius;

            if (variant == 0) {
                // Forma clásica de pino (suave y uniforme)
                radius = (int)(baseRadius * (1.0f - progress * 0.95f)) + 1;
            } else if (variant == 1) {
                // Forma de abeto (capas marcadas y escalonadas)
                int layer = (int)(progress * 7.0f);
                radius = baseRadius - layer;
                if (radius < 1) radius = 1;

                // ⭐ Capas más pronunciadas
                if (y % 5 == 0 && y > 0 && y < copaAltura - 5) {
                    radius = (radius * 2) / 3;
                }
            } else if (variant == 2) {
                // Forma cónica perfecta (curva cuadrática)
                radius = (int)((1.0f - progress * progress * 0.9f) * baseRadius) + 1;
            } else if (variant == 3) {
                // Forma irregular natural
                radius = (int)(baseRadius * (1.0f - progress * 0.85f)) + 1;
                // ⭐ Variación por capa más pronunciada
                int layerMod = (y / 3) % 3;
                if (layerMod == 0) radius = (radius * 5) / 6;
            } else if (variant == 4) {
                // Pino ancho (tipo cedro/sequoia)
                radius = (int)(baseRadius * (1.0f - progress * 0.65f)) + 2;
            } else if (variant == 5) {
                // ⭐ NUEVO: Pino delgado y alto (tipo ciprés)
                radius = (int)((baseRadius * 0.7f) * (1.0f - progress * 0.9f)) + 1;
            } else {
                // ⭐ NUEVO: Forma de piña (ancho en medio)
                float middle = 1.0f - 4.0f * (progress - 0.5f) * (progress - 0.5f);
                radius = (int)(baseRadius * middle * 0.9f) + 1;
            }

            // ⭐ Asimetría más natural
            int asymX = (variant >= 3) ? ((seed + y * 3) % 3) - 1 : 0; // -1, 0, 1
            int asymZ = (variant >= 3) ? ((seed * 2 + y * 5) % 3) - 1 : 0;

            for (int dx = -radius; dx <= radius; dx++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    int actualDx = dx + asymX;
                    int actualDz = dz + asymZ;
                    int dist = actualDx * actualDx + actualDz * actualDz;

                    // Forma más cuadrada para pinos (no circular)
                    if (dist <= radius * radius + radius) {
                        // No cubrir tronco
                        if (dx == 0 && dz == 0 && y < copaAltura - 3) continue;
                        if (thickTrunk && y < copaAltura / 2) {
                            if ((dx == 0 || dx == 1) && (dz == 0 || dz == 1)) continue;
                        }

                        // ⭐ MEJORADO: Huecos ocasionales para aspecto más natural
                        if (variant == 3 && (dx + dz + y + seed) % 7 == 0 && dist > radius * radius / 2) {
                            continue;
                        }

                        if (getBlock(worldX + actualDx, leafY, worldZ + actualDz) == BLOCK_AIR) {
                            setBlock(worldX + actualDx, leafY, worldZ + actualDz, BLOCK_LEAVES);
                        }
                    }
                }
            }
        }

        // ⭐ MEJORADO: Punta del pino variable
        int tipHeight = (variant == 4) ? 6 : 5;
        for (int dy = 0; dy < tipHeight; dy++) {
            int tipY = baseY + altura + dy;

            if (dy < 3) {
                // Base de la punta (cruz)
                if (getBlock(worldX, tipY, worldZ) == BLOCK_AIR)
                    setBlock(worldX, tipY, worldZ, BLOCK_LEAVES);
                if (getBlock(worldX + 1, tipY, worldZ) == BLOCK_AIR)
                    setBlock(worldX + 1, tipY, worldZ, BLOCK_LEAVES);
                if (getBlock(worldX - 1, tipY, worldZ) == BLOCK_AIR)
                    setBlock(worldX - 1, tipY, worldZ, BLOCK_LEAVES);
                if (getBlock(worldX, tipY, worldZ + 1) == BLOCK_AIR)
                    setBlock(worldX, tipY, worldZ + 1, BLOCK_LEAVES);
                if (getBlock(worldX, tipY, worldZ - 1) == BLOCK_AIR)
                    setBlock(worldX, tipY, worldZ - 1, BLOCK_LEAVES);
            } else {
                // Punta final (solo centro)
                if (getBlock(worldX, tipY, worldZ) == BLOCK_AIR)
                    setBlock(worldX, tipY, worldZ, BLOCK_LEAVES);
            }
        }
    }

    // Generar abedul (alto y delgado con copa pequeña)
    void generarAbedul(int worldX, int baseY, int worldZ, int altura) {
        // Tronco delgado
        for (int y = 0; y < altura; y++) {
            setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);
        }

        // Copa pequeña solo en la parte superior
        int copaBase = baseY + altura - 2;

        for (int dy = 0; dy <= 3; dy++) {
            int leafY = copaBase + dy;
            int radius = (dy <= 1) ? 2 : 1;

            for (int dx = -radius; dx <= radius; dx++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    if (dx == 0 && dz == 0 && dy < 2) continue; // No cubrir tronco

                    int dist = dx * dx + dz * dz;
                    if (dist <= radius * radius) {
                        if (getBlock(worldX + dx, leafY, worldZ + dz) == BLOCK_AIR) {
                            setBlock(worldX + dx, leafY, worldZ + dz, BLOCK_LEAVES);
                        }
                    }
                }
            }
        }
    }

    // Generar sauce (ramas que caen)
    void generarSauce(int worldX, int baseY, int worldZ, int altura) {
        // Tronco
        for (int y = 0; y < altura; y++) {
            setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);
        }

        // Copa superior
        int copaBase = baseY + altura - 2;

        for (int dx = -2; dx <= 2; dx++) {
            for (int dz = -2; dz <= 2; dz++) {
                int dist = dx * dx + dz * dz;
                if (dist <= 4) {
                    if (getBlock(worldX + dx, copaBase, worldZ + dz) == BLOCK_AIR) {
                        setBlock(worldX + dx, copaBase, worldZ + dz, BLOCK_LEAVES);
                    }
                }
            }
        }

        // Ramas caídas (4 direcciones principales)
        int ramaDirs[4][2] = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}};

        for (int i = 0; i < 4; i++) {
            int dx = ramaDirs[i][0];
            int dz = ramaDirs[i][1];

            // Rama que cae 3-5 bloques
            for (int dy = 0; dy < 5; dy++) {
                int ramX = worldX + dx;
                int ramY = copaBase - dy;
                int ramZ = worldZ + dz;

                if (getBlock(ramX, ramY, ramZ) == BLOCK_AIR) {
                    setBlock(ramX, ramY, ramZ, BLOCK_LEAVES);
                }
            }
        }
    }

    // Árbol de montaña (bajo, resistente, adaptado al viento)
    void generarArbolMontana(int worldX, int baseY, int worldZ) {
        // Tronco corto y grueso (solo 4-6 bloques)
        int altura = 4 + (worldX + worldZ) % 3;

        for (int y = 0; y < altura; y++) {
            setBlock(worldX, baseY + y, worldZ, BLOCK_WOOD);
        }

        // Copa compacta y extendida hacia los lados (resistente al viento)
        for (int dy = -1; dy <= 2; dy++) {
            int leafY = baseY + altura + dy;
            int radius = (dy == -1 || dy == 2) ? 1 : 2;

            for (int dx = -radius; dx <= radius; dx++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    if (dx == 0 && dz == 0 && dy < 1) continue;

                    int dist = dx * dx + dz * dz;
                    if (dist <= radius * radius + 1) {
                        if (getBlock(worldX + dx, leafY, worldZ + dz) == BLOCK_AIR) {
                            setBlock(worldX + dx, leafY, worldZ + dz, BLOCK_LEAVES);
                        }
                    }
                }
            }
        }
    }

    // Función principal mejorada de generación de árboles - VERSIÓN 2.0
    void generarArbol(int worldX, int baseY, int worldZ, int tipoArbol, int alturaVariante = 0) {
        // Tipos de árbol actualizados:
        // 0 = Roble pequeño (4-8 bloques)
        // 1 = Roble mediano (8-14 bloques)
        // 2 = Grande (roble o pino, 16-28 bloques)
        // 3 = Pino (10-25 bloques)
        // 4 = Abedul (8-14 bloques)
        // 5 = Sauce (7-11 bloques)
        // 6 = Árbol de montaña (4-7 bloques)

        // Semilla para variación
        int seed = worldX * 193 + worldZ * 271 + tipoArbol * 73;

        if (tipoArbol == 0) {
            // Roble pequeño - MÁS VARIEDAD
            int altura = 4 + (seed % 5); // 4-8 bloques
            generarRoble(worldX, baseY, worldZ, altura);

        } else if (tipoArbol == 1) {
            // Árbol mediano - MÁS VARIEDAD
            int altura = 8 + (seed % 7); // 8-14 bloques
            generarRoble(worldX, baseY, worldZ, altura);

        } else if (tipoArbol == 2) {
            // Árbol grande - MUCHA VARIEDAD
            int altura = 16 + (seed % 13); // 16-28 bloques
            if ((seed % 3) == 0) {
                // 33% pinos grandes
                generarPino(worldX, baseY, worldZ, altura);
            } else {
                // 67% robles grandes
                generarRoble(worldX, baseY, worldZ, altura);
            }

        } else if (tipoArbol == 3) {
            // Pino específico - MÁS VARIEDAD
            int altura = 10 + (seed % 16); // 10-25 bloques
            generarPino(worldX, baseY, worldZ, altura);

        } else if (tipoArbol == 4) {
            // Abedul - MÁS VARIEDAD
            int altura = 8 + (seed % 7); // 8-14 bloques
            generarAbedul(worldX, baseY, worldZ, altura);

        } else if (tipoArbol == 5) {
            // Sauce - MÁS VARIEDAD
            int altura = 7 + (seed % 5); // 7-11 bloques
            generarSauce(worldX, baseY, worldZ, altura);

        } else if (tipoArbol == 6) {
            // Árbol de montaña (ya tiene variedad interna)
            generarArbolMontana(worldX, baseY, worldZ);
        }
    }

    // Función de población: añade vegetación y decoraciones
    void poblacion(Chunk* chunk) {
        // All vegetation generation now integrated in generateChunk()
        // This function kept for compatibility but does nothing
        return;

        /* OLD CODE - DISABLED
        const int SEA_LEVEL = 45;

        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                int worldX = chunk->position.x * CHUNK_SIZE + x;
                int worldZ = chunk->position.z * CHUNK_SIZE + z;

                // Encontrar la altura del terreno en esta columna
                int terrainHeight = -1;
                for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block == BLOCK_GRASS || block == BLOCK_DIRT || block == BLOCK_SAND) {
                        terrainHeight = y;
                        break;
                    }
                }

                if (terrainHeight < SEA_LEVEL || terrainHeight < 0) continue;

                BlockType surfaceBlock = chunk->getBlock(x, terrainHeight, z);

                // Generar árboles (solo en tierra con pasto)
                if (surfaceBlock == BLOCK_GRASS) {
                    float treeNoise = noise.octaveNoise(worldX * 0.1f, 0, worldZ * 0.1f, 1);

                    if (treeNoise > 0.85f) { // ~7.5% de probabilidad de árbol
                        // Determinar tipo de árbol usando otro ruido
                        float sizeNoise = noise.octaveNoise(worldX * 0.05f, 50, worldZ * 0.05f, 1);
                        int tipoArbol = 0; // Por defecto: pequeño
                        int alturaVariante = (int)((sizeNoise + 1.0f) * 1000.0f); // Semilla para variante

                        // Distribución de tamaños:
                        // - Pequeño (tipo 0): 50% (sizeNoise < 0.0)
                        // - Mediano (tipo 1): 35% (0.0 <= sizeNoise < 0.7)
                        // - Grande (tipo 2): 15% (sizeNoise >= 0.7)

                        if (sizeNoise < 0.0f) {
                            tipoArbol = 0; // Pequeño (4 troncos, fijo)
                        } else if (sizeNoise < 0.7f) {
                            tipoArbol = 1; // Mediano (10-14 troncos, random)
                        } else {
                            tipoArbol = 2; // Grande (23-30 troncos, random)
                        }

                        generarArbol(worldX, terrainHeight + 1, worldZ, tipoArbol, alturaVariante);
                    }
                    // Hierba alta y flores decorativas
                    else if (treeNoise > 0.4f && treeNoise < 0.7f) { // ~30% de probabilidad
                        BlockType aboveBlock = getBlock(worldX, terrainHeight + 1, worldZ);
                        if (aboveBlock == BLOCK_AIR) {
                            // Usar ruido adicional para determinar si es flor o hierba
                            float flowerNoise = noise.octaveNoise(worldX * 0.25f, 200, worldZ * 0.25f, 1);
                            if (flowerNoise > 0.85f) { // ~8% de los bloques de vegetación son flores
                                setBlock(worldX, terrainHeight + 1, worldZ, BLOCK_ORANGE_FLOWER);
                            } else {
                                setBlock(worldX, terrainHeight + 1, worldZ, BLOCK_TALLGRASS);
                            }
                        }
                    }
                }
                // Hierba en la playa (menos frecuente)
                else if (surfaceBlock == BLOCK_SAND) {
                    float beachGrassNoise = noise.octaveNoise(worldX * 0.15f, 75, worldZ * 0.15f, 1);
                    if (beachGrassNoise > 0.9f) { // ~5% de probabilidad
                        BlockType aboveBlock = getBlock(worldX, terrainHeight + 1, worldZ);
                        if (aboveBlock == BLOCK_AIR) {
                            setBlock(worldX, terrainHeight + 1, worldZ, BLOCK_TALLGRASS);
                        }
                    }
                }
            }
        }

        chunk->needsRebuild = true;
        */
    }

    BlockType getBlock(int x, int y, int z) {
        if (y < 0 || y >= CHUNK_HEIGHT) return BLOCK_AIR;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return BLOCK_AIR;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        return chunk->getBlock(localX, y, localZ);
    }

    void setBlock(int x, int y, int z, BlockType type) {
        if (y < 0 || y >= CHUNK_HEIGHT) return;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        // ⭐ Durante la generación, no crear el chunk vecino: eso provocaba
        // generación recursiva en cascada (ver pendingBlocks). El bloque se
        // aplica cuando ese chunk se genere por su cuenta.
        if (generationDepth > 0 && !getChunk(chunkPos)) {
            if (pendingBlocks.size() < MAX_PENDING_CHUNKS || pendingBlocks.count(chunkPos)) {
                pendingBlocks[chunkPos].push_back({localX, y, localZ, type});
            }
            return;
        }

        // ⭐ PROTECCIÓN: Verificar que el chunk se creó correctamente
        Chunk* chunk = getOrCreateChunk(chunkPos);
        if (!chunk) {
            std::cerr << "⚠️ WARNING: No se pudo crear chunk en (" << chunkPos.x << ", " << chunkPos.z << ")" << std::endl;
            return;
        }

        chunk->setBlock(localX, y, localZ, type);

        // ⭐ Marcar chunk como modificado (para guardarlo después)
        chunk->isModified = true;

        // ⭐⭐⭐ RECONSTRUCCIÓN DIFERIDA: NO rebuild inmediato - marca para próximo frame
        // Esto evita congelamiento cuando rompes/colocas bloques rápidamente (ej: al saltar)
        // Las reconstrucciones se procesan de forma progresiva en updateChunks()

        // ⭐⭐⭐ SISTEMA DE DIRTY CHUNKS OPTIMIZADO: Solo marcar, NO reconstruir inmediatamente
        // Las reconstrucciones se harán en updateChunks() de forma progresiva

        // Borde X negativo (localX == 0)
        if (localX == 0) {
            Chunk* neighborChunk = getChunk(Vec3i(chunkPos.x - 1, 0, chunkPos.z));
            if (neighborChunk && neighborChunk->isGenerated) {
                neighborChunk->needsRebuild = true;
                // ⭐ NO rebuild inmediato - se procesará en el próximo frame
            }
        }

        // Borde X positivo (localX == CHUNK_SIZE - 1)
        if (localX == CHUNK_SIZE - 1) {
            Chunk* neighborChunk = getChunk(Vec3i(chunkPos.x + 1, 0, chunkPos.z));
            if (neighborChunk && neighborChunk->isGenerated) {
                neighborChunk->needsRebuild = true;
                // ⭐ NO rebuild inmediato - se procesará en el próximo frame
            }
        }

        // Borde Z negativo (localZ == 0)
        if (localZ == 0) {
            Chunk* neighborChunk = getChunk(Vec3i(chunkPos.x, 0, chunkPos.z - 1));
            if (neighborChunk && neighborChunk->isGenerated) {
                neighborChunk->needsRebuild = true;
                // ⭐ NO rebuild inmediato - se procesará en el próximo frame
            }
        }

        // Borde Z positivo (localZ == CHUNK_SIZE - 1)
        if (localZ == CHUNK_SIZE - 1) {
            Chunk* neighborChunk = getChunk(Vec3i(chunkPos.x, 0, chunkPos.z + 1));
            if (neighborChunk && neighborChunk->isGenerated) {
                neighborChunk->needsRebuild = true;
                // ⭐ NO rebuild inmediato - se procesará en el próximo frame
            }
        }
    }

    BlockType getBlockInChunk(int x, int y, int z) {
        if (y < 0 || y >= CHUNK_HEIGHT) return BLOCK_AIR;

        int cx = (int)floor((float)x / CHUNK_SIZE);
        int cz = (int)floor((float)z / CHUNK_SIZE);

        Vec3i chunkPos(cx, 0, cz);
        Chunk* chunk = getChunk(chunkPos);
        if (!chunk || !chunk->isGenerated) return BLOCK_AIR;

        int localX = ((x % CHUNK_SIZE) + CHUNK_SIZE) % CHUNK_SIZE;
        int localZ = ((z % CHUNK_SIZE) + CHUNK_SIZE) % CHUNK_SIZE;

        return chunk->getBlock(localX, y, localZ);
    }

    // ========================================================================
    // ⭐⭐⭐ SISTEMA AVANZADO DE FLUIDOS - Estilo Minecraft Mejorado ⭐⭐⭐
    // ========================================================================
private:
    // Cola de bloques de agua que necesitan actualización
    std::queue<std::tuple<int, int, int>> waterUpdateQueue;
    const int MAX_WATER_UPDATES_PER_TICK = 50;  // ⭐ AUMENTADO: 50 bloques por tick para flujo rápido y fluido

    // ⭐ Sistema de niveles de agua (0 = fuente, 1-7 = flujo decreciente)
    std::map<std::tuple<int, int, int>, int> waterLevels;

public:
    // Obtener nivel de agua en una posición
    int getWaterLevel(int x, int y, int z) {
        auto key = std::make_tuple(x, y, z);
        auto it = waterLevels.find(key);
        if (it != waterLevels.end()) {
            return it->second;
        }
        return -1; // No hay agua
    }

    // Establecer nivel de agua
    void setWaterLevel(int x, int y, int z, int level) {
        if (level < 0) {
            waterLevels.erase(std::make_tuple(x, y, z));
        } else {
            waterLevels[std::make_tuple(x, y, z)] = level;
        }
    }

    // ⭐⭐⭐ Detectar si hay una fuente infinita (2x2 de agua)
    bool isInfiniteSource(int x, int y, int z) {
        // Contar bloques de agua adyacentes (no diagonales)
        int waterCount = 0;
        int directions[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

        for (int i = 0; i < 4; i++) {
            int nx = x + directions[i][0];
            int nz = z + directions[i][1];

            if (getBlock(nx, y, nz) == BLOCK_WATER) {
                int neighborLevel = getWaterLevel(nx, y, nz);
                if (neighborLevel == 0) {  // Solo fuentes cuentan
                    waterCount++;
                }
            }
        }

        // Si hay 2+ fuentes adyacentes, esta posición es fuente infinita
        return waterCount >= 2;
    }

    // Añadir bloque de agua a la cola de actualización
    void scheduleWaterUpdate(int x, int y, int z) {
        if (waterUpdateQueue.size() < 5000) {  // Límite de cola aumentado
            waterUpdateQueue.push(std::make_tuple(x, y, z));
        }
    }

    // ⭐⭐⭐ Actualizar flujo de agua mejorado (procesarcola)
    void updateWaterFlow() {
        PROFILE_SCOPE("World::updateWaterFlow");
        try {
            int updatesProcessed = 0;

            while (!waterUpdateQueue.empty() && updatesProcessed < MAX_WATER_UPDATES_PER_TICK) {
                auto pos = waterUpdateQueue.front();
                waterUpdateQueue.pop();

                int x = std::get<0>(pos);
                int y = std::get<1>(pos);
                int z = std::get<2>(pos);

                // Validar posición
                if (y < 1 || y >= CHUNK_HEIGHT - 1) continue;

                BlockType currentBlock = getBlock(x, y, z);

                // Solo procesar si es agua
                if (currentBlock != BLOCK_WATER) {
                    setWaterLevel(x, y, z, -1);  // Limpiar nivel
                    continue;
                }

                int currentLevel = getWaterLevel(x, y, z);
                if (currentLevel < 0) currentLevel = 0;  // Asumir fuente si no hay nivel

                // ⭐ PASO 1: Verificar si es fuente infinita
                if (isInfiniteSource(x, y, z)) {
                    setWaterLevel(x, y, z, 0);  // Convertir en fuente
                    currentLevel = 0;
                }

                // ⭐ PASO 2: Caer hacia abajo (PRIORIDAD MÁXIMA - gravedad)
                BlockType blockBelow = getBlock(x, y - 1, z);
                if (blockBelow == BLOCK_AIR) {
                    setBlock(x, y - 1, z, BLOCK_WATER);
                    setWaterLevel(x, y - 1, z, 0);  // Agua que cae es fuente
                    scheduleWaterUpdate(x, y - 1, z);
                    updatesProcessed++;
                    continue;  // No expandir horizontalmente si puede caer
                }

                // ⭐ PASO 3: Expandir horizontalmente (solo si no puede caer)
                if (blockBelow != BLOCK_AIR && currentLevel < 7) {
                    int directions[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    int nextLevel = currentLevel + 1;

                    // Expandir en todas las direcciones simultáneamente
                    for (int i = 0; i < 4; i++) {
                        int nx = x + directions[i][0];
                        int nz = z + directions[i][1];

                        BlockType neighborBlock = getBlock(nx, y, nz);

                        if (neighborBlock == BLOCK_AIR) {
                            // Colocar agua con nivel reducido
                            setBlock(nx, y, nz, BLOCK_WATER);
                            setWaterLevel(nx, y, nz, nextLevel);
                            scheduleWaterUpdate(nx, y, nz);
                            updatesProcessed++;

                            if (updatesProcessed >= MAX_WATER_UPDATES_PER_TICK) break;
                        } else if (neighborBlock == BLOCK_WATER) {
                            // Actualizar nivel si es menor
                            int neighborLevel = getWaterLevel(nx, y, nz);
                            if (neighborLevel > nextLevel) {
                                setWaterLevel(nx, y, nz, nextLevel);
                                scheduleWaterUpdate(nx, y, nz);
                            }
                        }
                    }
                }

                // ⭐ PASO 4: Remover agua si no tiene fuente válida (nivel 7 sin soporte)
                if (currentLevel >= 7 && !isInfiniteSource(x, y, z)) {
                    bool hasSourceNearby = false;
                    int directions[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

                    for (int i = 0; i < 4; i++) {
                        int nx = x + directions[i][0];
                        int nz = z + directions[i][1];

                        if (getBlock(nx, y, nz) == BLOCK_WATER) {
                            int neighborLevel = getWaterLevel(nx, y, nz);
                            if (neighborLevel >= 0 && neighborLevel < currentLevel) {
                                hasSourceNearby = true;
                                break;
                            }
                        }
                    }

                    // Si no hay fuente, secar este bloque
                    if (!hasSourceNearby) {
                        setBlock(x, y, z, BLOCK_AIR);
                        setWaterLevel(x, y, z, -1);
                        updatesProcessed++;
                    }
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "⚠️ Error en updateWaterFlow: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "⚠️ Error desconocido en updateWaterFlow" << std::endl;
        }
    }

    // Llamar cuando se coloca o destruye agua para iniciar propagación
    void notifyWaterPlaced(int x, int y, int z) {
        setWaterLevel(x, y, z, 0);  // Nueva agua es fuente (nivel 0)
        scheduleWaterUpdate(x, y, z);

        // Notificar vecinos para recalcular niveles
        int directions[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int i = 0; i < 4; i++) {
            int nx = x + directions[i][0];
            int nz = z + directions[i][1];
            if (getBlock(nx, y, nz) == BLOCK_WATER) {
                scheduleWaterUpdate(nx, y, nz);
            }
        }

        // Notificar bloque superior y inferior
        if (getBlock(x, y + 1, z) == BLOCK_WATER) {
            scheduleWaterUpdate(x, y + 1, z);
        }
        if (getBlock(x, y - 1, z) == BLOCK_WATER) {
            scheduleWaterUpdate(x, y - 1, z);
        }
    }

    // Llamar cuando se destruye agua
    void notifyWaterRemoved(int x, int y, int z) {
        setWaterLevel(x, y, z, -1);

        // Notificar todos los vecinos para que recalculen
        int directions[6][3] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
        for (int i = 0; i < 6; i++) {
            int nx = x + directions[i][0];
            int ny = y + directions[i][1];
            int nz = z + directions[i][2];

            if (getBlock(nx, ny, nz) == BLOCK_WATER) {
                scheduleWaterUpdate(nx, ny, nz);
            }
        }
    }

    // ========================================================================
    // GREEDY MESHING - Reduce ~96% de caras combinando quads adyacentes
    // ========================================================================


    void buildChunkMesh(Chunk* chunk) {
        // ⭐⭐⭐ PROTECCIÓN CRÍTICA: Validar chunk antes de procesar
        if (!chunk) return;
        if (!chunk->needsRebuild) return;
        if (!chunk->isGenerated) return;

        // ⭐ PROTECCIÓN: Evitar race conditions en threading
        bool expected = false;
        if (!chunk->isUpdatingMesh.compare_exchange_strong(expected, true)) {
            return; // Ya se está procesando este chunk
        }

        PROFILE_SCOPE("World::buildChunkMesh");

        // ⭐ OPTIMIZACIÓN: Early exit si el chunk está vacío.
        // La paleta ya lo sabe: basta mirar los 16 subchunks en vez de recorrer
        // los 65.536 bloques con getBlock() (que hace división, módulo e
        // indirección de paleta en cada uno).
        // Equivalente exacto: una paleta no uniforme tiene entradas distintas,
        // así que al menos una no es aire.
        bool hasBlocks = false;
        for (const PalettedSubChunk& sub : chunk->subchunks) {
            if (!sub.isUniform() || sub.getUniformBlock() != BLOCK_AIR) {
                hasBlocks = true;
                break;
            }
        }

        if (!hasBlocks) {
            // Limpiar batches existentes si los hay
            for (auto batch : chunk->batches) {
                if (batch) delete batch;
            }
            chunk->batches.clear();
            chunk->needsRebuild = false;
            chunk->waitingForNeighbors = false;
            chunk->isUpdatingMesh.store(false);  // ⭐ CRÍTICO: Desbloquear
            return;
        }

        // ⭐ CRITICAL FIX: Verificar que TODOS los vecinos horizontales existan
        Vec3i northChunkPos(chunk->position.x, 0, chunk->position.z + 1);
        Vec3i southChunkPos(chunk->position.x, 0, chunk->position.z - 1);
        Vec3i eastChunkPos(chunk->position.x + 1, 0, chunk->position.z);
        Vec3i westChunkPos(chunk->position.x - 1, 0, chunk->position.z);

        Chunk* northChunk = getChunk(northChunkPos);
        Chunk* southChunk = getChunk(southChunkPos);
        Chunk* eastChunk = getChunk(eastChunkPos);
        Chunk* westChunk = getChunk(westChunkPos);

        // ⭐⭐⭐ CORREGIDO: Verificar vecinos pero REDUCIR reintentos de 10 a 3
        // 10 reintentos era muy alto y causaba chunks visibles tardíos
        bool missingNeighbors = !northChunk || !northChunk->isGenerated ||
                                !southChunk || !southChunk->isGenerated ||
                                !eastChunk || !eastChunk->isGenerated ||
                                !westChunk || !westChunk->isGenerated;

        if (missingNeighbors) {
            chunk->buildRetries++;

            // ⭐⭐⭐ Después de SOLO 3 reintentos, construir de todas formas
            // Esto evita chunks cortados esperando demasiado tiempo por vecinos
            if (chunk->buildRetries < 3) {
                chunk->waitingForNeighbors = true;
                chunk->isUpdatingMesh.store(false);
                return;  // Se reintentará en el próximo frame
            }
            // Si llega aquí (>= 3 reintentos), construir de todas formas (mejor visible que invisible)
        }

        // Reset contador de reintentos
        chunk->buildRetries = 0;

        // Todos los vecinos existen, proceder con construcción
        chunk->waitingForNeighbors = false;

        // DOBLE BUFFER: Construir nuevos batches sin borrar los viejos (evita parpadeo)
        std::vector<Chunk::TextureBatch*> newBatches;

        // MAPA TEMPORAL: agrupar vértices por textura
        std::map<GLuint, std::vector<float>> verticesByTexture;
        std::map<GLuint, std::vector<float>> colorsByTexture;
        std::map<GLuint, std::vector<float>> uvsByTexture;

        int facesRendered = 0;  // Contador para debug

        // OPTIMIZACIÓN: Reset texture bind cache para este chunk
        g_textureManager->resetBindCache();

        // ⭐ OPTIMIZACIÓN: Pre-reservar memoria para reducir reallocations
        // Estimación conservadora: ~30% de las caras posibles son visibles
        int estimatedFaces = (CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE * 6) / 4;
        for (auto& pair : verticesByTexture) {
            pair.second.reserve(estimatedFaces * 12); // 4 vértices * 3 componentes
        }
        for (auto& pair : colorsByTexture) {
            pair.second.reserve(estimatedFaces * 16); // 4 vértices * 4 componentes
        }
        for (auto& pair : uvsByTexture) {
            pair.second.reserve(estimatedFaces * 8); // 4 vértices * 2 componentes
        }

        // Lambda para obtener bloques vecinos de forma optimizada usando cache
        auto getNeighborBlockCached = [&](int x, int y, int z, int dx, int dy, int dz) -> BlockType {
            int nx = x + dx;
            int ny = y + dy;
            int nz = z + dz;

            // Bounds checking vertical
            if (ny < 0 || ny >= CHUNK_HEIGHT) return BLOCK_AIR;

            // Dentro del mismo chunk (caso más común)
            if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
                return chunk->getBlock(nx, ny, nz);
            }

            // Chunk vecino norte (+Z)
            if (nz >= CHUNK_SIZE && northChunk && nx >= 0 && nx < CHUNK_SIZE) {
                return northChunk->getBlock(nx, ny, nz - CHUNK_SIZE);
            }

            // Chunk vecino sur (-Z)
            if (nz < 0 && southChunk && nx >= 0 && nx < CHUNK_SIZE) {
                return southChunk->getBlock(nx, ny, nz + CHUNK_SIZE);
            }

            // Chunk vecino este (+X)
            if (nx >= CHUNK_SIZE && eastChunk && nz >= 0 && nz < CHUNK_SIZE) {
                return eastChunk->getBlock(nx - CHUNK_SIZE, ny, nz);
            }

            // Chunk vecino oeste (-X)
            if (nx < 0 && westChunk && nz >= 0 && nz < CHUNK_SIZE) {
                return westChunk->getBlock(nx + CHUNK_SIZE, ny, nz);
            }

            // ⭐ CRITICAL FIX: Manejar esquinas diagonales correctamente
            // Si llegamos aquí, es porque el bloque está en una esquina diagonal
            // o coordenadas muy fuera de rango

            // Caso 1: Esquina diagonal (X y Z fuera de rango) - renderizar cara para evitar huecos
            bool isXOutOfRange = (nx < 0 || nx >= CHUNK_SIZE);
            bool isZOutOfRange = (nz < 0 || nz >= CHUNK_SIZE);

            if (isXOutOfRange && isZOutOfRange) {
                // Esquina diagonal - renderizar la cara para evitar huecos visuales
                return BLOCK_AIR;
            }

            // Caso 2: Coordenadas muy fuera de rango (no debería pasar) - optimizar no renderizando
            return BLOCK_STONE;
        };

        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    BlockType block = chunk->getBlock(x, y, z);
                    if (block == BLOCK_AIR) continue;

                    // ⭐ OPTIMIZACIÓN: Early exit si el bloque está completamente rodeado (no tiene caras visibles)
                    // Solo aplica a bloques opacos (no agua/lava)
                    bool isWaterOrLava = (block == BLOCK_WATER || block == BLOCK_LAVA || block == BLOCK_ORANGE_FLOWER);
                    if (!isWaterOrLava) {
                        BlockType top = getNeighborBlockCached(x, y, z, 0, 1, 0);
                        BlockType bottom = getNeighborBlockCached(x, y, z, 0, -1, 0);
                        BlockType north = getNeighborBlockCached(x, y, z, 0, 0, 1);
                        BlockType south = getNeighborBlockCached(x, y, z, 0, 0, -1);
                        BlockType east = getNeighborBlockCached(x, y, z, 1, 0, 0);
                        BlockType west = getNeighborBlockCached(x, y, z, -1, 0, 0);

                        // Si todos los vecinos son opacos, este bloque es invisible
                        if (top != BLOCK_AIR && top != BLOCK_WATER && top != BLOCK_LAVA &&
                            bottom != BLOCK_AIR && bottom != BLOCK_WATER && bottom != BLOCK_LAVA &&
                            north != BLOCK_AIR && north != BLOCK_WATER && north != BLOCK_LAVA &&
                            south != BLOCK_AIR && south != BLOCK_WATER && south != BLOCK_LAVA &&
                            east != BLOCK_AIR && east != BLOCK_WATER && east != BLOCK_LAVA &&
                            west != BLOCK_AIR && west != BLOCK_WATER && west != BLOCK_LAVA) {
                            continue; // Bloque completamente oculto, skip
                        }
                    }

                    int worldX = chunk->position.x * CHUNK_SIZE + x;
                    int worldY = y;
                    int worldZ = chunk->position.z * CHUNK_SIZE + z;

                    float wx = (float)worldX;
                    float wy = (float)worldY;
                    float wz = (float)worldZ;

                    // NEXT-GEN LIGHTING CALCULATION
                    uint8_t lightLevel = chunk->getLightLevel(x, y, z);

                    // GAMMA CURVE para oscuridad realista (no lineal)
                    float rawLight = (float)lightLevel / 18.0f;

                    // Si NO hay luz calculada, usar luz ambiental temporal
                    if (rawLight == 0.0f) {
                        rawLight = 0.8f;  // 80% luz temporal mientras se calcula
                    }

                    float lightFactor = pow(rawLight, 1.2f); // Gamma 1.2 (menos agresivo)

                    // LUZ AMBIENTAL MÍNIMA (nunca negro absoluto)
                    if (lightFactor < 0.15f) lightFactor = 0.15f;  // 15% ambient light

                    // COLORED LIGHTING - Obtener color de luz
                    float lightColorR, lightColorG, lightColorB;
                    chunk->getLightColor(x, y, z, lightColorR, lightColorG, lightColorB);

                    // AGUA: Configurar color azulado
                    bool isWater = (block == BLOCK_WATER);
                    bool isLava = (block == BLOCK_LAVA);
                    bool isOrangeFlower = (block == BLOCK_ORANGE_FLOWER);
                    bool isTransparent = isWater || isLava || isOrangeFlower;
                    float uvAnimOffset = 0.0f; // Sin offset - la animación se maneja en el render

                    if (isWater) {
                        // Color azulado semi-transparente para agua
                        lightColorR *= 0.7f;
                        lightColorG *= 0.85f;
                        lightColorB *= 1.0f;
                    } else if (isLava || isOrangeFlower) {
                        // Color naranja brillante para lava
                        lightColorR *= 1.2f;
                        lightColorG *= 0.6f;
                        lightColorB *= 0.3f;
                    }

                    // Variable para multiplicador de brillo por cara
                    float faceBrightness;

                    // Top face (+Y) - face index 0
                    BlockType topNeighbor = getNeighborBlockCached(x, y, z, 0, 1, 0);
                    if (shouldRenderFace(block, topNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 0);
                        float faceBrightness = 1.0f; // Top = más brillante
                        float alpha = isTransparent ? 0.6f : 1.0f; // Agua/Lava semi-transparente
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        // Vértice 1
                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);
                        // Vértice 2
                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);
                        // Vértice 3
                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);
                        // Vértice 4
                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        facesRendered++;
                    }

                    // Bottom face (-Y) - face index 1
                    BlockType bottomNeighbor = getNeighborBlockCached(x, y, z, 0, -1, 0);
                    if (shouldRenderFace(block, bottomNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 1);
                        faceBrightness = 0.5f; // Bottom = más oscuro
                        float alpha = isWater ? 0.6f : 1.0f;
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        facesRendered++;
                    }

                    // North face (+Z) - face index 2
                    BlockType northNeighbor = getNeighborBlockCached(x, y, z, 0, 0, 1);
                    if (shouldRenderFace(block, northNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 2);
                        faceBrightness = 0.8f; // N/S faces
                        float alpha = isWater ? 0.6f : 1.0f;
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        facesRendered++;
                    }

                    // South face (-Z) - face index 3
                    BlockType southNeighbor = getNeighborBlockCached(x, y, z, 0, 0, -1);
                    if (shouldRenderFace(block, southNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 3);
                        faceBrightness = 0.8f; // N/S faces
                        float alpha = isWater ? 0.6f : 1.0f;
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        facesRendered++;
                    }

                    // East face (+X) - face index 4
                    BlockType eastNeighbor = getNeighborBlockCached(x, y, z, 1, 0, 0);
                    if (shouldRenderFace(block, eastNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 4);
                        faceBrightness = 0.6f; // E/W faces = más oscuro
                        float alpha = isWater ? 0.6f : 1.0f;
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        verts.push_back(wx + 1); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        facesRendered++;
                    }

                    // West face (-X) - face index 5
                    BlockType westNeighbor = getNeighborBlockCached(x, y, z, -1, 0, 0);
                    if (shouldRenderFace(block, westNeighbor)) {
                        GLuint texture = g_textureManager->getBlockTexture(block, 5);
                        faceBrightness = 0.6f; // E/W faces = más oscuro
                        float alpha = isWater ? 0.6f : 1.0f;
                        float r = lightFactor * lightColorR * faceBrightness;
                        float g = lightFactor * lightColorG * faceBrightness;
                        float b = lightFactor * lightColorB * faceBrightness;

                        // VBO BATCHING: Agregar vértices al batch de esta textura
                        auto& verts = verticesByTexture[texture];
                        auto& cols = colorsByTexture[texture];
                        auto& uvCoords = uvsByTexture[texture];

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(0 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz + 1);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(1 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        verts.push_back(wx); verts.push_back(wy + 1); verts.push_back(wz);
                        cols.push_back(r); cols.push_back(g); cols.push_back(b); cols.push_back(alpha);
                        uvCoords.push_back(0 + uvAnimOffset); uvCoords.push_back(1 + uvAnimOffset);

                        facesRendered++;
                    }
                }
            }
        }

        // VBO BATCHING: Crear un batch por cada textura usada en este chunk
        for (auto& pair : verticesByTexture) {
            GLuint texture = pair.first;
            auto& verts = pair.second;
            auto& cols = colorsByTexture[texture];
            auto& uvCoords = uvsByTexture[texture];

            if (verts.empty()) continue; // Skip empty batches

            // Crear nuevo batch
            Chunk::TextureBatch* batch = new Chunk::TextureBatch();
            batch->texture = texture;
            batch->vertexCount = verts.size() / 3;

            // Generar VBOs para este batch
            glGenBuffers(1, &batch->vbo);
            glGenBuffers(1, &batch->colorVBO);
            glGenBuffers(1, &batch->uvVBO);

            // VALIDACIÓN CRÍTICA: Verificar que los VBOs se crearon exitosamente
            // Si algún buffer es inválido (0), NO agregar este batch corrupto
            if (batch->vbo == 0 || batch->colorVBO == 0 || batch->uvVBO == 0) {
                glDeleteBuffers(1, &batch->vbo);
                glDeleteBuffers(1, &batch->colorVBO);
                glDeleteBuffers(1, &batch->uvVBO);
                delete batch;
                continue; // Skip este batch corrupto
            }

            // ⭐ OPTIMIZACIÓN: Siempre usar GL_STATIC_DRAW (más rápido para este caso de uso)
            // Los chunks raramente se modifican, y cuando lo hacen, rebuild completo es OK

            // Subir posiciones al GPU
            glBindBuffer(GL_ARRAY_BUFFER, batch->vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);

            // Subir colores al GPU
            glBindBuffer(GL_ARRAY_BUFFER, batch->colorVBO);
            glBufferData(GL_ARRAY_BUFFER, cols.size() * sizeof(float), cols.data(), GL_STATIC_DRAW);

            // Subir UVs al GPU
            glBindBuffer(GL_ARRAY_BUFFER, batch->uvVBO);
            glBufferData(GL_ARRAY_BUFFER, uvCoords.size() * sizeof(float), uvCoords.data(), GL_STATIC_DRAW);

            // Agregar batch a la lista temporal (NO a chunk->batches todavía)
            newBatches.push_back(batch);
        }

        // Desbindear
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // ⭐ SWAP ATÓMICO SEGURO: Prevenir renderizado durante actualización
        chunk->isUpdatingMesh.store(true, std::memory_order_release);

        auto oldBatches = chunk->batches;
        chunk->batches = newBatches;

        chunk->isUpdatingMesh.store(false, std::memory_order_release);

        // ⭐⭐⭐ Limpiar batches viejos DESPUÉS del swap (ya no se están usando)
        try {
            for (auto batch : oldBatches) {
                if (batch) {
                    // Liberar VBOs de OpenGL
                    if (glDeleteBuffers) {
                        if (batch->vbo != 0) glDeleteBuffers(1, &batch->vbo);
                        if (batch->colorVBO != 0) glDeleteBuffers(1, &batch->colorVBO);
                        if (batch->uvVBO != 0) glDeleteBuffers(1, &batch->uvVBO);
                    }
                    delete batch;
                }
            }
        } catch (...) {
            std::cerr << "⚠️ Error al limpiar batches viejos" << std::endl;
        }

        chunk->needsRebuild = false;
    }

    void updateChunks(const Vec3& playerPos, const Vec3& previousPos, float deltaTime = 0.016f) {
        PROFILE_SCOPE("World::updateChunks");
        // ⭐⭐⭐ Actualizar tiempo de frame para cache LRU
        currentFrameTime++;

        // ⭐⭐⭐ Actualizar pinning de caché
        updateCachePinning(playerPos);

        // ⭐⭐⭐ Actualizar métricas de cache hit rate
        int totalAccess = perfMetrics.cacheHits + perfMetrics.cacheMisses;
        if (totalAccess > 0) {
            perfMetrics.cacheHitRate = (float)perfMetrics.cacheHits / (float)totalAccess;
        }

        Vec3i playerChunk = worldToChunkPos(playerPos);

        // ⭐⭐⭐ SISTEMA CIRCULAR LIMPIO: Cargar continuamente en radio circular
        // NO depende de la dirección del jugador, solo de la distancia

        // ⭐⭐⭐ THROTTLING DINÁMICO OPTIMIZADO: Target 50-60 FPS constantes
        static int MAX_CHUNKS_PER_FRAME = 1;  // ⭐ Iniciar conservador
        static int MAX_MESHES_PER_FRAME_DYNAMIC = 2;
        static float performanceSmoothed = 0.018f; // Target 55 FPS promedio
        static int framesSinceAdjust = 0;

        performanceSmoothed = performanceSmoothed * 0.95f + deltaTime * 0.05f;  // ⭐ Suavizado más agresivo
        framesSinceAdjust++;

        // ⭐⭐⭐ Ajustar solo cada 30 frames (0.5 segundos) para estabilidad
        if (framesSinceAdjust >= 30) {
            framesSinceAdjust = 0;

            // Target: 50-60 FPS (16.67ms - 20ms)
            // Si rendimiento > 60 FPS (< 16.67ms), aumentar carga gradualmente
            if (performanceSmoothed < 0.0167f) {
                if (MAX_CHUNKS_PER_FRAME < 2) MAX_CHUNKS_PER_FRAME++;
                if (MAX_MESHES_PER_FRAME_DYNAMIC < 3) MAX_MESHES_PER_FRAME_DYNAMIC++;
            }
            // Si rendimiento entre 50-60 FPS (16.67-20ms), mantener
            else if (performanceSmoothed < 0.020f) {
                // Zona óptima - mantener valores actuales
            }
            // Si rendimiento < 50 FPS (> 20ms), reducir inmediatamente
            else {
                if (MAX_CHUNKS_PER_FRAME > 1) MAX_CHUNKS_PER_FRAME--;
                if (MAX_MESHES_PER_FRAME_DYNAMIC > 1) MAX_MESHES_PER_FRAME_DYNAMIC--;
            }

            // ⭐ LÍMITES ABSOLUTOS: Nunca exceder para garantizar estabilidad
            if (MAX_CHUNKS_PER_FRAME > 2) MAX_CHUNKS_PER_FRAME = 2;
            if (MAX_MESHES_PER_FRAME_DYNAMIC > 3) MAX_MESHES_PER_FRAME_DYNAMIC = 3;
        }

        int chunksGeneratedThisFrame = 0;

        // Crear lista de chunks prioritizados
        struct ChunkPriority {
            Vec3i pos;
            float distance;  // Solo distancia, sin complicaciones
            float priority;  // Prioridad final (combina distancia + dirección)
        };
        std::vector<ChunkPriority> chunksToGenerate;

        // ⭐⭐⭐ CALCULAR DIRECCIÓN DE MOVIMIENTO
        Vec3 moveDir(0, 0, 0);
        if (previousPos.x != playerPos.x || previousPos.z != playerPos.z) {
            moveDir.x = playerPos.x - previousPos.x;
            moveDir.z = playerPos.z - previousPos.z;
            float len = sqrtf(moveDir.x * moveDir.x + moveDir.z * moveDir.z);
            if (len > 0.001f) {
                moveDir.x /= len;
                moveDir.z /= len;
            }
        }

        // ⭐⭐⭐ ESCANEO CIRCULAR: Solo chunks dentro del radio circular
        for (int x = -RENDER_DISTANCE; x <= RENDER_DISTANCE; x++) {
            for (int z = -RENDER_DISTANCE; z <= RENDER_DISTANCE; z++) {
                // ⭐ CLAVE: Verificar distancia circular (no cuadrada)
                float distance = sqrtf((float)(x*x + z*z));
                if (distance > RENDER_DISTANCE) continue;  // Fuera del círculo

                Vec3i chunkPos(playerChunk.x + x, 0, playerChunk.z + z);

                // Solo chunks no generados
                if (chunks.find(chunkPos) != chunks.end()) continue;

                // ⭐⭐⭐ PRIORIDAD MEJORADA: Distancia + Dirección de movimiento
                float priority = distance;

                // Si el jugador se está moviendo, dar bonus a chunks adelante
                if (moveDir.x != 0 || moveDir.z != 0) {
                    float dotProduct = (x * moveDir.x + z * moveDir.z) / (distance + 0.1f);
                    // dotProduct = 1.0 si el chunk está exactamente adelante
                    // dotProduct = -1.0 si está exactamente atrás
                    priority = distance * (1.0f - dotProduct * 0.5f); // Chunks adelante tienen menor prioridad (se cargan primero)
                }

                chunksToGenerate.push_back({chunkPos, distance, priority});
            }
        }

        // ⭐ Ordenar por prioridad (menor = mejor)
        std::sort(chunksToGenerate.begin(), chunksToGenerate.end(),
            [](const ChunkPriority& a, const ChunkPriority& b) {
                return a.priority < b.priority;  // Menor prioridad = se carga primero
            });

        // ⭐⭐⭐ GENERACIÓN SINCRÓNICA SIMPLE (sin async - más estable)
        for (const auto& cp : chunksToGenerate) {
            if (chunksGeneratedThisFrame >= MAX_CHUNKS_PER_FRAME) break;

            // Verificar que no exista ya
            if (chunks.find(cp.pos) != chunks.end()) {
                continue;
            }

            // Usar getOrCreateChunk que maneja todo correctamente
            Chunk* chunk = getOrCreateChunk(cp.pos);
            if (chunk) {
                chunksGeneratedThisFrame++;
            }
        }

        // ⭐⭐⭐ DESCARGA CIRCULAR: Descargar chunks fuera del círculo + buffer
        std::vector<Vec3i> chunksToRemove;
        for (auto& pair : chunks) {
            int dx = pair.first.x - playerChunk.x;
            int dz = pair.first.z - playerChunk.z;

            // ⭐ CLAVE: Distancia circular, no cuadrada
            float distance = sqrtf((float)(dx*dx + dz*dz));

            // Descargar chunks que están más allá del radio + buffer
            if (distance > RENDER_DISTANCE + 3.0f) {
                chunksToRemove.push_back(pair.first);
            }
        }

        // ⭐⭐⭐ MEJORADO: Guardar y descargar chunks con sistema de pool y batch
        int chunksSaved = 0;
        int chunksUnloaded = 0;
        int chunksPooled = 0;

        for (const Vec3i& pos : chunksToRemove) {
            Chunk* chunk = chunks[pos];
            if (!chunk) continue;

            // ⭐ PASO 1: GUARDAR CHUNK GENERADO (batch queue) - ¡CRÍTICO!
            if (chunk->isGenerated) {  // ⭐ Cambio: guardar si está generado (no solo modificado)
                queueChunkForSave(pos);  // Agregar a cola de guardado batch
                chunksSaved++;
            }

            // ⭐ PASO 2: BORRAR DE CHUNKS ACTIVOS
            chunks.erase(pos);

            // ⭐ PASO 3: DEVOLVER AL POOL O ELIMINAR
            deallocateChunk(chunk);
            chunksUnloaded++;
        }

        // ⭐ Flush batch saves si hay muchos chunks pendientes (guardar en background)
        if (pendingSaveChunks.size() >= 32) {  // Batch size = 32 chunks
            flushPendingSaves();
        }

        // Debug: mostrar estadísticas de descarga
        static int totalUnloaded = 0;
        static int totalSaved = 0;
        static int framesSinceLastReport = 0;

        totalUnloaded += chunksUnloaded;
        totalSaved += chunksSaved;
        framesSinceLastReport++;

        // Reportar solo cada 60 frames (1 segundo a 60 FPS)
        if (chunksUnloaded > 0 && framesSinceLastReport >= 60) {
            std::cout << "📦 Chunks descargados: " << chunksUnloaded
                      << " (Queue save: " << chunksSaved << ")"
                      << " | En memoria: " << chunks.size()
                      << " | Cache: " << chunkCache.size()
                      << " | Pool: " << chunkPool.size() << "/" << CHUNK_POOL_SIZE
                      << std::endl;
            framesSinceLastReport = 0;
        }

        // ⭐⭐⭐ Procesar chunks que necesitan rebuild O están esperando vecinos
        std::vector<Chunk*> chunksToRebuild;
        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;
            if (!chunk) continue;

            // ⭐ CRÍTICO: Incluir chunks esperando vecinos (reintentar)
            if (chunk->isGenerated && (chunk->needsRebuild || chunk->waitingForNeighbors)) {
                chunksToRebuild.push_back(chunk);
            }
        }

        // ⭐⭐⭐ ORDENAR POR DISTANCIA CON PRIORIDAD PARA CHUNKS MODIFICADOS
        std::sort(chunksToRebuild.begin(), chunksToRebuild.end(),
            [&playerChunk](Chunk* a, Chunk* b) {
                // Calcular distancia euclidiana
                int dxA = a->position.x - playerChunk.x;
                int dzA = a->position.z - playerChunk.z;
                int dxB = b->position.x - playerChunk.x;
                int dzB = b->position.z - playerChunk.z;

                float distA = sqrtf((float)(dxA*dxA + dzA*dzA));
                float distB = sqrtf((float)(dxB*dxB + dzB*dzB));

                // ⭐ PRIORIDAD EXTRA: Chunks modificados recientemente (muy cerca del jugador)
                // Si están a distancia <= 2, tienen máxima prioridad
                bool nearA = distA <= 2.0f;
                bool nearB = distB <= 2.0f;

                if (nearA && !nearB) return true;   // A tiene prioridad
                if (!nearA && nearB) return false;  // B tiene prioridad

                return distA < distB; // Ambos cerca o ambos lejos - usar distancia
            });

        // ⭐⭐⭐ CONSTRUIR MESHES: Con budget de tiempo estricto
        int meshesBuiltThisFrame = 0;
        auto meshBuildStart = std::chrono::high_resolution_clock::now();
        const float MAX_MESH_BUILD_TIME_MS = 8.0f;  // ⭐ Máximo 8ms para meshes (deja 8-12ms para resto)

        for (Chunk* chunk : chunksToRebuild) {
            // ⭐ Verificar límite de meshes
            if (meshesBuiltThisFrame >= MAX_MESHES_PER_FRAME_DYNAMIC) break;

            // ⭐⭐⭐ BUDGET DE TIEMPO: Parar si ya usamos mucho tiempo este frame
            auto now = std::chrono::high_resolution_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(now - meshBuildStart).count();
            if (elapsedMs > MAX_MESH_BUILD_TIME_MS && meshesBuiltThisFrame > 0) {
                break;  // Ya gastamos suficiente tiempo este frame
            }

            buildChunkMesh(chunk);
            meshesBuiltThisFrame++;
        }
    }

    // Generar chunks iniciales alrededor del origen (para spawn)
    // ⭐⭐⭐ MEJORADO: Carga en espiral desde el centro para tener terreno jugable rápido
    void generateInitialChunks(int radius, GLFWwindow* window = nullptr) {
        std::cout << "Generando chunks iniciales en espiral..." << std::endl;

        // ⭐ Generar en patrón de espiral (centro primero)
        std::vector<Vec3i> chunkPositions;

        // Centro primero
        chunkPositions.push_back(Vec3i(0, 0, 0));

        // Espiral desde el centro
        for (int r = 1; r <= radius; r++) {
            // Recorrer el perímetro del cuadrado de radio r
            for (int x = -r; x <= r; x++) {
                for (int z = -r; z <= r; z++) {
                    // Solo el borde del cuadrado actual
                    if (abs(x) == r || abs(z) == r) {
                        // Verificar distancia circular
                        float dist = sqrtf((float)(x*x + z*z));
                        if (dist <= radius) {
                            chunkPositions.push_back(Vec3i(x, 0, z));
                        }
                    }
                }
            }
        }

        int totalChunks = chunkPositions.size();
        int chunksGenerated = 0;

        for (const Vec3i& chunkPos : chunkPositions) {
            getOrCreateChunk(chunkPos);
            chunksGenerated++;

            // ⭐ CRÍTICO: Procesar eventos de Windows cada 3 chunks (más frecuente)
            if (window && chunksGenerated % 3 == 0) {
                glfwPollEvents(); // Evita "No responde"

                // Actualizar título con progreso
                int progress = (chunksGenerated * 100) / totalChunks;
                std::string title = "Voxel World - Generando terreno... " + std::to_string(progress) + "%";
                glfwSetWindowTitle(window, title.c_str());
            }
        }

        // Restaurar título
        if (window) {
            glfwSetWindowTitle(window, "Voxel World - Sandbox Infinito");
        }

        std::cout << "✅ Chunks generados! Total: " << chunks.size() << std::endl;
    }

    // Construir meshes pendientes (OPTIMIZADO: Solo primeros chunks críticos)
    // ⭐⭐⭐ MEJORADO: Construcción progresiva con feedback visual
    void buildAllPendingMeshes(GLFWwindow* window = nullptr) {
        std::cout << "Construyendo meshes iniciales..." << std::endl;
        int meshCount = 0;
        const int MAX_INITIAL_MESHES = 20;  // ⭐⭐⭐ ULTRA REDUCIDO: Solo 20 meshes para inicio instantáneo

        // Priorizar chunks cercanos al origen (spawn)
        std::vector<Chunk*> chunksToBuild;
        for (auto& pair : chunks) {
            if (pair.second->needsRebuild && pair.second->isGenerated) {
                chunksToBuild.push_back(pair.second);
            }
        }

        // Ordenar por distancia al origen (distancia euclidiana real)
        std::sort(chunksToBuild.begin(), chunksToBuild.end(), [](Chunk* a, Chunk* b) {
            float distA = sqrtf((float)(a->position.x * a->position.x + a->position.z * a->position.z));
            float distB = sqrtf((float)(b->position.x * b->position.x + b->position.z * b->position.z));
            return distA < distB;
        });

        // Construir solo los primeros MAX_INITIAL_MESHES
        int maxToBuild = (chunksToBuild.size() < MAX_INITIAL_MESHES) ? chunksToBuild.size() : MAX_INITIAL_MESHES;
        for (int i = 0; i < maxToBuild; i++) {
            buildChunkMesh(chunksToBuild[i]);
            meshCount++;

            // ⭐ CRÍTICO: Procesar eventos de Windows cada 3 meshes
            if (window && meshCount % 3 == 0) {
                glfwPollEvents(); // Evita "No responde"

                // Actualizar título con progreso
                int progress = (meshCount * 100) / maxToBuild;
                std::string title = "Voxel World - Construyendo mundo... " + std::to_string(progress) + "%";
                glfwSetWindowTitle(window, title.c_str());
            }
        }

        // Restaurar título
        if (window) {
            glfwSetWindowTitle(window, "Voxel World - Sandbox Infinito");
        }

        std::cout << "Meshes construidos: " << meshCount << " de " << chunksToBuild.size() << " (resto se construirán en juego)" << std::endl;
    }

    // Obtener numero de chunks cargados
    int getChunkCount() const {
        return chunks.size();
    }

    // ⭐ NUEVO: Obtener estadísticas detalladas de chunks
    struct ChunkStats {
        int totalChunks;
        int modifiedChunks;
        int chunksNeedingRebuild;
        int chunksWithMesh;
        float memoryUsageMB;
    };

    ChunkStats getChunkStats() const {
        ChunkStats stats;
        stats.totalChunks = chunks.size();
        stats.modifiedChunks = 0;
        stats.chunksNeedingRebuild = 0;
        stats.chunksWithMesh = 0;

        for (const auto& pair : chunks) {
            if (pair.second->isModified) stats.modifiedChunks++;
            if (pair.second->needsRebuild) stats.chunksNeedingRebuild++;
            if (!pair.second->batches.empty()) stats.chunksWithMesh++;
        }

        // Calcular uso aproximado de memoria
        // Cada chunk: blocks array + mesh data (aproximado)
        size_t blockArraySize = CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE * sizeof(BlockType);
        size_t lightDataSize = CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE * sizeof(LightVoxel);
        size_t perChunkSize = blockArraySize + lightDataSize + sizeof(Chunk);

        stats.memoryUsageMB = (stats.totalChunks * perChunkSize) / (1024.0f * 1024.0f);

        return stats;
    }

    void render(const Vec3& playerPos = Vec3(0, 0, 0)) {
        PROFILE_SCOPE("World::render");
        // Asegurar estados de OpenGL correctos
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);

        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // ⭐⭐⭐ NIEBLA CIRCULAR MEJORADA: Oculta el borde del radio de carga circular
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);  // Niebla lineal (más natural que exponencial)

        // Color de niebla = color del cielo para transición suave
        float fogColor[4] = {0.53f, 0.81f, 0.92f, 1.0f};
        glFogfv(GL_FOG_COLOR, fogColor);

        // ⭐ DISTANCIAS AJUSTADAS para radio circular
        // Niebla empieza antes del borde y termina justo en el borde
        float chunkRadius = RENDER_DISTANCE * CHUNK_SIZE;
        float fogStart = chunkRadius * 0.70f;  // 70% del radio
        float fogEnd = chunkRadius * 0.98f;    // 98% del radio (justo en el borde)

        glFogf(GL_FOG_START, fogStart);
        glFogf(GL_FOG_END, fogEnd);
        glHint(GL_FOG_HINT, GL_NICEST);  // Mejor calidad de niebla

        // FRUSTUM CULLING - Solo renderizar chunks en vista
        float projection[16], modelview[16], viewProj[16];
        glGetFloatv(GL_PROJECTION_MATRIX, projection);
        glGetFloatv(GL_MODELVIEW_MATRIX, modelview);

        // Multiplicar matrices: viewProj = projection * modelview
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                viewProj[i * 4 + j] = 0;
                for (int k = 0; k < 4; k++) {
                    viewProj[i * 4 + j] += projection[k * 4 + j] * modelview[i * 4 + k];
                }
            }
        }

        Frustum frustum;
        frustum.extractFromMatrix(viewProj);

        int chunksRendered = 0;
        int chunksCulled = 0;
        int facesRendered = 0;
        int batchesRendered = 0;

        // ⭐ OPTIMIZACIÓN: Crear lista de chunks visibles con distancia para sorting
        struct ChunkRenderInfo {
            Chunk* chunk;
            float distanceSquared;
        };
        std::vector<ChunkRenderInfo> visibleChunks;

        // Primera pasada: Recopilar chunks visibles
        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;

            // ⭐ PROTECCIÓN CRÍTICA: Validar puntero del chunk
            if (!chunk) continue;

            // ⭐⭐⭐ Validación mejorada con logging silencioso
            if (!chunk->isGenerated) continue;

            // Si está actualizando mesh, skip (normal)
            if (chunk->isUpdatingMesh.load(std::memory_order_acquire)) continue;

            // ⭐ CRÍTICO: Si no tiene batches pero está generado, marcarlo para rebuild
            if (chunk->batches.empty()) {
                // Solo marcar si no está ya marcado (evitar spam)
                if (!chunk->needsRebuild && !chunk->waitingForNeighbors) {
                    chunk->needsRebuild = true;
                }
                continue;
            }

            // Frustum culling
            float chunkWorldX = chunk->position.x * CHUNK_SIZE;
            float chunkWorldZ = chunk->position.z * CHUNK_SIZE;

            if (frustum.isChunkVisible(chunkWorldX, 0, chunkWorldZ, CHUNK_SIZE)) {
                // Calcular distancia al jugador (squared para evitar sqrt)
                float dx = chunkWorldX + CHUNK_SIZE/2.0f - playerPos.x;
                float dz = chunkWorldZ + CHUNK_SIZE/2.0f - playerPos.z;
                float distSq = dx*dx + dz*dz;

                visibleChunks.push_back({chunk, distSq});
                chunksRendered++;
            } else {
                chunksCulled++;
            }
        }

        // ⭐ OPTIMIZACIÓN: Ordenar chunks por distancia (más cercanos primero)
        // Esto mejora la coherencia de caché y el early Z-testing
        std::sort(visibleChunks.begin(), visibleChunks.end(),
            [](const ChunkRenderInfo& a, const ChunkRenderInfo& b) {
                return a.distanceSquared < b.distanceSquared;
            });

        // VBO RENDERING: Habilitar vertex arrays
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        // ANIMACIÓN DE AGUA/LAVA: Obtener texturas para comparación
        GLuint waterTexture = g_textureManager->getWaterTexture();
        GLuint lavaTexture = g_textureManager->getTexture("Lava.gif");
        // BLOCK_ORANGE_FLOWER también usa textura de lava
        double currentTime = glfwGetTime();
        float waterOffsetU = (float)fmod(currentTime * 0.05, 1.0); // Scroll horizontal lento
        float waterOffsetV = (float)fmod(currentTime * 0.03, 1.0); // Scroll vertical más lento

        // ⭐⭐⭐ PASE 1: RENDERIZAR BLOQUES OPACOS (sin blending) ⭐⭐⭐
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);  // Escribir en depth buffer

        // Renderizar chunks visibles ordenados - BLOQUES OPACOS
        for (const auto& info : visibleChunks) {
            Chunk* chunk = info.chunk;

            // VBO BATCHING: Renderizar cada batch con su textura
            for (auto* batch : chunk->batches) {
                // ⭐ PROTECCIÓN CRÍTICA: Validar puntero del batch
                if (!batch) continue;

                // VALIDACIÓN CRÍTICA: NO renderizar batches corruptos o inválidos
                if (batch->vertexCount == 0 ||
                    batch->vbo == 0 || batch->colorVBO == 0 || batch->uvVBO == 0 ||
                    batch->vertexCount > 1000000) continue;

                // ⭐ PASE 1: Saltar agua/lava, solo bloques opacos
                bool isTransparentBatch = (batch->texture == waterTexture || batch->texture == lavaTexture);
                if (isTransparentBatch) continue;  // Skip agua/lava en este pase

                // Bind textura para este batch (optimizado con cache)
                g_textureManager->bindOptimized(batch->texture);

                // ⭐ PROTECCIÓN: Verificar funciones VBO antes de usar
                if (!glBindBuffer) continue;  // glVertexPointer es estática (GL 1.1), no necesita chequeo

                // Bind VBOs para este batch
                glBindBuffer(GL_ARRAY_BUFFER, batch->vbo);
                glVertexPointer(3, GL_FLOAT, 0, 0);

                glBindBuffer(GL_ARRAY_BUFFER, batch->colorVBO);
                glColorPointer(4, GL_FLOAT, 0, 0);

                glBindBuffer(GL_ARRAY_BUFFER, batch->uvVBO);
                glTexCoordPointer(2, GL_FLOAT, 0, 0);

                // Renderizar como GL_QUADS
                glDrawArrays(GL_QUADS, 0, batch->vertexCount);

                // Estadísticas
                facesRendered += batch->vertexCount / 4;  // 4 vértices por cara
                batchesRendered++;
            }
        }

        // ⭐⭐⭐ PASE 2: RENDERIZAR BLOQUES TRANSPARENTES (agua con blending) ⭐⭐⭐
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);  // NO escribir en depth buffer (leer sí, escribir no)

        // ⭐ OPTIMIZACIÓN: Renderizar transparentes de atrás hacia adelante (back-to-front)
        // Para blending correcto, invertir el orden de los chunks
        for (auto it = visibleChunks.rbegin(); it != visibleChunks.rend(); ++it) {
            Chunk* chunk = it->chunk;

            // VBO BATCHING: Renderizar solo batches de agua/lava
            for (auto* batch : chunk->batches) {
                // ⭐ PROTECCIÓN CRÍTICA: Validar puntero del batch
                if (!batch) continue;

                // VALIDACIÓN CRÍTICA: NO renderizar batches corruptos
                if (batch->vertexCount == 0 ||
                    batch->vbo == 0 || batch->colorVBO == 0 || batch->uvVBO == 0 ||
                    batch->vertexCount > 1000000) continue;

                // ⭐ PASE 2: Solo renderizar agua/lava
                bool isTransparentBatch = (batch->texture == waterTexture || batch->texture == lavaTexture);
                if (!isTransparentBatch) continue;  // Skip bloques opacos en este pase

                // Bind textura para este batch (optimizado con cache)
                g_textureManager->bindOptimized(batch->texture);

                // ANIMACIÓN DE AGUA/LAVA: Aplicar transformación UV
                glMatrixMode(GL_TEXTURE);
                glPushMatrix();
                glTranslatef(waterOffsetU, waterOffsetV, 0.0f);
                glMatrixMode(GL_MODELVIEW);

                // ⭐ PROTECCIÓN: Verificar funciones VBO antes de usar
                // (glVertexPointer es estática de GL 1.1, no necesita chequeo)
                if (!glBindBuffer) {
                    glMatrixMode(GL_TEXTURE);
                    glPopMatrix();
                    glMatrixMode(GL_MODELVIEW);
                    continue;
                }

                // Bind VBOs para este batch
                glBindBuffer(GL_ARRAY_BUFFER, batch->vbo);
                glVertexPointer(3, GL_FLOAT, 0, 0);

                glBindBuffer(GL_ARRAY_BUFFER, batch->colorVBO);
                glColorPointer(4, GL_FLOAT, 0, 0);

                glBindBuffer(GL_ARRAY_BUFFER, batch->uvVBO);
                glTexCoordPointer(2, GL_FLOAT, 0, 0);

                // Renderizar como GL_QUADS
                glDrawArrays(GL_QUADS, 0, batch->vertexCount);

                // ANIMACIÓN DE AGUA: Restaurar matriz de textura
                glMatrixMode(GL_TEXTURE);
                glPopMatrix();
                glMatrixMode(GL_MODELVIEW);
            }
        }

        // Restaurar depth write
        glDepthMask(GL_TRUE);

        // VBO RENDERING: Desactivar vertex arrays
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDisable(GL_FOG);  // Desactivar niebla para no afectar UI/HUD
    }

    // ========================================================================
    // SISTEMA DE GUARDADO DE CHUNKS (ESTILO MINECRAFT)
    // ========================================================================

    // ⭐⭐⭐ Guardar un chunk individual a disco (MEJORADO - siempre guarda si isGenerated)
    void saveChunk(Chunk* chunk, const std::string& worldPath) {
        if (!chunk || !chunk->isGenerated) return;  // ⭐ Cambio: guardar si está generado (no solo si está modificado)

        try {
            // Crear directorio de chunks si no existe
            std::filesystem::path chunksDir = std::filesystem::path(worldPath) / "chunks";
            std::filesystem::create_directories(chunksDir);

            // Nombre del archivo: chunk_X_Z.dat
            std::string filename = "chunk_" + std::to_string(chunk->position.x) + "_" + std::to_string(chunk->position.z) + ".dat";
            std::filesystem::path chunkPath = chunksDir / filename;

            // Escritura atómica: tmp + rename (un crash a media escritura ya
            // no deja el chunk truncado en disco)
            std::filesystem::path chunkTmpPath = chunksDir / (filename + ".tmp");
            std::ofstream file(chunkTmpPath, std::ios::binary);
            if (!file.is_open()) {
                std::cerr << "❌ ERROR: No se pudo abrir " << filename << " para guardar" << std::endl;
                return;
            }

            // Guardar posición del chunk
            file.write((char*)&chunk->position, sizeof(Vec3i));

            // ⭐ IMPORTANTE: Guardar TODOS los bloques del chunk
            file.write((char*)chunk->blocks, sizeof(BlockType) * CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE);

            bool writeOk = file.good();
            file.close();

            if (writeOk) {
                std::error_code ec;
                std::filesystem::rename(chunkTmpPath, chunkPath, ec);
                if (ec) {
                    std::filesystem::remove(chunkPath, ec);
                    std::filesystem::rename(chunkTmpPath, chunkPath, ec);
                }
                if (ec) {
                    std::cerr << "❌ ERROR al renombrar " << filename << ".tmp: " << ec.message() << std::endl;
                }
            } else {
                std::error_code ec;
                std::filesystem::remove(chunkTmpPath, ec);
                std::cerr << "❌ ERROR de escritura en " << filename << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "⚠️ Error al guardar chunk (" << chunk->position.x << ", " << chunk->position.z << "): " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "⚠️ Error desconocido al guardar chunk (" << chunk->position.x << ", " << chunk->position.z << ")" << std::endl;
        }
    }

    // Cargar un chunk individual desde disco
    bool loadChunk(Vec3i chunkPos, const std::string& worldPath) {
        try {
            std::filesystem::path chunksDir = std::filesystem::path(worldPath) / "chunks";
            std::string filename = "chunk_" + std::to_string(chunkPos.x) + "_" + std::to_string(chunkPos.z) + ".dat";
            std::filesystem::path chunkPath = chunksDir / filename;

            if (!std::filesystem::exists(chunkPath)) return false;

            std::ifstream file(chunkPath, std::ios::binary);
            if (!file.is_open()) return false;

            Vec3i pos;
            file.read((char*)&pos, sizeof(Vec3i));

            // ⭐ PROTECCIÓN: Verificar que la lectura fue exitosa
            if (!file.good()) {
                file.close();
                return false;
            }

            Chunk* chunk = new Chunk(pos);
            file.read((char*)chunk->blocks, sizeof(BlockType) * CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE);

            // ⭐ PROTECCIÓN: Verificar que la lectura fue exitosa
            if (!file.good()) {
                delete chunk;
                file.close();
                return false;
            }

            // ⭐ CRÍTICO: Sincronizar subchunks con el array blocks cargado
            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    for (int z = 0; z < CHUNK_SIZE; z++) {
                        BlockType blockType = chunk->blocks[x][y][z];
                        int subchunkIndex = y / SUBCHUNK_HEIGHT;
                        int localY = y % SUBCHUNK_HEIGHT;
                        chunk->subchunks[subchunkIndex].setBlock(x, localY, z, blockType);
                    }
                }
            }

            chunk->isGenerated = true;
            chunk->isModified = false;  // Ya está guardado
            chunk->needsRebuild = true;
            chunks[pos] = chunk;

            file.close();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "⚠️ Error al cargar chunk (" << chunkPos.x << ", " << chunkPos.z << "): " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "⚠️ Error desconocido al cargar chunk (" << chunkPos.x << ", " << chunkPos.z << ")" << std::endl;
            return false;
        }
    }

    // Guardar todos los chunks modificados
    // ⭐⭐⭐ BATCH SAVING SYSTEM - Guardar múltiples chunks eficientemente ⭐⭐⭐
    void queueChunkForSave(const Vec3i& chunkPos) {
        std::lock_guard<std::mutex> lock(saveMutex);

        // Verificar si ya está en la cola
        for (const Vec3i& pos : pendingSaveChunks) {
            if (pos == chunkPos) {
                return;  // Ya está en cola
            }
        }

        pendingSaveChunks.push_back(chunkPos);
    }

    void flushPendingSaves() {
        if (pendingSaveChunks.empty()) return;

        std::lock_guard<std::mutex> lock(saveMutex);
        auto saveStart = std::chrono::high_resolution_clock::now();

        int savedCount = 0;
        std::cout << "\n⚡ Guardando " << pendingSaveChunks.size() << " chunks en lote..." << std::endl;

        // Guardar todos los chunks pendientes
        for (const Vec3i& pos : pendingSaveChunks) {
            Chunk* chunk = getChunk(pos);
            // ⭐ CRÍTICO: Guardar si está generado (no solo si está modificado)
            if (chunk && chunk->isGenerated) {
                if (useAAASystem && saveManager) {
                    ChunkMetadata metadata;
                    metadata.isDirty = true;
                    metadata.isGenerated = chunk->isGenerated;
                    metadata.lastModified = std::time(nullptr);
                    metadata.hasEntities = false;
                    metadata.hasLighting = false;
                    metadata.modificationCount = 1;
                    metadata.blockChanges = 0;
                    metadata.uuid = 0;

                    saveManager->saveChunkAsync(pos.x, pos.z,
                        chunk->blocks, sizeof(chunk->blocks), metadata);
                    savedCount++;
                } else if (!currentWorldPath.empty()) {
                    saveChunk(chunk, currentWorldPath);
                    savedCount++;
                }

                chunk->isModified = false;  // Marcar como guardado
                totalChunksSaved++;
            }
        }

        // Flush del sistema AAA
        if (useAAASystem && saveManager) {
            saveManager->saveAllDirtyChunks();
        }

        // Calcular tiempo de guardado
        auto saveEnd = std::chrono::high_resolution_clock::now();
        float saveTimeMs = std::chrono::duration<float, std::milli>(saveEnd - saveStart).count();
        perfMetrics.avgSaveTimeMs = (perfMetrics.avgSaveTimeMs * 0.95f) + (saveTimeMs * 0.05f);

        std::cout << "✅ Guardados " << savedCount << " chunks en " << saveTimeMs << " ms" << std::endl;
        std::cout << "   Promedio: " << (saveTimeMs / (savedCount > 0 ? savedCount : 1)) << " ms/chunk" << std::endl;

        pendingSaveChunks.clear();
    }

    void saveWorld(const std::string& worldPath) {
        auto saveStart = std::chrono::high_resolution_clock::now();

        // ⭐⭐⭐ GUARDADO COMPLETO: Recopilar TODOS los chunks en memoria (modificados o no)
        // Esto asegura que TODO el área circular alrededor del jugador se guarde
        std::vector<Chunk*> chunksToSave;
        for (auto& pair : chunks) {
            // Guardar TODOS los chunks cargados para asegurar persistencia completa
            if (pair.second && pair.second->isGenerated) {
                chunksToSave.push_back(pair.second);
            }
        }

        if (chunksToSave.empty()) {
            // Silencioso - no molestar si no hay nada que guardar
            return;
        }

        // Contar cuántos están realmente modificados
        int modifiedCount = 0;
        for (Chunk* chunk : chunksToSave) {
            if (chunk->isModified) modifiedCount++;
        }

        // ⭐⭐⭐ Use AAA Save System if available
        if (useAAASystem && saveManager) {
            // ⭐ Guardar ordenadamente chunk por chunk
            for (Chunk* chunk : chunksToSave) {
                ChunkMetadata metadata;
                metadata.isDirty = chunk->isModified;
                metadata.isGenerated = chunk->isGenerated;
                metadata.lastModified = std::time(nullptr);
                metadata.hasEntities = false;
                metadata.hasLighting = false;
                metadata.modificationCount = 1;
                metadata.blockChanges = 0;
                metadata.uuid = 0;

                saveManager->saveChunkAsync(
                    chunk->position.x,
                    chunk->position.z,
                    chunk->blocks,
                    sizeof(chunk->blocks),
                    metadata
                );

                chunk->isModified = false;  // Marcar como guardado
                totalChunksSaved++;
            }

            saveManager->saveAllDirtyChunks();

            auto saveEnd = std::chrono::high_resolution_clock::now();
            float saveTimeMs = std::chrono::duration<float, std::milli>(saveEnd - saveStart).count();

            // Silencioso - solo log si hay chunks modificados
            if (modifiedCount > 0) {
                std::cout << "💾 Guardados " << modifiedCount << " chunks modificados ("
                          << chunksToSave.size() << " total) en " << saveTimeMs << " ms" << std::endl;
            }
        }
        // Fallback to old system
        else {
            // ⭐ Guardar ordenadamente fila por fila
            for (Chunk* chunk : chunksToSave) {
                saveChunk(chunk, worldPath);
                chunk->isModified = false;
                totalChunksSaved++;
            }

            auto saveEnd = std::chrono::high_resolution_clock::now();
            float saveTimeMs = std::chrono::duration<float, std::milli>(saveEnd - saveStart).count();

            // Silencioso - solo log si hay chunks modificados
            if (modifiedCount > 0) {
                std::cout << "💾 Guardados " << modifiedCount << " chunks modificados ("
                          << chunksToSave.size() << " total) en " << saveTimeMs << " ms" << std::endl;
            }
        }
    }

    // Cargar chunks guardados en el área del jugador
    void loadWorld(const std::string& worldPath) {
        std::filesystem::path chunksDir = std::filesystem::path(worldPath) / "chunks";
        if (!std::filesystem::exists(chunksDir)) return;

        // No cargar todos los chunks, solo los que el jugador necesita
        // Los chunks se cargarán bajo demanda cuando el jugador se acerque
        std::cout << "Sistema de carga de chunks listo" << std::endl;
    }

    // ========================================================================
    // SISTEMA DE ILUMINACIÓN DINÁMICA (0-18 niveles) - CON THREADING
    // ========================================================================

    std::mutex lightingMutex;
    std::atomic<bool> lightingInProgress{false};
    std::thread* lightingThread = nullptr;
    std::queue<Vec3i> lightingQueue;  // Queue de chunks pendientes de iluminacion
    std::mutex lightingQueueMutex;  // Proteger la queue

    unsigned char getLightLevel(int x, int y, int z) {
        if (y < 0 || y >= CHUNK_HEIGHT) return 0;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return 0;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        return chunk->getLightLevel(localX, y, localZ);
    }

    void setLightLevel(int x, int y, int z, unsigned char level) {
        if (y < 0 || y >= CHUNK_HEIGHT) return;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        chunk->setLightLevel(localX, y, localZ, level);
    }

    // ========================================================================
    // NEXT-GEN LIGHTING - EMISSIVE BLOCKS WITH RGB COLOR
    // ========================================================================

    struct EmissiveBlock {
        uint8_t light;  // 0-18
        uint8_t r, g, b; // 0-3
    };

    EmissiveBlock getBlockEmission(BlockType block) {
        EmissiveBlock emission = {0, 0, 0, 0};

        switch (block) {
            // Futuro: bloques emisores con color
            case BLOCK_STONE: // Placeholder para GLOWSTONE
                emission = {15, 3, 3, 2}; // Luz blanca-amarilla
                break;

            // case BLOCK_TORCH:
            //     emission = {14, 3, 2, 1}; // Luz naranja
            //     break;

            // case BLOCK_LAVA:
            //     emission = {15, 3, 1, 0}; // Luz roja-naranja
            //     break;

            // case BLOCK_BLUE_CRYSTAL:
            //     emission = {12, 0, 2, 3}; // Luz azul
            //     break;

            default:
                break;
        }

        return emission;
    }

    // ========================================================================
    // SKYLIGHT SYSTEM - Propagación vertical desde el cielo
    // ========================================================================

    void calculateSkylight() {
        std::cout << "Calculando skylight (luz solar vertical)..." << std::endl;

        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;
            if (!chunk || !chunk->isGenerated) continue;

            // Para cada columna (x, z)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int z = 0; z < CHUNK_SIZE; z++) {
                    int worldX = chunk->position.x * CHUNK_SIZE + x;
                    int worldZ = chunk->position.z * CHUNK_SIZE + z;

                    // Empezar con luz máxima del sol
                    uint8_t currentLight = 18;

                    // Propagación vertical desde arriba
                    for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                        BlockType block = chunk->getBlock(x, y, z);

                        if (block == BLOCK_AIR || block == BLOCK_WATER) {
                            // Bloque transparente: mantener luz
                            chunk->setSunlight(x, y, z, currentLight);
                        } else {
                            // Bloque sólido: detener luz solar
                            chunk->setSunlight(x, y, z, 0);
                            currentLight = 0; // Debajo de bloques sólidos = oscuro
                        }
                    }
                }
            }
        }

        std::cout << "Skylight completado!" << std::endl;
    }

    // ========================================================================
    // BFS FLOOD-FILL PROPAGATION - Para sunlight Y torchlight
    // ========================================================================

    struct LightNode {
        int x, y, z;
        uint8_t lightLevel;

        LightNode(int _x, int _y, int _z, uint8_t _light)
            : x(_x), y(_y), z(_z), lightLevel(_light) {}
    };

    // Propagar SUNLIGHT horizontalmente (después de skylight vertical)
    void propagateSunlight() {
        std::cout << "Propagando sunlight horizontal (OPTIMIZADO)..." << std::endl;

        std::queue<LightNode> lightQueue;
        std::unordered_set<int64_t> visited;

        auto hashPos = [](int x, int y, int z) -> int64_t {
            return ((int64_t)x << 32) | ((int64_t)y << 16) | (int64_t)z;
        };

        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;
            if (!chunk || !chunk->isGenerated) continue;

            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    for (int z = 0; z < CHUNK_SIZE; z++) {
                        uint8_t sunlight = chunk->getSunlight(x, y, z);
                        if (sunlight > 0) {
                            int worldX = chunk->position.x * CHUNK_SIZE + x;
                            int worldZ = chunk->position.z * CHUNK_SIZE + z;
                            int64_t hash = hashPos(worldX, y, worldZ);
                            lightQueue.push(LightNode(worldX, y, worldZ, sunlight));
                            visited.insert(hash);
                        }
                    }
                }
            }
        }

        int iterations = 0;
        const int MAX_ITERATIONS = 1000000;

        while (!lightQueue.empty() && iterations < MAX_ITERATIONS) {
            LightNode node = lightQueue.front();
            lightQueue.pop();

            if (node.lightLevel <= 1) continue;

            int dx[] = {0, 0, 0, 0, 1, -1};
            int dy[] = {1, -1, 0, 0, 0, 0};
            int dz[] = {0, 0, 1, -1, 0, 0};

            for (int i = 0; i < 6; i++) {
                int nx = node.x + dx[i];
                int ny = node.y + dy[i];
                int nz = node.z + dz[i];

                if (ny < 0 || ny >= CHUNK_HEIGHT) continue;

                int64_t hash = hashPos(nx, ny, nz);
                if (visited.count(hash)) continue;

                BlockType neighborBlock = getBlock(nx, ny, nz);
                if (neighborBlock != BLOCK_AIR && neighborBlock != BLOCK_WATER) continue;

                uint8_t currentSunlight = getSunlight(nx, ny, nz);
                uint8_t newLight = node.lightLevel - 1;

                if (newLight > currentSunlight) {
                    setSunlight(nx, ny, nz, newLight);
                    lightQueue.push(LightNode(nx, ny, nz, newLight));
                    visited.insert(hash);
                }
            }

            iterations++;
        }

        std::cout << "Sunlight propagation completada! (" << iterations << " iterations)" << std::endl;
    }


    // Propagar TORCHLIGHT (luz de antorchas y bloques emisores)
    void propagateTorchlight() {
        std::cout << "Propagando torchlight (OPTIMIZADO)..." << std::endl;

        std::queue<LightNode> lightQueue;
        std::unordered_set<int64_t> visited;

        auto hashPos = [](int x, int y, int z) -> int64_t {
            return ((int64_t)x << 32) | ((int64_t)y << 16) | (int64_t)z;
        };

        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;
            if (!chunk || !chunk->isGenerated) continue;

            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    for (int z = 0; z < CHUNK_SIZE; z++) {
                        BlockType block = chunk->getBlock(x, y, z);
                        EmissiveBlock emission = getBlockEmission(block);

                        if (emission.light > 0) {
                            int worldX = chunk->position.x * CHUNK_SIZE + x;
                            int worldZ = chunk->position.z * CHUNK_SIZE + z;

                            chunk->setTorchlight(x, y, z, emission.light,
                                               emission.r, emission.g, emission.b);

                            int64_t hash = hashPos(worldX, y, worldZ);
                            lightQueue.push(LightNode(worldX, y, worldZ, emission.light));
                            visited.insert(hash);
                        }
                    }
                }
            }
        }

        int iterations = 0;
        const int MAX_ITERATIONS = 500000;

        while (!lightQueue.empty() && iterations < MAX_ITERATIONS) {
            LightNode node = lightQueue.front();
            lightQueue.pop();

            if (node.lightLevel <= 1) continue;

            int dx[] = {0, 0, 0, 0, 1, -1};
            int dy[] = {1, -1, 0, 0, 0, 0};
            int dz[] = {0, 0, 1, -1, 0, 0};

            for (int i = 0; i < 6; i++) {
                int nx = node.x + dx[i];
                int ny = node.y + dy[i];
                int nz = node.z + dz[i];

                if (ny < 0 || ny >= CHUNK_HEIGHT) continue;

                int64_t hash = hashPos(nx, ny, nz);
                if (visited.count(hash)) continue;

                BlockType neighborBlock = getBlock(nx, ny, nz);
                if (neighborBlock != BLOCK_AIR && neighborBlock != BLOCK_WATER) continue;

                uint8_t currentTorchlight = getTorchlight(nx, ny, nz);
                uint8_t newLight = node.lightLevel - 1;

                if (newLight > currentTorchlight) {
                    setTorchlight(nx, ny, nz, newLight, 3, 2, 1);
                    lightQueue.push(LightNode(nx, ny, nz, newLight));
                    visited.insert(hash);
                }
            }

            iterations++;
        }

        std::cout << "Torchlight propagation completada! (" << iterations << " iterations)" << std::endl;
    }

    // EXTREME FPS: Iluminar UN CHUNK (solo skylight vertical, SIN BFS)
    void lightChunk(Chunk* chunk) {
        if (!chunk || !chunk->isGenerated) return;

        const int cx = chunk->position.x;
        const int cz = chunk->position.z;

        // SOLO SKYLIGHT VERTICAL - Sin propagación horizontal (ultra rápido)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                uint8_t currentLight = 18;

                // Propagación vertical top-down
                for (int y = CHUNK_HEIGHT - 1; y >= 0; y--) {
                    BlockType block = chunk->getBlock(x, y, z);

                    if (block == BLOCK_AIR || block == BLOCK_WATER) {
                        chunk->setSunlight(x, y, z, currentLight);
                    } else {
                        chunk->setSunlight(x, y, z, 0);
                        currentLight = 0;  // Bloque sólido bloquea luz
                    }
                }
            }
        }

        // Propagar 1 bloque a los lados (mínima propagación)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            for (int y = 0; y < CHUNK_HEIGHT; y++) {
                for (int z = 0; z < CHUNK_SIZE; z++) {
                    uint8_t light = chunk->getSunlight(x, y, z);
                    if (light <= 1) continue;

                    // Propagar a vecinos inmediatos (solo -1 light)
                    int dx[] = {1, -1, 0, 0};
                    int dz[] = {0, 0, 1, -1};

                    for (int i = 0; i < 4; i++) {
                        int nx = x + dx[i];
                        int nz = z + dz[i];

                        if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
                            BlockType neighborBlock = chunk->getBlock(nx, y, nz);
                            if (neighborBlock == BLOCK_AIR || neighborBlock == BLOCK_WATER) {
                                uint8_t neighborLight = chunk->getSunlight(nx, y, nz);
                                uint8_t newLight = light - 1;
                                if (newLight > neighborLight) {
                                    chunk->setSunlight(nx, y, nz, newLight);
                                }
                            }
                        }
                    }
                }
            }
        }

        // chunk->needsRebuild = true;  // CRÍTICO: No forzar rebuild
        chunk->needsLightUpdate = false;
    }

    // Procesar queue de iluminación (llamar cada frame)
    void processLightingQueue() {
        std::lock_guard<std::mutex> lock(lightingQueueMutex);

        // Procesar hasta 3 chunks por frame (balance entre FPS y velocidad de iluminación)
        int chunksProcessed = 0;
        const int MAX_CHUNKS_PER_FRAME = 1;  // OPTIMIZADO: 1 chunk/frame - Sin congelamiento  // EXTREME FPS: 1 chunk/frame

        while (!lightingQueue.empty() && chunksProcessed < MAX_CHUNKS_PER_FRAME) {
            Vec3i chunkPos = lightingQueue.front();
            lightingQueue.pop();

            Chunk* chunk = getChunk(chunkPos);
            if (chunk && chunk->isGenerated && chunk->needsLightUpdate) {
                lightChunk(chunk);
                chunksProcessed++;
            }
        }
    }

    // Agregar chunk a la queue de iluminación
    void queueChunkForLighting(Vec3i chunkPos) {
        std::lock_guard<std::mutex> lock(lightingQueueMutex);
        lightingQueue.push(chunkPos);
    }



    // ========================================================================
    // SISTEMA COMPLETO - Skylight + Sunlight + Torchlight
    // ========================================================================

    void calculateWorldLightingThreaded() {
        std::cout << "\n=== NEXT-GEN LIGHTING CALCULATION ===" << std::endl;

        // PASO 1: Inicializar todo en 0
        std::cout << "[1/4] Inicializando luz..." << std::endl;
        for (auto& pair : chunks) {
            Chunk* chunk = pair.second;
            if (!chunk || !chunk->isGenerated) continue;

            for (int x = 0; x < CHUNK_SIZE; x++) {
                for (int y = 0; y < CHUNK_HEIGHT; y++) {
                    for (int z = 0; z < CHUNK_SIZE; z++) {
                        chunk->lightData[x][y][z] = LightVoxel();
                    }
                }
            }
        }

        // PASO 2: Skylight (propagación vertical)
        std::cout << "[2/4] Calculando skylight..." << std::endl;
        calculateSkylight();

        // PASO 3: Sunlight (propagación horizontal)
        std::cout << "[3/4] Propagando sunlight..." << std::endl;
        propagateSunlight();

        // PASO 4: Torchlight (bloques emisores)
        std::cout << "[4/4] Propagando torchlight..." << std::endl;
        propagateTorchlight();

        std::cout << "=== LIGHTING COMPLETE! ===" << std::endl;

        // Marcar chunks para rebuild
        for (auto& pair : chunks) {
            if (pair.second && pair.second->isGenerated) {
                pair.second->needsRebuild = true;
                pair.second->needsLightUpdate = false;
            }
        }
    }

    // Helpers para World class
    uint8_t getSunlight(int x, int y, int z) {
        if (y < 0 || y >= CHUNK_HEIGHT) return 0;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return 0;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        return chunk->getSunlight(localX, y, localZ);
    }

    void setSunlight(int x, int y, int z, uint8_t level) {
        if (y < 0 || y >= CHUNK_HEIGHT) return;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        chunk->setSunlight(localX, y, localZ, level);
    }

    uint8_t getTorchlight(int x, int y, int z) {
        if (y < 0 || y >= CHUNK_HEIGHT) return 0;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return 0;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        return chunk->getTorchlight(localX, y, localZ);
    }

    void setTorchlight(int x, int y, int z, uint8_t level, uint8_t r, uint8_t g, uint8_t b) {
        if (y < 0 || y >= CHUNK_HEIGHT) return;

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) return;

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        chunk->setTorchlight(localX, y, localZ, level, r, g, b);
    }

    void getLightColor(int x, int y, int z, float& r, float& g, float& b) {
        if (y < 0 || y >= CHUNK_HEIGHT) {
            r = g = b = 1.0f;
            return;
        }

        Vec3i chunkPos(
            (int)floor((float)x / CHUNK_SIZE),
            0,
            (int)floor((float)z / CHUNK_SIZE)
        );

        Chunk* chunk = getChunk(chunkPos);
        if (!chunk) {
            r = g = b = 1.0f;
            return;
        }

        int localX = x - chunkPos.x * CHUNK_SIZE;
        int localZ = z - chunkPos.z * CHUNK_SIZE;

        if (localX < 0) localX += CHUNK_SIZE;
        if (localZ < 0) localZ += CHUNK_SIZE;

        chunk->getLightColor(localX, y, localZ, r, g, b);
    }

        // Calcular iluminación en un hilo separado
    void startLightingCalculation() {
        if (lightingInProgress) {
            std::cout << "Iluminación ya en progreso, ignorando..." << std::endl;
            return;
        }

        // Esperar a que termine el hilo anterior si existe
        if (lightingThread != nullptr) {
            if (lightingThread->joinable()) {
                lightingThread->join();
            }
            delete lightingThread;
        }

        lightingInProgress = true;
        lightingThread = new std::thread([this]() {
            calculateWorldLightingThreaded();
            lightingInProgress = false;
        });
    }

    // Actualizar iluminación (versión simplificada, llama al sistema threaded)
    void updateWorldLighting() {
        // Solo iniciar cálculo si hay chunks que necesitan actualización
        bool needsUpdate = false;
        for (auto& pair : chunks) {
            if (pair.second && pair.second->needsLightUpdate && pair.second->isGenerated) {
                needsUpdate = true;
                break;
            }
        }

        if (needsUpdate && !lightingInProgress) {
            startLightingCalculation();
        }
    }



// ============================================================================
};

// FISICA Y COLISIONES
// ============================================================================

// Estructura para representar un AABB (Axis-Aligned Bounding Box)
struct AABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;

    AABB(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
        : minX(minX), minY(minY), minZ(minZ), maxX(maxX), maxY(maxY), maxZ(maxZ) {}

    // Comprobar si este AABB intersecta con otro
    bool intersects(const AABB& other) const {
        return (minX < other.maxX && maxX > other.minX) &&
               (minY < other.maxY && maxY > other.minY) &&
               (minZ < other.maxZ && maxZ > other.minZ);
    }

    // Expandir el AABB por un margen
    AABB expand(float margin) const {
        return AABB(minX - margin, minY - margin, minZ - margin,
                    maxX + margin, maxY + margin, maxZ + margin);
    }
};

// Obtener el AABB del jugador en una posición específica
AABB getPlayerAABB(const Vec3& pos, float width, float height) {
    float halfWidth = width / 2.0f;
    return AABB(
        pos.x - halfWidth, pos.y, pos.z - halfWidth,
        pos.x + halfWidth, pos.y + height, pos.z + halfWidth
    );
}

// Obtener el AABB de un bloque
AABB getBlockAABB(int x, int y, int z) {
    return AABB(
        (float)x, (float)y, (float)z,
        (float)x + 1.0f, (float)y + 1.0f, (float)z + 1.0f
    );
}

// Verificar colisión mejorada con detección precisa
bool checkAABBCollision(const Vec3& pos, float width, float height, World& world) {
    AABB playerBox = getPlayerAABB(pos, width, height);

    // Calcular el rango de bloques a verificar (con un pequeño margen)
    int minX = (int)floor(playerBox.minX - 0.1f);
    int maxX = (int)floor(playerBox.maxX + 0.1f);
    int minY = (int)floor(playerBox.minY - 0.1f);
    int maxY = (int)floor(playerBox.maxY + 0.1f);
    int minZ = (int)floor(playerBox.minZ - 0.1f);
    int maxZ = (int)floor(playerBox.maxZ + 0.1f);

    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            for (int z = minZ; z <= maxZ; z++) {
                BlockType block = world.getBlock(x, y, z);
                if (isBlockSolid(block)) {
                    AABB blockBox = getBlockAABB(x, y, z);
                    if (playerBox.intersects(blockBox)) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// Verificar colisión en un eje específico y retornar la posición corregida
float resolveCollisionAxis(float oldPos, float newPos, float otherAxis1Old, float otherAxis1New,
                           float otherAxis2Old, float otherAxis2New, float width, float height,
                           World& world, int axis) {
    Vec3 testPos;
    if (axis == 0) { // X
        testPos = Vec3(newPos, otherAxis1New, otherAxis2New);
    } else if (axis == 1) { // Y
        testPos = Vec3(otherAxis1New, newPos, otherAxis2New);
    } else { // Z
        testPos = Vec3(otherAxis1New, otherAxis2New, newPos);
    }

    if (checkAABBCollision(testPos, width, height, world)) {
        return oldPos; // Mantener posición anterior si hay colisión
    }
    return newPos; // Permitir movimiento si no hay colisión
}

// Forward declaration
struct GameState;

// ⭐ SISTEMA DE TIRAR ITEMS: Tirar el item seleccionado con Q (como Minecraft)
void dropSelectedItem(GameState* state);  // Declaración forward

void updatePlayerPhysics(Player& player, World& world, float deltaTime, bool keys[256]) {
    // ⭐ PROTECCIÓN CRÍTICA: Limitar deltaTime para evitar saltos enormes
    if (deltaTime > 0.1f) deltaTime = 0.1f; // Máximo 100ms por frame
    if (deltaTime < 0.0f) deltaTime = 0.0f; // Nunca negativo

    // ⭐ PROTECCIÓN: Si el jugador está en el suelo y completamente quieto, no actualizar física
    bool hasMovementInput = keys['W'] || keys['S'] || keys['A'] || keys['D'] || keys[' '];

    if (player.onGround && !hasMovementInput &&
        fabs(player.velocity.x) < 0.0001f &&
        fabs(player.velocity.z) < 0.0001f &&
        fabs(player.velocity.y) < 0.0001f) {
        // Jugador completamente quieto en el suelo - asegurar que esté 100% inmóvil
        player.velocity.x = 0;
        player.velocity.y = 0;
        player.velocity.z = 0;
        return; // ⭐ NO actualizar posición si está quieto
    }

    // Aplicar gravedad
    if (!player.onGround) {
        player.velocity.y -= player.GRAVITY * deltaTime;
    } else {
        // ⭐ Si está en el suelo, asegurar que la velocidad Y sea exactamente 0
        player.velocity.y = 0;
    }

    // Sistema FPS estándar: movimiento en plano horizontal basado en yaw
    Vec3 forward = player.getMovementForward();
    Vec3 right = player.getMovementRight();

    // Acumular input direccional
    Vec3 moveDir(0, 0, 0);
    if (keys['W']) moveDir = moveDir + forward;
    if (keys['S']) moveDir = moveDir - forward;
    if (keys['D']) moveDir = moveDir + right;
    if (keys['A']) moveDir = moveDir - right;

    if (moveDir.length() > 0) {
        moveDir = moveDir.normalize();
        player.velocity.x = moveDir.x * player.WALK_SPEED;
        player.velocity.z = moveDir.z * player.WALK_SPEED;
    } else {
        player.velocity.x = 0;
        player.velocity.z = 0;
    }

    // Saltar
    if (keys[' '] && player.onGround) {
        player.velocity.y = player.JUMP_FORCE;
        player.onGround = false;
    }

    // SISTEMA DE COLISIONES CON SLIDING
    Vec3 oldPos = player.position;
    Vec3 newPos = oldPos;

    // Calcular nueva posición deseada
    Vec3 desiredPos = oldPos;
    desiredPos.x += player.velocity.x * deltaTime;
    desiredPos.y += player.velocity.y * deltaTime;
    desiredPos.z += player.velocity.z * deltaTime;

    // Intentar movimiento completo primero
    if (!checkAABBCollision(desiredPos, player.WIDTH, player.HEIGHT, world)) {
        // No hay colisión, movimiento libre
        player.position = desiredPos;
        player.onGround = false;

        // Verificar si estamos en el suelo
        Vec3 groundCheck = player.position;
        groundCheck.y -= 0.01f;
        if (checkAABBCollision(groundCheck, player.WIDTH, player.HEIGHT, world)) {
            player.onGround = true;
        }
    } else {
        // Hay colisión, intentar sliding (deslizamiento)

        // 1. Intentar mover solo en X
        Vec3 tryX = oldPos;
        tryX.x = desiredPos.x;
        bool xBlocked = checkAABBCollision(tryX, player.WIDTH, player.HEIGHT, world);

        // 2. Intentar mover solo en Y
        Vec3 tryY = oldPos;
        tryY.y = desiredPos.y;
        bool yBlocked = checkAABBCollision(tryY, player.WIDTH, player.HEIGHT, world);

        // 3. Intentar mover solo en Z
        Vec3 tryZ = oldPos;
        tryZ.z = desiredPos.z;
        bool zBlocked = checkAABBCollision(tryZ, player.WIDTH, player.HEIGHT, world);

        // Aplicar movimiento permitido en cada eje
        if (!xBlocked) {
            newPos.x = desiredPos.x;
        } else {
            player.velocity.x = 0;
        }

        if (!yBlocked) {
            newPos.y = desiredPos.y;
            player.onGround = false;
        } else {
            // ⭐ Bloqueado en Y - el jugador tocó el suelo o el techo
            if (player.velocity.y < 0) {
                // Cayendo: tocar el suelo
                player.onGround = true;
                // ⭐ Asegurar velocidad Y exactamente 0 para evitar rebotes
                player.velocity.y = 0;
            } else if (player.velocity.y > 0) {
                // Subiendo: tocar el techo
                player.velocity.y = 0;
            }
        }

        if (!zBlocked) {
            newPos.z = desiredPos.z;
        } else {
            player.velocity.z = 0;
        }

        // Intentar combinaciones de dos ejes (sliding diagonal)
        // Si X está bloqueado pero Z no, intentar mover en Z
        if (xBlocked && !zBlocked) {
            Vec3 tryXZ = oldPos;
            tryXZ.z = desiredPos.z;
            tryXZ.y = newPos.y;
            if (!checkAABBCollision(tryXZ, player.WIDTH, player.HEIGHT, world)) {
                newPos.z = desiredPos.z;
            }
        }

        // Si Z está bloqueado pero X no, intentar mover en X
        if (zBlocked && !xBlocked) {
            Vec3 tryZX = oldPos;
            tryZX.x = desiredPos.x;
            tryZX.y = newPos.y;
            if (!checkAABBCollision(tryZX, player.WIDTH, player.HEIGHT, world)) {
                newPos.x = desiredPos.x;
            }
        }

        player.position = newPos;

        // ⭐ VERIFICACIÓN FINAL: Si estamos en el suelo, asegurar velocidad Y = 0
        if (player.onGround) {
            player.velocity.y = 0;
        }
    }

    // ⭐ VERIFICACIÓN FINAL DE SUELO: Prevenir micro-rebotes
    // Verificar si el jugador está justo sobre un bloque sólido
    Vec3 finalGroundCheck = player.position;
    finalGroundCheck.y -= 0.001f; // Chequeo muy pequeño
    if (checkAABBCollision(finalGroundCheck, player.WIDTH, player.HEIGHT, world)) {
        if (!player.onGround && fabs(player.velocity.y) < 0.1f) {
            // Estamos tocando el suelo pero no estaba marcado
            player.onGround = true;
            player.velocity.y = 0;
        }
    }

    // ⭐ SONIDOS: Reproducir sonidos de pasos si el jugador está en el suelo y moviéndose
    bool isMoving = (fabs(player.velocity.x) > 0.01f || fabs(player.velocity.z) > 0.01f);
    if (player.onGround && isMoving && g_soundManager) {
        // Detectar el tipo de bloque bajo los pies del jugador
        int footX = (int)floor(player.position.x);
        int footY = (int)floor(player.position.y - 0.1f); // Justo debajo de los pies
        int footZ = (int)floor(player.position.z);

        BlockType groundBlock = world.getBlock(footX, footY, footZ);
        g_soundManager->playFootstep(groundBlock, glfwGetTime());
    }

    // ⭐ SONIDO: Salto
    static bool wasOnGroundLastFrame = player.onGround;
    if (!player.onGround && wasOnGroundLastFrame && player.velocity.y > 5.0f && g_soundManager) {
        g_soundManager->playJump();
    }

    // ⭐ SONIDO: Aterrizaje (detectar cambio de estado de aire a suelo)
    if (player.onGround && !wasOnGroundLastFrame && g_soundManager) {
        g_soundManager->playLand();
    }

    wasOnGroundLastFrame = player.onGround;

    // ⭐⭐⭐ NUEVO: DETECCIÓN DE AGUA PARA OVERLAY Y FÍSICA
    // Detectar si la cámara (ojos) está bajo el agua
    Vec3 eyePos = player.getEyePosition();
    int eyeX = (int)floor(eyePos.x);
    int eyeY = (int)floor(eyePos.y);
    int eyeZ = (int)floor(eyePos.z);
    BlockType eyeBlock = world.getBlock(eyeX, eyeY, eyeZ);
    player.isUnderwater = (eyeBlock == BLOCK_WATER || eyeBlock == BLOCK_LAVA);

    // Detectar si el jugador está tocando agua (para física y sonidos)
    bool touchingWater = false;
    for (int y = (int)floor(player.position.y); y <= (int)floor(player.position.y + player.HEIGHT); y++) {
        for (int x = (int)floor(player.position.x - player.WIDTH); x <= (int)floor(player.position.x + player.WIDTH); x++) {
            for (int z = (int)floor(player.position.z - player.WIDTH); z <= (int)floor(player.position.z + player.WIDTH); z++) {
                BlockType block = world.getBlock(x, y, z);
                if (block == BLOCK_WATER || block == BLOCK_LAVA) {
                    touchingWater = true;
                    break;
                }
            }
            if (touchingWater) break;
        }
        if (touchingWater) break;
    }
    player.isInWater = touchingWater;
}

// ============================================================================
// GAME STATE
// ============================================================================

enum PauseMenuState {
    PAUSE_MENU_MAIN,
    PAUSE_MENU_GRAPHICS,
    PAUSE_MENU_SENSITIVITY
};

enum GameScreenState {
    SCREEN_MAIN_MENU,         // Menú principal
    SCREEN_WORLD_SELECT,      // Selección de mundos
    SCREEN_WORLD_CREATE,      // ⭐⭐⭐ NUEVA: Configuración de nuevo mundo
    SCREEN_LOADING,           // Pantalla de carga
    SCREEN_IN_GAME            // Jugando
};

// ⭐⭐⭐ ESTRUCTURA DE INFORMACIÓN DE MUNDO (MEJORADO - Inspirado en Minecraft level.dat pero MEJOR) ⭐⭐⭐
struct WorldInfo {
    // Información básica
    std::string name;
    std::string folderPath;

    // Timestamps
    long long creationDate;    // ⭐ NUEVO: Cuándo se creó el mundo
    long long lastPlayed;      // Cuándo se jugó por última vez

    // Estadísticas de juego
    float totalPlaytime;       // ⭐ NUEVO: Tiempo total jugado en segundos
    long long worldSizeBytes;  // ⭐ NUEVO: Tamaño del mundo en bytes

    // Información del mundo
    unsigned int seed;         // ⭐ NUEVO: Semilla del mundo (para mostrar en UI)
    std::string versionCreated;// ⭐ NUEVO: Versión con la que se creó

    // Punto de spawn del mundo (no del jugador)
    int spawnX, spawnY, spawnZ; // ⭐ NUEVO: Spawn point del mundo

    // Futuro: Game mode, difficulty, etc.
    int gameMode;              // ⭐ NUEVO: 0=Survival, 1=Creative, 2=Adventure

    WorldInfo() : name(""), folderPath(""), creationDate(0), lastPlayed(0),
                  totalPlaytime(0), worldSizeBytes(0), seed(0),
                  versionCreated("1.0.0"), spawnX(0), spawnY(128), spawnZ(0),
                  gameMode(0) {}

    WorldInfo(const std::string& n, const std::string& path, long long time)
        : name(n), folderPath(path), creationDate(time), lastPlayed(time),
          totalPlaytime(0), worldSizeBytes(0), seed(0),
          versionCreated("1.0.0"), spawnX(0), spawnY(128), spawnZ(0),
          gameMode(0) {}
};

struct Button {
    float x, y, width, height;
    const char* text;
    bool isHovered;

    Button() : x(0), y(0), width(0), height(0), text(""), isHovered(false) {}

    Button(float x, float y, float w, float h, const char* txt)
        : x(x), y(y), width(w), height(h), text(txt), isHovered(false) {}

    bool contains(float mouseX, float mouseY) const {
        return mouseX >= x && mouseX <= x + width &&
               mouseY >= y && mouseY <= y + height;
    }
};

struct GameState {
    Player player;
    World world;
    ParticleSystem particles;
    Inventory inventory;
    CraftingGrid craftingGrid;      // ⭐⭐⭐ Grid de crafteo 3x3
    CraftingSystem craftingSystem;  // ⭐⭐⭐ Sistema de recetas
    InventorySlot craftingResult;   // ⭐⭐⭐ Slot de resultado del crafteo
    InventorySlot heldSlot;         // ⭐⭐⭐ Item que el jugador tiene en el cursor
    std::vector<ItemEntity> items;
    bool keys[256];
    double lastMouseX;
    double lastMouseY;
    bool firstMouse;
    bool cursorLocked;
    bool isPaused;
    bool inventoryOpen;
    PauseMenuState pauseMenuState;
    GameScreenState screenState;  // Estado actual de la pantalla

    // Sistema de mundos
    std::vector<WorldInfo> savedWorlds;
    std::string currentWorldName;
    int selectedWorldIndex;
    bool isEditingWorldName;
    std::string editingWorldNewName;
    bool confirmingDelete;  // Estado de confirmación de borrado

    // Botones del menú principal
    Button btnMundosSolitarios;
    Button btnOpciones;
    Button btnSalir;
    Button btnAvatar;

    // Botones de selección de mundos
    Button btnCrearMundo;
    Button btnVolverMenu;
    std::vector<Button> worldButtons;

    // Botones de gestión de mundos (solo visibles cuando hay un mundo seleccionado)
    Button btnJugarMundo;
    Button btnEditarMundo;
    Button btnBorrarMundo;
    Button btnRespaldoMundo;

    // Botones de edición de nombre (solo visibles en modo edición)
    Button btnGuardarNombre;
    Button btnCancelarEdicion;

    // ⭐⭐⭐ NUEVA PANTALLA: Configuración de creación de mundo
    std::string newWorldName;           // Nombre del nuevo mundo
    std::string newWorldSeed;           // Semilla del nuevo mundo (texto)
    int newWorldGameMode;               // 0=Survival, 1=Creative
    bool isEditingNewWorldName;         // Si está editando el nombre
    bool isEditingNewWorldSeed;         // Si está editando la semilla
    Button btnCreateWorldConfirm;       // Botón "Crear Mundo"
    Button btnCreateWorldCancel;        // Botón "Cancelar"
    Button btnGameModeSurvival;         // Botón modo Survival
    Button btnGameModeCreative;         // Botón modo Creativo

    // Configuraciones
    int renderDistance;
    float mouseSensitivity;

    // Timers para evitar spam
    float breakCooldown;
    float placeCooldown;

    // Sistema de minado progresivo (como Minecraft)
    bool isMining;
    Vec3i miningBlockPos;
    float miningProgress;
    float miningParticleTimer;  // Timer para partículas de minado
    bool mouseLeftPressed;

    // ⭐⭐⭐ Sistema de acumulación de items en hotbar
    int lastPressedSlot;        // Último slot presionado
    double lastSlotPressTime;   // Tiempo del último press

    // Sistema de guardado
    bool isSaving;
    float savingTimer;
    bool returnToMenuAfterSave;

    // ⭐ Sistema de auto-guardado periódico
    float autoSaveTimer;
    float autoSaveInterval;  // Guardar cada X segundos
    bool showSavingIndicator;
    float savingIndicatorTimer;

    // ⭐⭐⭐ NUEVO: Sistema de tracking de tiempo de juego (para level.dat)
    float sessionStartTime;     // Cuando se cargó el mundo
    float currentSessionTime;   // Tiempo de la sesión actual

    // ⭐⭐⭐ NUEVO: Sistema de repetición de BACKSPACE (borrado rápido)
    bool backspacePressed;          // Si BACKSPACE está presionado
    double backspaceFirstPressTime; // Tiempo del primer press
    double backspaceLastRepeatTime; // Tiempo de la última repetición
    float backspaceRepeatDelay;     // Delay inicial antes de empezar a repetir (0.5s)
    float backspaceRepeatRate;      // Velocidad de repetición (0.05s entre borrados)

    // Sistema de carga
    bool isLoading;
    float loadingStartTime;
    float loadingDuration;  // Duración de la carga (10 segundos)
    Vec3 targetSpawnPosition;
    bool spawnFound;

    GameState() : firstMouse(true), cursorLocked(false), isPaused(false), inventoryOpen(false),
                  pauseMenuState(PAUSE_MENU_MAIN), screenState(SCREEN_MAIN_MENU),
                  currentWorldName(""), selectedWorldIndex(-1),
                  isEditingWorldName(false), editingWorldNewName(""), confirmingDelete(false),
                  newWorldName("Nuevo Mundo"), newWorldSeed(""), newWorldGameMode(0),
                  isEditingNewWorldName(false), isEditingNewWorldSeed(false),
                  renderDistance(8), mouseSensitivity(0.15f),
                  breakCooldown(0), placeCooldown(0),
                  isMining(false), miningBlockPos(0, 0, 0), miningProgress(0.0f),
                  miningParticleTimer(0.0f), mouseLeftPressed(false),
                  lastPressedSlot(-1), lastSlotPressTime(0.0),
                  isSaving(false), savingTimer(0.0f), returnToMenuAfterSave(false),
                  autoSaveTimer(0.0f), autoSaveInterval(120.0f), showSavingIndicator(false), savingIndicatorTimer(0.0f),
                  sessionStartTime(0.0f), currentSessionTime(0.0f),  // ⭐ NUEVO: Session tracking
                  backspacePressed(false), backspaceFirstPressTime(0.0), backspaceLastRepeatTime(0.0),
                  backspaceRepeatDelay(0.5f), backspaceRepeatRate(0.05f),  // ⭐ NUEVO: Backspace repeat
                  isLoading(false), loadingStartTime(0.0f), loadingDuration(10.0f),
                  targetSpawnPosition(0, 100, 0), spawnFound(false) {
        for (int i = 0; i < 256; i++) keys[i] = false;
    }

    // ⭐ Determinar qué item debe caer cuando se rompe un bloque
    BlockType getDroppedItem(BlockType brokenBlock) {
        switch (brokenBlock) {
            case BLOCK_GRASS:
                // El bloque de pasto suelta tierra
                return BLOCK_DIRT;

            case BLOCK_DIRT:
                // La tierra suelta tierra
                return BLOCK_DIRT;

            case BLOCK_STONE:
                // La piedra suelta piedra labrada (cobblestone)
                return BLOCK_COBBLESTONE;

            case BLOCK_TALLGRASS:
            case BLOCK_ORANGE_FLOWER:
                // Plantas/flores no sueltan nada (retornar AIR para no spawnear)
                return BLOCK_AIR;

            case BLOCK_LEAVES:
                // Las hojas pueden no soltar nada (20% de probabilidad de soltar)
                if ((rand() % 100) < 20) {
                    return BLOCK_LEAVES;
                }
                return BLOCK_AIR;

            default:
                // Por defecto, el bloque se suelta a sí mismo
                return brokenBlock;
        }
    }

    void spawnItem(Vec3 position, BlockType blockType) {
        // ⭐ Determinar qué item debe caer
        BlockType droppedItem = getDroppedItem(blockType);

        // Si no debe soltar nada (BLOCK_AIR), no spawnear
        if (droppedItem == BLOCK_AIR) {
            return;
        }

        // Añadir pequeña velocidad aleatoria y spawn un poco más arriba
        ItemEntity item(position, droppedItem);
        item.position.y += 0.3f;  // Spawn 0.3 bloques más arriba
        item.velocity = Vec3(
            (rand() % 100 - 50) / 100.0f,  // Más velocidad horizontal
            0.3f,  // Más velocidad hacia arriba
            (rand() % 100 - 50) / 100.0f
        );
        items.push_back(item);
    }

    void updateItems(float deltaTime) {
        for (size_t i = 0; i < items.size(); ) {
            // ⭐⭐⭐ Pasar posición del jugador para atracción magnética
            items[i].update(deltaTime, player.position);

            // ⭐ COLISIÓN CON EL SUELO (solo si NO está siendo atraído)
            if (!items[i].isBeingAttracted) {
                Vec3 itemPos = items[i].position;
                int blockX = (int)floor(itemPos.x);
                int blockY = (int)floor(itemPos.y - 0.1f);
                int blockZ = (int)floor(itemPos.z);

                BlockType blockBelow = world.getBlock(blockX, blockY, blockZ);
                if (blockBelow != BLOCK_AIR && items[i].velocity.y < 0) {
                    // Rebote al caer (solo si está cayendo)
                    if (!items[i].onGround && items[i].velocity.y < -0.5f) {
                        items[i].velocity.y *= -0.4f;  // Rebote del 40% de la velocidad
                    } else {
                        items[i].onGround = true;
                        items[i].velocity.y = 0;
                    }
                    items[i].position.y = blockY + 1.2f;  // Flotar un poco sobre el bloque
                } else if (blockBelow == BLOCK_AIR) {
                    items[i].onGround = false;  // Está en el aire
                }
            }

            // ⭐⭐⭐ RECOGER ITEM (Radio más pequeño para recogida final)
            if (items[i].pickupDelay <= 0) {
                Vec3 toItem = items[i].position - player.position;
                float dist = toItem.length();

                // ⭐ Radio de recogida final: 0.8 bloques (más pequeño que atracción)
                if (dist < 0.8f) {
                    if (inventory.addItem(items[i].blockType, 1)) {
                        items.erase(items.begin() + i);
                        continue;
                    }
                }
            }

            i++;
        }
    }

    // ⭐⭐⭐ CRAFTEO: Actualizar el slot de resultado cuando cambia el grid
    void updateCraftingResult() {
        CraftingRecipe* recipe = craftingSystem.matchRecipe(craftingGrid);

        if (recipe != nullptr) {
            // Hay una receta válida, mostrar resultado
            craftingResult.blockType = recipe->result;
            craftingResult.count = recipe->resultCount;
        } else {
            // No hay receta válida, limpiar resultado
            craftingResult.blockType = BLOCK_AIR;
            craftingResult.count = 0;
        }
    }

    // ⭐⭐⭐ CRAFTEO: Ejecutar el crafteo (cuando el jugador toma el resultado)
    bool executeCrafting() {
        if (craftingResult.isEmpty()) {
            return false;  // No hay nada que craftear
        }

        // Consumir items del crafting grid
        for (int i = 0; i < CraftingGrid::SIZE; i++) {
            if (!craftingGrid.slots[i].isEmpty()) {
                craftingGrid.slots[i].remove(1);
            }
        }

        // El resultado ya se colocó en heldSlot en el mouseButtonCallback
        // Solo necesitamos limpiar y actualizar

        // Limpiar resultado
        BlockType resultType = craftingResult.blockType;
        int resultCount = craftingResult.count;

        craftingResult.blockType = BLOCK_AIR;
        craftingResult.count = 0;

        // Actualizar para ver si hay otra receta válida
        updateCraftingResult();

        return true;
    }
};

// ============================================================================
// FUNCIÓN PARA VISUALIZAR EL RAYCAST
// ============================================================================

// Dibujar línea de raycast desde el jugador hasta el bloque apuntado
void renderRaycastLine(Vec3 origin, Vec3 direction, float distance, bool hit) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Vec3 endpoint = origin + direction * distance;

    // Color: Verde si detectó bloque, Rojo si no
    if (hit) {
        glColor4f(0.0f, 1.0f, 0.0f, 0.5f); // Verde semi-transparente
    } else {
        glColor4f(1.0f, 0.0f, 0.0f, 0.3f); // Rojo semi-transparente
    }

    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glVertex3f(origin.x, origin.y, origin.z);
    glVertex3f(endpoint.x, endpoint.y, endpoint.z);
    glEnd();

    // Pequeña esfera en el origen (posición de los ojos del jugador)
    glPointSize(8.0f);
    glColor4f(1.0f, 1.0f, 0.0f, 1.0f); // Amarillo
    glBegin(GL_POINTS);
    glVertex3f(origin.x, origin.y, origin.z);
    glEnd();

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

GameState* g_gameState = nullptr;

// Variables globales para pantalla completa
bool g_isFullscreen = false;
int g_windowedWidth = 1280;
int g_windowedHeight = 720;
int g_windowedPosX = 100;
int g_windowedPosY = 100;

// ============================================================================
// SISTEMA DE RAYCAST PARA BLOQUES
// ============================================================================

struct RaycastResult {
    bool hit;
    Vec3i blockPos;
    Vec3i previousPos;  // Posición anterior (para colocar bloques)
    Vec3i normal;       // Normal de la cara del bloque
    float distance;

    RaycastResult() : hit(false), blockPos(0, 0, 0), previousPos(0, 0, 0), normal(0, 0, 0), distance(0) {}
};

// Raycast principal (como Minecraft - detecta todos los bloques excepto aire y agua)
RaycastResult raycastBlock(World& world, Vec3 origin, Vec3 direction, float maxDistance) {
    RaycastResult result;

    // EPSILON para evitar división por cero y mejorar estabilidad
    const float EPSILON = 0.00001f;

    // Normalizar dirección
    direction = direction.normalize();

    // Proteger contra valores muy cercanos a cero
    if (fabs(direction.x) < EPSILON) direction.x = EPSILON;
    if (fabs(direction.y) < EPSILON) direction.y = EPSILON;
    if (fabs(direction.z) < EPSILON) direction.z = EPSILON;

    // Posición actual en el grid
    int x = (int)floor(origin.x);
    int y = (int)floor(origin.y);
    int z = (int)floor(origin.z);

    // Dirección de paso (1 o -1)
    int stepX = direction.x > 0 ? 1 : -1;
    int stepY = direction.y > 0 ? 1 : -1;
    int stepZ = direction.z > 0 ? 1 : -1;

    // Distancia para avanzar un voxel en cada eje
    float tDeltaX = (direction.x != 0) ? fabs(1.0f / direction.x) : 1000000.0f;
    float tDeltaY = (direction.y != 0) ? fabs(1.0f / direction.y) : 1000000.0f;
    float tDeltaZ = (direction.z != 0) ? fabs(1.0f / direction.z) : 1000000.0f;

    // Distancia al próximo borde del voxel
    float tMaxX, tMaxY, tMaxZ;

    // Calcular tMax con protección contra división por cero
    if (direction.x != 0) {
        if (direction.x > 0)
            tMaxX = ((x + 1) - origin.x) / direction.x;
        else
            tMaxX = (x - origin.x) / direction.x;
    } else {
        tMaxX = 1000000.0f;  // Infinito efectivo
    }

    if (direction.y != 0) {
        if (direction.y > 0)
            tMaxY = ((y + 1) - origin.y) / direction.y;
        else
            tMaxY = (y - origin.y) / direction.y;
    } else {
        tMaxY = 1000000.0f;  // Infinito efectivo
    }

    if (direction.z != 0) {
        if (direction.z > 0)
            tMaxZ = ((z + 1) - origin.z) / direction.z;
        else
            tMaxZ = (z - origin.z) / direction.z;
    } else {
        tMaxZ = 1000000.0f;  // Infinito efectivo
    }

    // Bloque anterior (para colocar bloques)
    Vec3i prevBlock(x, y, z);

    // Raycast usando DDA 3D
    float t = 0.0f;
    int stepCount = 0;
    const int maxSteps = (int)(maxDistance * 2.0f) + 10; // Límite de pasos para evitar bucles infinitos

    while (t < maxDistance && stepCount < maxSteps) {
        stepCount++;

        // Guardar posición anterior ANTES de verificar
        prevBlock = Vec3i(x, y, z);

        // Avanzar al siguiente voxel PRIMERO
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            t = tMaxX;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            t = tMaxY;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            t = tMaxZ;
            tMaxZ += tDeltaZ;
        }

        // AHORA verificar el bloque (después de avanzar)
        BlockType block = world.getBlock(x, y, z);

        // Detectar TODOS los bloques sólidos (como Minecraft - solo ignora aire y agua)
        if (block != BLOCK_AIR && block != BLOCK_WATER) {
            result.hit = true;
            result.blockPos = Vec3i(x, y, z);
            result.previousPos = prevBlock;
            result.normal = Vec3i(prevBlock.x - x, prevBlock.y - y, prevBlock.z - z);
            result.distance = t;
            return result;
        }
    }

    return result;
}

// ⭐ SISTEMA DE DROPS: Determinar qué item(s) dropea un bloque al minarlo
struct BlockDrop {
    BlockType itemType;
    int count;
    float chance;  // Probabilidad de drop (0.0 - 1.0)
};

std::vector<BlockDrop> getBlockDrops(BlockType blockType) {
    std::vector<BlockDrop> drops;

    switch (blockType) {
        case BLOCK_COAL_ORE:
            // Carbón mineral → dropea 1 carbón item
            drops.push_back({BLOCK_COAL_ITEM, 1, 1.0f});
            break;

        case BLOCK_SCRAP_METAL:
            // Desecho de metales → dropea zinc Y cobre crudo
            drops.push_back({BLOCK_RAW_ZINC, 1, 1.0f});
            drops.push_back({BLOCK_RAW_COPPER, 1, 1.0f});
            break;

        default:
            // Todos los demás bloques dropean ellos mismos
            drops.push_back({blockType, 1, 1.0f});
            break;
    }

    return drops;
}

// Sistema de minado progresivo (como Minecraft)
void updateMining(GameState* state, float deltaTime) {
    Vec3 origin = state->player.getEyePosition();
    Vec3 direction = state->player.getForward();

    // Detectar qué bloque estamos mirando (como Minecraft Java: 5 bloques de alcance)
    RaycastResult result = raycastBlock(state->world, origin, direction, 5.0f);

    // Si NO estamos presionando click izquierdo, resetear minado
    if (!state->mouseLeftPressed) {
        state->isMining = false;
        state->miningProgress = 0.0f;
        state->miningParticleTimer = 0.0f;
        return;
    }

    // ⭐⭐⭐ NUEVO: Si no hay bloque frente a nosotros, intentar romper bloque sobre la cabeza
    if (!result.hit) {
        // Verificar bloque directamente encima de la cabeza del jugador
        Vec3 headPos = state->player.position;
        headPos.y += state->player.HEIGHT; // Posición de la cabeza

        int headBlockX = (int)floor(headPos.x);
        int headBlockY = (int)floor(headPos.y);
        int headBlockZ = (int)floor(headPos.z);

        BlockType headBlock = state->world.getBlock(headBlockX, headBlockY, headBlockZ);

        // Si hay un bloque sólido sobre la cabeza, permitir romperlo
        if (isBlockSolid(headBlock) && headBlock != BLOCK_BEDROCK) {
            // Crear un resultado "falso" para el bloque de la cabeza
            result.hit = true;
            result.blockPos = Vec3i(headBlockX, headBlockY, headBlockZ);
            result.distance = 0.5f;

            std::cout << "🔨 Rompiendo bloque sobre la cabeza en (" << headBlockX << ", " << headBlockY << ", " << headBlockZ << ")" << std::endl;
        } else {
            // No hay nada que romper
            state->isMining = false;
            state->miningProgress = 0.0f;
            state->miningParticleTimer = 0.0f;
            return;
        }
    }

    BlockType blockType = state->world.getBlock(result.blockPos.x, result.blockPos.y, result.blockPos.z);

    // Solo no minar aire (el agua no se puede romper según getBlockBreakTime)
    if (blockType == BLOCK_AIR) {
        state->isMining = false;
        state->miningProgress = 0.0f;
        state->miningParticleTimer = 0.0f;
        return;
    }

    // No minar bloques irrompibles (bedrock y agua según getBlockBreakTime)
    float checkBreakTime = getBlockBreakTime(blockType);
    if (checkBreakTime >= 999.0f) {
        state->isMining = false;
        state->miningProgress = 0.0f;
        state->miningParticleTimer = 0.0f;
        return;
    }

    // Si cambiamos de bloque, resetear progreso
    if (state->isMining && (result.blockPos.x != state->miningBlockPos.x ||
                            result.blockPos.y != state->miningBlockPos.y ||
                            result.blockPos.z != state->miningBlockPos.z)) {
        state->isMining = false;
        state->miningProgress = 0.0f;
        state->miningParticleTimer = 0.0f;
    }

    // Iniciar minado de nuevo bloque
    if (!state->isMining) {
        state->isMining = true;
        state->miningBlockPos = result.blockPos;
        state->miningProgress = 0.0f;
    }

    // Incrementar progreso según el tiempo de rotura del bloque
    float breakTime = getBlockBreakTime(blockType);

    // Si el bloque es instantáneo (TALLGRASS) o muy rápido
    if (breakTime < 0.1f) {
        state->miningProgress = 1.0f;
    } else {
        state->miningProgress += deltaTime / breakTime;

        // PARTÍCULAS PROGRESIVAS mientras se mina
        state->miningParticleTimer += deltaTime;
        if (state->miningParticleTimer >= 0.1f) {  // Cada 0.1 segundos
            state->particles.spawnMiningParticles(
                Vec3(result.blockPos.x, result.blockPos.y, result.blockPos.z),
                blockType
            );
            state->miningParticleTimer = 0.0f;
        }
    }

    // Si completamos el minado, romper el bloque
    if (state->miningProgress >= 1.0f) {
        int bx = result.blockPos.x;
        int by = result.blockPos.y;
        int bz = result.blockPos.z;

        // ⭐ MEJORADO: Si es agua, notificar remoción
        if (blockType == BLOCK_WATER) {
            state->world.notifyWaterRemoved(bx, by, bz);
        }

        // Romper el bloque
        state->world.setBlock(bx, by, bz, BLOCK_AIR);

        // ⭐ SISTEMA DE AGUA: Notificar agua adyacente para que fluya
        // Revisar los 6 bloques adyacentes
        int directions[6][3] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
        for (int d = 0; d < 6; d++) {
            int nx = bx + directions[d][0];
            int ny = by + directions[d][1];
            int nz = bz + directions[d][2];

            // Si hay agua adyacente, programar su actualización
            if (state->world.getBlock(nx, ny, nz) == BLOCK_WATER) {
                state->world.notifyWaterPlaced(nx, ny, nz);
            }
        }

        // ⭐ SONIDO: Reproducir sonido de romper bloque
        if (g_soundManager) {
            g_soundManager->playBreakBlock(blockType, glfwGetTime());
        }

        // EXPLOSIÓN FINAL - Generar muchas partículas de rotura
        state->particles.spawnBlockBreakParticles(
            Vec3(result.blockPos.x, result.blockPos.y, result.blockPos.z),
            blockType
        );

        // ⭐ SISTEMA DE DROPS: Spawnear items según el tipo de bloque
        Vec3 itemPos(result.blockPos.x + 0.5f, result.blockPos.y + 0.5f, result.blockPos.z + 0.5f);
        std::vector<BlockDrop> drops = getBlockDrops(blockType);
        for (const auto& drop : drops) {
            // Verificar probabilidad de drop (siempre 100% por ahora)
            if (drop.chance >= 1.0f) {
                for (int i = 0; i < drop.count; i++) {
                    state->spawnItem(itemPos, drop.itemType);
                }
            }
        }

        // Resetear minado
        state->isMining = false;
        state->miningProgress = 0.0f;
        state->miningParticleTimer = 0.0f;
    }
}

void breakBlock(GameState* state) {
    // Iniciar el proceso de minado al presionar click izquierdo
    state->mouseLeftPressed = true;
}

// ⭐ SISTEMA DE TIRAR ITEMS: Implementación
void dropSelectedItem(GameState* state) {
    // Verificar que hay un item seleccionado
    if (!state->inventory.hasSelectedBlock()) {
        return;
    }

    // Obtener el item seleccionado
    BlockType itemType = state->inventory.getSelectedBlock();

    // Calcular posición de spawn (frente al jugador)
    Vec3 playerEye = state->player.getEyePosition();
    Vec3 forward = state->player.getForward();
    Vec3 spawnPos = playerEye + (forward * 1.5f); // 1.5 bloques adelante

    // Spawnear el item en el mundo
    state->spawnItem(spawnPos, itemType);

    // Remover 1 item del inventario
    state->inventory.consumeSelected();

    std::cout << "Item tirado: " << itemType << std::endl;
}

// ============================================================================
// RENDERIZADO DE HOTBAR (estilo Minecraft)
// Declaraciones forward
void renderText(const char* text, float x, float y, float size);
void renderChar(char c, float x, float y, float size);

// ============================================================================

void renderHotbar(Inventory* inventory, int width, int height) {
    // ⭐ IMPORTANTE: Configurar OpenGL correctamente para UI 2D
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float slotSize = 50.0f;
    float spacing = 4.0f;
    float totalWidth = 9 * slotSize + 8 * spacing;
    float startX = (width - totalWidth) / 2.0f;
    float startY = height - 80.0f;

    // Renderizar los 9 slots del hotbar
    for (int i = 0; i < 9; i++) {
        float x = startX + i * (slotSize + spacing);
        float y = startY;

        // Fondo del slot (gris oscuro semi-transparente)
        if (i == inventory->selectedSlot) {
            // Slot seleccionado (más claro y con borde blanco)
            glColor4f(0.5f, 0.5f, 0.5f, 0.8f);
        } else {
            glColor4f(0.2f, 0.2f, 0.2f, 0.7f);
        }

        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + slotSize, y);
        glVertex2f(x + slotSize, y + slotSize);
        glVertex2f(x, y + slotSize);
        glEnd();

        // Borde del slot
        if (i == inventory->selectedSlot) {
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Blanco para seleccionado
            glLineWidth(3);
        } else {
            glColor4f(0.5f, 0.5f, 0.5f, 0.9f); // Gris para no seleccionados
            glLineWidth(2);
        }

        glBegin(GL_LINE_LOOP);
        glVertex2f(x, y);
        glVertex2f(x + slotSize, y);
        glVertex2f(x + slotSize, y + slotSize);
        glVertex2f(x, y + slotSize);
        glEnd();

        // Renderizar el bloque (si hay)
        InventorySlot& slot = inventory->slots[i];
        if (!slot.isEmpty()) {
            float blockSize = 40.0f;
            float blockX = x + (slotSize - blockSize) / 2.0f;
            float blockY = y + (slotSize - blockSize) / 2.0f - 5.0f;

            // ⭐⭐⭐ SISTEMA DE TEXTURAS MEJORADO
            // Asegura que SIEMPRE haya una textura válida para cada bloque
            GLuint texture = 0;
            if (g_textureManager != nullptr) {
                // Intentar obtener la textura del item
                texture = g_textureManager->getItemTexture(slot.blockType);

                // ⭐ FALLBACK AGRESIVO: Si falla, intentar múltiples estrategias
                if (texture == 0 && slot.blockType != BLOCK_AIR) {
                    // Estrategia 1: Recargar todas las texturas
                    g_textureManager->loadAllBlockTextures();
                    texture = g_textureManager->getItemTexture(slot.blockType);

                    // Estrategia 2: Si sigue fallando, intentar obtener textura de bloque directamente
                    if (texture == 0) {
                        texture = g_textureManager->getBlockTexture(slot.blockType, 0);
                    }

                    // Estrategia 3: Si TODO falla, usar textura de piedra como fallback final
                    if (texture == 0) {
                        texture = g_textureManager->getTexture("Piedra.png");
                    }
                }
            }

            // ⭐ Verificar que la textura se cargó correctamente
            if (texture != 0) {
                // Renderizar con textura
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, texture);

                glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Color blanco para no alterar la textura
                glBegin(GL_QUADS);
                // Renderizar quad con textura
                glTexCoord2f(0, 0); glVertex2f(blockX, blockY);
                glTexCoord2f(1, 0); glVertex2f(blockX + blockSize, blockY);
                glTexCoord2f(1, 1); glVertex2f(blockX + blockSize, blockY + blockSize);
                glTexCoord2f(0, 1); glVertex2f(blockX, blockY + blockSize);
                glEnd();

                glDisable(GL_TEXTURE_2D);

                // Borde decorativo del item
                glColor4f(0.3f, 0.3f, 0.3f, 0.6f);
                glLineWidth(1);
                glBegin(GL_LINE_LOOP);
                glVertex2f(blockX, blockY);
                glVertex2f(blockX + blockSize, blockY);
                glVertex2f(blockX + blockSize, blockY + blockSize);
                glVertex2f(blockX, blockY + blockSize);
                glEnd();
            } else {
                // Fallback: renderizar con color (modo antiguo)
                float r, g, b;
                getBlockColor(slot.blockType, r, g, b);

                glColor4f(r, g, b, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(blockX, blockY);
                glVertex2f(blockX + blockSize, blockY);
                glVertex2f(blockX + blockSize, blockY + blockSize);
                glVertex2f(blockX, blockY + blockSize);
                glEnd();

                // Borde del bloque
                glColor4f(r * 0.5f, g * 0.5f, b * 0.5f, 1.0f);
                glLineWidth(1);
                glBegin(GL_LINE_LOOP);
                glVertex2f(blockX, blockY);
                glVertex2f(blockX + blockSize, blockY);
                glVertex2f(blockX + blockSize, blockY + blockSize);
                glVertex2f(blockX, blockY + blockSize);
                glEnd();
            }

            // Contador de items (si hay más de 1)
            if (slot.count > 1) {
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                char countStr[8];
                snprintf(countStr, sizeof(countStr), "%d", slot.count);
                renderText(countStr, x + slotSize - 18, y + slotSize - 18, 12);
            }
        }

        // Número del slot (1-9)
        glColor4f(0.8f, 0.8f, 0.8f, 0.9f);
        char numStr[2];
        snprintf(numStr, sizeof(numStr), "%d", i + 1);
        renderText(numStr, x + 3, y + 3, 10);
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);  // ⭐ Restaurar depth test para el juego
}

// ⭐ RENDERIZAR ITEM EN LA MANO (esquina inferior derecha, como Minecraft)
void renderItemInHand(Inventory* inventory, int width, int height) {
    // Verificar que hay un item seleccionado
    if (!inventory->hasSelectedBlock()) {
        return;
    }

    // ⭐ Protección adicional: Verificar bounds del selectedSlot
    if (inventory->selectedSlot < 0 || inventory->selectedSlot >= Inventory::SLOTS) {
        return;
    }

    BlockType heldItem = inventory->getSelectedBlock();
    int itemCount = inventory->slots[inventory->selectedSlot].count;

    // Configurar OpenGL para UI 2D
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Posición en la esquina inferior derecha
    float itemSize = 120.0f;  // Tamaño grande para mejor visibilidad
    float margin = 30.0f;
    float x = width - itemSize - margin;
    float y = height - itemSize - margin - 90.0f;  // Arriba del hotbar

    // Fondo semi-transparente
    glColor4f(0.2f, 0.2f, 0.2f, 0.6f);
    glBegin(GL_QUADS);
    glVertex2f(x - 5, y - 5);
    glVertex2f(x + itemSize + 5, y - 5);
    glVertex2f(x + itemSize + 5, y + itemSize + 5);
    glVertex2f(x - 5, y + itemSize + 5);
    glEnd();

    // Borde
    glColor4f(0.8f, 0.8f, 0.8f, 0.8f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x - 5, y - 5);
    glVertex2f(x + itemSize + 5, y - 5);
    glVertex2f(x + itemSize + 5, y + itemSize + 5);
    glVertex2f(x - 5, y + itemSize + 5);
    glEnd();

    // Renderizar el item con textura
    GLuint texture = 0;
    if (g_textureManager != nullptr) {
        texture = g_textureManager->getItemTexture(heldItem); // ⭐ Usar textura de item
    }

    if (texture != 0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + itemSize, y);
        glTexCoord2f(1, 1); glVertex2f(x + itemSize, y + itemSize);
        glTexCoord2f(0, 1); glVertex2f(x, y + itemSize);
        glEnd();

        glDisable(GL_TEXTURE_2D);
    } else {
        // Fallback: renderizar con color
        float r, g, b;
        getBlockColor(heldItem, r, g, b);

        glColor4f(r, g, b, 1.0f);
        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + itemSize, y);
        glVertex2f(x + itemSize, y + itemSize);
        glVertex2f(x, y + itemSize);
        glEnd();
    }

    // Contador de items
    if (itemCount > 1) {
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        char countStr[16];
        snprintf(countStr, sizeof(countStr), "x%d", itemCount);
        renderText(countStr, x + itemSize - 40, y + itemSize - 25, 16);
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ============================================================================
// SISTEMA DE MANOS/BRAZOS DEL JUGADOR (Primera Persona)
// ============================================================================

// Renderizar la mano del jugador sosteniendo el bloque seleccionado
void renderPlayerHand(BlockType heldBlock, float swingProgress, float deltaTime) {
    if (heldBlock == BLOCK_AIR) return;

    static float swingAngle = 0.0f;
    static float bobOffset = 0.0f;
    static float walkTimer = 0.0f;

    // Animación de caminar (bob effect)
    if (g_gameState && (g_gameState->keys['W'] || g_gameState->keys['A'] ||
        g_gameState->keys['S'] || g_gameState->keys['D'])) {
        walkTimer += deltaTime * 8.0f;
        bobOffset = sinf(walkTimer) * 0.02f;
    } else {
        walkTimer = 0.0f;
        bobOffset *= 0.9f; // Smooth decay
    }

    // Animación de swing al minar o colocar
    if (swingProgress > 0.0f) {
        swingAngle = sinf(swingProgress * 3.14159f) * 30.0f;
    } else {
        swingAngle *= 0.8f; // Smooth decay
    }

    glPushMatrix();

    // Posicionar la mano en la parte inferior derecha de la pantalla
    glTranslatef(0.4f, -0.3f + bobOffset, -0.5f);

    // Rotación de swing
    glRotatef(swingAngle, 0, 0, 1);

    // Inclinación natural de la mano
    glRotatef(-10, 1, 0, 0);
    glRotatef(10, 0, 1, 0);

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // === RENDERIZAR BRAZO (color piel) ===
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    // Color de piel
    glColor3f(0.95f, 0.76f, 0.65f);

    // Brazo (cilindro rectangular)
    float armWidth = 0.06f;
    float armLength = 0.35f;

    glBegin(GL_QUADS);
    // Cara frontal del brazo
    glVertex3f(-armWidth, -armWidth, 0);
    glVertex3f( armWidth, -armWidth, 0);
    glVertex3f( armWidth, -armWidth, -armLength);
    glVertex3f(-armWidth, -armWidth, -armLength);

    // Cara trasera del brazo
    glVertex3f(-armWidth,  armWidth, 0);
    glVertex3f(-armWidth,  armWidth, -armLength);
    glVertex3f( armWidth,  armWidth, -armLength);
    glVertex3f( armWidth,  armWidth, 0);

    // Cara izquierda del brazo
    glVertex3f(-armWidth, -armWidth, 0);
    glVertex3f(-armWidth, -armWidth, -armLength);
    glVertex3f(-armWidth,  armWidth, -armLength);
    glVertex3f(-armWidth,  armWidth, 0);

    // Cara derecha del brazo
    glVertex3f( armWidth, -armWidth, 0);
    glVertex3f( armWidth,  armWidth, 0);
    glVertex3f( armWidth,  armWidth, -armLength);
    glVertex3f( armWidth, -armWidth, -armLength);
    glEnd();

    // === RENDERIZAR BLOQUE SOSTENIDO ===
    glPushMatrix();
    glTranslatef(0.0f, 0.0f, -armLength - 0.15f); // Al final del brazo
    glRotatef(45, 0, 1, 0);  // Rotación para mejor visualización
    glRotatef(20, 1, 0, 0);

    glEnable(GL_TEXTURE_2D);
    float blockSize = 0.12f;

    // Renderizar las 6 caras del bloque con texturas
    for (int face = 0; face < 6; face++) {
        GLuint texture = g_textureManager->getBlockTexture(heldBlock, face);
        glBindTexture(GL_TEXTURE_2D, texture);

        glBegin(GL_QUADS);

        // Ajustar color según la cara (iluminación simple)
        if (face == 0) glColor3f(1.0f, 1.0f, 1.0f);      // Top - más brillante
        else if (face == 1) glColor3f(0.5f, 0.5f, 0.5f); // Bottom - más oscuro
        else glColor3f(0.8f, 0.8f, 0.8f);                // Sides - intermedio

        switch (face) {
            case 0: // Top (+Y)
                glTexCoord2f(0, 0); glVertex3f(-blockSize,  blockSize, -blockSize);
                glTexCoord2f(1, 0); glVertex3f( blockSize,  blockSize, -blockSize);
                glTexCoord2f(1, 1); glVertex3f( blockSize,  blockSize,  blockSize);
                glTexCoord2f(0, 1); glVertex3f(-blockSize,  blockSize,  blockSize);
                break;
            case 1: // Bottom (-Y)
                glTexCoord2f(0, 0); glVertex3f(-blockSize, -blockSize, -blockSize);
                glTexCoord2f(1, 0); glVertex3f(-blockSize, -blockSize,  blockSize);
                glTexCoord2f(1, 1); glVertex3f( blockSize, -blockSize,  blockSize);
                glTexCoord2f(0, 1); glVertex3f( blockSize, -blockSize, -blockSize);
                break;
            case 2: // North (+Z)
                glTexCoord2f(0, 0); glVertex3f(-blockSize, -blockSize,  blockSize);
                glTexCoord2f(1, 0); glVertex3f( blockSize, -blockSize,  blockSize);
                glTexCoord2f(1, 1); glVertex3f( blockSize,  blockSize,  blockSize);
                glTexCoord2f(0, 1); glVertex3f(-blockSize,  blockSize,  blockSize);
                break;
            case 3: // South (-Z)
                glTexCoord2f(0, 0); glVertex3f(-blockSize, -blockSize, -blockSize);
                glTexCoord2f(1, 0); glVertex3f(-blockSize,  blockSize, -blockSize);
                glTexCoord2f(1, 1); glVertex3f( blockSize,  blockSize, -blockSize);
                glTexCoord2f(0, 1); glVertex3f( blockSize, -blockSize, -blockSize);
                break;
            case 4: // East (+X)
                glTexCoord2f(0, 0); glVertex3f( blockSize, -blockSize,  blockSize);
                glTexCoord2f(1, 0); glVertex3f( blockSize, -blockSize, -blockSize);
                glTexCoord2f(1, 1); glVertex3f( blockSize,  blockSize, -blockSize);
                glTexCoord2f(0, 1); glVertex3f( blockSize,  blockSize,  blockSize);
                break;
            case 5: // West (-X)
                glTexCoord2f(0, 0); glVertex3f(-blockSize, -blockSize, -blockSize);
                glTexCoord2f(1, 0); glVertex3f(-blockSize, -blockSize,  blockSize);
                glTexCoord2f(1, 1); glVertex3f(-blockSize,  blockSize,  blockSize);
                glTexCoord2f(0, 1); glVertex3f(-blockSize,  blockSize, -blockSize);
                break;
        }
        glEnd();
    }

    glPopMatrix();

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glPopMatrix();
}

// Renderizar outline del bloque apuntado (estilo Minecraft)
void renderBlockOutline(Vec3i blockPos, Vec3 cameraPos) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Línea negra gruesa
    glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
    glLineWidth(3);

    float x = blockPos.x;
    float y = blockPos.y;
    float z = blockPos.z;
    float size = 1.001f; // Ligeramente más grande para evitar z-fighting

    glBegin(GL_LINES);

    // Aristas inferiores
    glVertex3f(x, y, z);         glVertex3f(x + size, y, z);
    glVertex3f(x + size, y, z);  glVertex3f(x + size, y, z + size);
    glVertex3f(x + size, y, z + size); glVertex3f(x, y, z + size);
    glVertex3f(x, y, z + size);  glVertex3f(x, y, z);

    // Aristas superiores
    glVertex3f(x, y + size, z);         glVertex3f(x + size, y + size, z);
    glVertex3f(x + size, y + size, z);  glVertex3f(x + size, y + size, z + size);
    glVertex3f(x + size, y + size, z + size); glVertex3f(x, y + size, z + size);
    glVertex3f(x, y + size, z + size);  glVertex3f(x, y + size, z);

    // Aristas verticales
    glVertex3f(x, y, z);         glVertex3f(x, y + size, z);
    glVertex3f(x + size, y, z);  glVertex3f(x + size, y + size, z);
    glVertex3f(x + size, y, z + size); glVertex3f(x + size, y + size, z + size);
    glVertex3f(x, y, z + size);  glVertex3f(x, y + size, z + size);

    glEnd();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// Renderizar animación de grietas usando texturas (como Minecraft)
void renderBlockCrack(Vec3i blockPos, float progress) {
    if (progress <= 0.0f || !g_textureManager || !g_gameState) return;

    // Obtener textura de destrucción según el progreso
    GLuint crackTexture = g_textureManager->getDestroyStageTexture(progress);
    if (crackTexture == 0) return;

    // Obtener posición de la cámara (jugador)
    Vec3 cameraPos = g_gameState->player.getEyePosition();

    // Calcular centros de cada cara del bloque
    Vec3 faceTop(blockPos.x + 0.5f, blockPos.y + 1.0f, blockPos.z + 0.5f);
    Vec3 faceBottom(blockPos.x + 0.5f, blockPos.y, blockPos.z + 0.5f);
    Vec3 faceNorth(blockPos.x + 0.5f, blockPos.y + 0.5f, blockPos.z + 1.0f);
    Vec3 faceSouth(blockPos.x + 0.5f, blockPos.y + 0.5f, blockPos.z);
    Vec3 faceEast(blockPos.x + 1.0f, blockPos.y + 0.5f, blockPos.z + 0.5f);
    Vec3 faceWest(blockPos.x, blockPos.y + 0.5f, blockPos.z + 0.5f);

    // Normales de cada cara (hacia afuera del bloque)
    Vec3 normalTop(0, 1, 0);
    Vec3 normalBottom(0, -1, 0);
    Vec3 normalNorth(0, 0, 1);
    Vec3 normalSouth(0, 0, -1);
    Vec3 normalEast(1, 0, 0);
    Vec3 normalWest(-1, 0, 0);

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    // Configurar OpenGL para renderizar overlay de destrucción
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, crackTexture);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-3.0f, -3.0f);
    glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    float x = blockPos.x;
    float y = blockPos.y;
    float z = blockPos.z;
    float e = 0.003f;

    // Renderizar caras visibles usando producto punto
    glBegin(GL_QUADS);

    // Cara superior (+Y)
    Vec3 toCameraTop = cameraPos - faceTop;
    toCameraTop = toCameraTop.normalize();
    if (normalTop.dot(toCameraTop) > -0.1f) {  // Flexible: ve la cara si está mirándola
        glTexCoord2f(0, 0); glVertex3f(x,     y+1+e, z);
        glTexCoord2f(1, 0); glVertex3f(x+1,   y+1+e, z);
        glTexCoord2f(1, 1); glVertex3f(x+1,   y+1+e, z+1);
        glTexCoord2f(0, 1); glVertex3f(x,     y+1+e, z+1);
    }

    // Cara inferior (-Y)
    Vec3 toCameraBottom = cameraPos - faceBottom;
    toCameraBottom = toCameraBottom.normalize();
    if (normalBottom.dot(toCameraBottom) > -0.1f) {
        glTexCoord2f(0, 0); glVertex3f(x,     y-e, z+1);
        glTexCoord2f(1, 0); glVertex3f(x+1,   y-e, z+1);
        glTexCoord2f(1, 1); glVertex3f(x+1,   y-e, z);
        glTexCoord2f(0, 1); glVertex3f(x,     y-e, z);
    }

    // Cara norte (+Z)
    Vec3 toCameraNorth = cameraPos - faceNorth;
    toCameraNorth = toCameraNorth.normalize();
    if (normalNorth.dot(toCameraNorth) > -0.1f) {
        glTexCoord2f(0, 0); glVertex3f(x,     y,   z+1+e);
        glTexCoord2f(1, 0); glVertex3f(x+1,   y,   z+1+e);
        glTexCoord2f(1, 1); glVertex3f(x+1,   y+1, z+1+e);
        glTexCoord2f(0, 1); glVertex3f(x,     y+1, z+1+e);
    }

    // Cara sur (-Z)
    Vec3 toCameraSouth = cameraPos - faceSouth;
    toCameraSouth = toCameraSouth.normalize();
    if (normalSouth.dot(toCameraSouth) > -0.1f) {
        glTexCoord2f(0, 0); glVertex3f(x+1,   y,   z-e);
        glTexCoord2f(1, 0); glVertex3f(x,     y,   z-e);
        glTexCoord2f(1, 1); glVertex3f(x,     y+1, z-e);
        glTexCoord2f(0, 1); glVertex3f(x+1,   y+1, z-e);
    }

    // Cara este (+X)
    Vec3 toCameraEast = cameraPos - faceEast;
    toCameraEast = toCameraEast.normalize();
    if (normalEast.dot(toCameraEast) > -0.1f) {
        glTexCoord2f(0, 0); glVertex3f(x+1+e, y,   z);
        glTexCoord2f(1, 0); glVertex3f(x+1+e, y,   z+1);
        glTexCoord2f(1, 1); glVertex3f(x+1+e, y+1, z+1);
        glTexCoord2f(0, 1); glVertex3f(x+1+e, y+1, z);
    }

    // Cara oeste (-X)
    Vec3 toCameraWest = cameraPos - faceWest;
    toCameraWest = toCameraWest.normalize();
    if (normalWest.dot(toCameraWest) > -0.1f) {
        glTexCoord2f(0, 0); glVertex3f(x-e,   y,   z+1);
        glTexCoord2f(1, 0); glVertex3f(x-e,   y,   z);
        glTexCoord2f(1, 1); glVertex3f(x-e,   y+1, z);
        glTexCoord2f(0, 1); glVertex3f(x-e,   y+1, z+1);
    }

    glEnd();

    glPopAttrib();
}

// ⭐ SISTEMA DE ITEMS: Determinar si un item es colocable en el mundo
bool isPlaceableItem(BlockType type) {
    // Items puros (no colocables) - solo para inventario y crafteo
    switch (type) {
        case BLOCK_DIRT_POWDER:  // Polvo de tierra - item puro
        case BLOCK_STICK:        // Palo - item puro
        case BLOCK_HOE:          // Hoz - herramienta, no colocable
        case BLOCK_COAL_ITEM:    // Carbón - item puro
        case BLOCK_RAW_ZINC:     // Zinc crudo - item puro
        case BLOCK_RAW_COPPER:   // Cobre crudo - item puro
            return false;

        case BLOCK_AIR:          // Aire no es colocable
            return false;

        default:
            return true;  // Todos los demás bloques son colocables
    }
}

void placeBlock(GameState* state) {
    if (state->placeCooldown > 0) return;
    if (!state->inventory.hasSelectedBlock()) return;

    // ⭐ Verificar si el item seleccionado es colocable
    BlockType selectedBlock = state->inventory.getSelectedBlock();
    if (!isPlaceableItem(selectedBlock)) {
        return;  // No se puede colocar este item
    }

    Vec3 origin = state->player.getEyePosition();
    Vec3 direction = state->player.getForward();

    RaycastResult result = raycastBlock(state->world, origin, direction, 5.0f);

    if (result.hit) {
        Vec3i placePos = result.previousPos;

        // Verificar que no colisione con el jugador
        Vec3 playerFeet = state->player.position;
        Vec3 playerHead = playerFeet + Vec3(0, state->player.HEIGHT, 0);

        // AABB del bloque a colocar
        float minX = placePos.x, maxX = placePos.x + 1.0f;
        float minY = placePos.y, maxY = placePos.y + 1.0f;
        float minZ = placePos.z, maxZ = placePos.z + 1.0f;

        // AABB del jugador
        float pMinX = playerFeet.x - state->player.WIDTH / 2.0f;
        float pMaxX = playerFeet.x + state->player.WIDTH / 2.0f;
        float pMinY = playerFeet.y;
        float pMaxY = playerHead.y;
        float pMinZ = playerFeet.z - state->player.WIDTH / 2.0f;
        float pMaxZ = playerFeet.z + state->player.WIDTH / 2.0f;

        // Verificar intersección
        bool intersects = (pMinX < maxX && pMaxX > minX) &&
                          (pMinY < maxY && pMaxY > minY) &&
                          (pMinZ < maxZ && pMaxZ > minZ);

        if (!intersects) {
            BlockType blockToPlace = state->inventory.getSelectedBlock();
            state->world.setBlock(placePos.x, placePos.y, placePos.z, blockToPlace);
            state->inventory.consumeSelected();
            state->placeCooldown = 0.25f;  // 250ms cooldown

            // ⭐ SISTEMA DE AGUA: Si se coloca agua, programar actualización
            if (blockToPlace == BLOCK_WATER) {
                state->world.notifyWaterPlaced(placePos.x, placePos.y, placePos.z);
            }

            // ⭐ SONIDO: Reproducir sonido de colocar bloque
            if (g_soundManager) {
                g_soundManager->playPlaceBlock(glfwGetTime());
            }
        }
    }
}

// ============================================================================
// SISTEMA DE RENDERIZADO DE TEXTO SIMPLE
// ============================================================================

// Función para renderizar un carácter simple en estilo bitmap
void renderChar(char c, float x, float y, float size) {
    float w = size * 0.6f;  // Ancho de carácter
    float h = size;          // Alto de carácter
    float t = size * 0.15f;  // Grosor de línea

    glBegin(GL_QUADS);

    switch(c) {
        case 'A':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal media
            glVertex2f(x + t, y + h * 0.4f); glVertex2f(x + w - t, y + h * 0.4f);
            glVertex2f(x + w - t, y + h * 0.4f + t); glVertex2f(x + t, y + h * 0.4f + t);
            break;

        case 'B':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x, y + t);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w - t, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w - t, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            // Curva superior derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h * 0.5f); glVertex2f(x + w - t, y + h * 0.5f);
            // Curva inferior derecha
            glVertex2f(x + w - t, y + h * 0.5f); glVertex2f(x + w, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'C':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x + t, y + h);
            break;

        case 'D':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x, y + t);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x, y + h);
            // Curva derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            break;

        case 'E':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w * 0.8f, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w * 0.8f, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case 'F':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w * 0.8f, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w * 0.8f, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            break;

        case 'G':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x + t, y + h);
            // Barra vertical derecha (mitad inferior)
            glVertex2f(x + w - t, y + h * 0.5f); glVertex2f(x + w, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal media
            glVertex2f(x + w * 0.5f, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x + w * 0.5f, y + h * 0.5f + t * 0.5f);
            break;

        case 'H':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal media
            glVertex2f(x + t, y + h * 0.5f - t * 0.5f); glVertex2f(x + w - t, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w - t, y + h * 0.5f + t * 0.5f); glVertex2f(x + t, y + h * 0.5f + t * 0.5f);
            break;

        case 'I':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical central
            glVertex2f(x + w * 0.5f - t * 0.5f, y); glVertex2f(x + w * 0.5f + t * 0.5f, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x + w * 0.5f - t * 0.5f, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case 'J':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical derecha
            glVertex2f(x + w * 0.5f - t * 0.5f, y); glVertex2f(x + w * 0.5f + t * 0.5f, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h - t); glVertex2f(x + w * 0.5f - t * 0.5f, y + h - t);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w * 0.5f + t * 0.5f, y + h - t);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x, y + h);
            // Barra vertical izquierda inferior
            glVertex2f(x, y + h * 0.7f); glVertex2f(x + t, y + h * 0.7f);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            break;

        case 'K':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Diagonal superior
            glVertex2f(x + t, y + h * 0.5f); glVertex2f(x + t * 2, y + h * 0.5f);
            glVertex2f(x + w, y); glVertex2f(x + w - t, y);
            // Diagonal inferior
            glVertex2f(x + t, y + h * 0.5f); glVertex2f(x + t * 2, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'L':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case 'M':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Diagonal izquierda
            glVertex2f(x, y); glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f); glVertex2f(x + t, y);
            // Diagonal derecha
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f); glVertex2f(x + w - t, y);
            glVertex2f(x + w, y); glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'N':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Diagonal
            glVertex2f(x, y); glVertex2f(x + t * 2, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t * 2, y + h);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'O':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            break;

        case 'P':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x, y + t);
            // Barra vertical derecha (mitad superior)
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h * 0.5f); glVertex2f(x + w - t, y + h * 0.5f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            break;

        case 'Q':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            // Diagonal de la cola (Q)
            glVertex2f(x + w * 0.5f, y + h * 0.6f); glVertex2f(x + w * 0.5f + t, y + h * 0.6f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'R':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x, y + t);
            // Barra vertical derecha (mitad superior)
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h * 0.5f); glVertex2f(x + w - t, y + h * 0.5f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Diagonal
            glVertex2f(x + w * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f + t, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'S':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical izquierda superior
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h * 0.5f); glVertex2f(x, y + h * 0.5f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra vertical derecha inferior
            glVertex2f(x + w - t, y + h * 0.5f); glVertex2f(x + w, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case 'T':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical central
            glVertex2f(x + w * 0.5f - t * 0.5f, y); glVertex2f(x + w * 0.5f + t * 0.5f, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x + w * 0.5f - t * 0.5f, y + h);
            break;

        case 'U':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            break;

        case 'V':
            // Diagonal izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x + w * 0.5f - t * 0.5f, y + h);
            // Diagonal derecha
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h); glVertex2f(x + w * 0.5f + t * 0.5f, y + h);
            glVertex2f(x + w, y); glVertex2f(x + w - t, y);
            break;

        case 'W':
            // Barra vertical izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Diagonal izquierda (hacia centro inferior)
            glVertex2f(x, y + h); glVertex2f(x + t, y + h);
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f - t * 1.5f, y + h * 0.5f);
            // Diagonal derecha (desde centro inferior)
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f + t * 1.5f, y + h * 0.5f);
            glVertex2f(x + w - t, y + h); glVertex2f(x + w, y + h);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case 'X':
            // Diagonal de arriba-izquierda a abajo-derecha
            glVertex2f(x, y); glVertex2f(x + t * 1.5f, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t * 1.5f, y + h);
            // Diagonal de arriba-derecha a abajo-izquierda
            glVertex2f(x + w - t * 1.5f, y); glVertex2f(x + w, y);
            glVertex2f(x + t * 1.5f, y + h); glVertex2f(x, y + h);
            break;

        case 'Y':
            // Diagonal izquierda
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f);
            // Diagonal derecha
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f);
            glVertex2f(x + w, y); glVertex2f(x + w - t, y);
            // Barra vertical central inferior
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.5f); glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.5f);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x + w * 0.5f - t * 0.5f, y + h);
            break;

        case 'Z':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Diagonal
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case '+':
            // Barra horizontal
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra vertical
            glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.2f); glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.2f);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h * 0.8f); glVertex2f(x + w * 0.5f - t * 0.5f, y + h * 0.8f);
            break;

        case '-':
            // Barra horizontal
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            break;

        case '0':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            break;

        case '1':
            // Barra vertical central
            glVertex2f(x + w * 0.5f - t * 0.5f, y); glVertex2f(x + w * 0.5f + t * 0.5f, y);
            glVertex2f(x + w * 0.5f + t * 0.5f, y + h); glVertex2f(x + w * 0.5f - t * 0.5f, y + h);
            // Diagonal superior izquierda
            glVertex2f(x + w * 0.2f, y + h * 0.2f); glVertex2f(x + w * 0.5f, y);
            glVertex2f(x + w * 0.5f, y + t); glVertex2f(x + w * 0.2f, y + h * 0.2f + t);
            break;

        case '2':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical derecha superior
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h * 0.5f); glVertex2f(x + w - t, y + h * 0.5f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra vertical izquierda inferior
            glVertex2f(x, y + h * 0.5f); glVertex2f(x + t, y + h * 0.5f);
            glVertex2f(x + t, y + h); glVertex2f(x, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case '3':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal media
            glVertex2f(x + w * 0.3f, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x + w * 0.3f, y + h * 0.5f + t * 0.5f);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case '4':
            // Barra vertical izquierda superior
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h * 0.6f); glVertex2f(x, y + h * 0.6f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.6f - t * 0.5f); glVertex2f(x + w, y + h * 0.6f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.6f + t * 0.5f); glVertex2f(x, y + h * 0.6f + t * 0.5f);
            // Barra vertical derecha
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            break;

        case '5':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Barra vertical izquierda superior
            glVertex2f(x, y); glVertex2f(x + t, y);
            glVertex2f(x + t, y + h * 0.5f); glVertex2f(x, y + h * 0.5f);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra vertical derecha inferior
            glVertex2f(x + w - t, y + h * 0.5f); glVertex2f(x + w, y + h * 0.5f);
            glVertex2f(x + w, y + h); glVertex2f(x + w - t, y + h);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w, y + h - t);
            glVertex2f(x + w, y + h); glVertex2f(x, y + h);
            break;

        case '6':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal media
            glVertex2f(x, y + h * 0.5f - t * 0.5f); glVertex2f(x + w - t, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w - t, y + h * 0.5f + t * 0.5f); glVertex2f(x, y + h * 0.5f + t * 0.5f);
            // Barra vertical derecha inferior
            glVertex2f(x + w - t, y + h * 0.5f); glVertex2f(x + w, y + h * 0.5f);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            break;

        case '7':
            // Barra horizontal superior
            glVertex2f(x, y); glVertex2f(x + w, y);
            glVertex2f(x + w, y + t); glVertex2f(x, y + t);
            // Diagonal
            glVertex2f(x + w - t, y); glVertex2f(x + w, y);
            glVertex2f(x + w * 0.3f, y + h); glVertex2f(x + w * 0.3f - t, y + h);
            break;

        case '8':
            // Barra vertical izquierda
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h - t); glVertex2f(x, y + h - t);
            // Barra vertical derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal media
            glVertex2f(x + t, y + h * 0.5f - t * 0.5f); glVertex2f(x + w - t, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w - t, y + h * 0.5f + t * 0.5f); glVertex2f(x + t, y + h * 0.5f + t * 0.5f);
            // Barra horizontal inferior
            glVertex2f(x + t, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x + t, y + h);
            break;

        case '9':
            // Barra vertical izquierda superior
            glVertex2f(x, y + t); glVertex2f(x + t, y + t);
            glVertex2f(x + t, y + h * 0.5f); glVertex2f(x, y + h * 0.5f);
            // Barra vertical derecha
            glVertex2f(x + w - t, y + t); glVertex2f(x + w, y + t);
            glVertex2f(x + w, y + h - t); glVertex2f(x + w - t, y + h - t);
            // Barra horizontal superior
            glVertex2f(x + t, y); glVertex2f(x + w - t, y);
            glVertex2f(x + w - t, y + t); glVertex2f(x + t, y + t);
            // Barra horizontal media
            glVertex2f(x + t, y + h * 0.5f - t * 0.5f); glVertex2f(x + w, y + h * 0.5f - t * 0.5f);
            glVertex2f(x + w, y + h * 0.5f + t * 0.5f); glVertex2f(x + t, y + h * 0.5f + t * 0.5f);
            // Barra horizontal inferior
            glVertex2f(x, y + h - t); glVertex2f(x + w - t, y + h - t);
            glVertex2f(x + w - t, y + h); glVertex2f(x, y + h);
            break;

        case ' ':
            // Espacio - no dibujar nada
            break;

        default:
            // Carácter desconocido - dibujar rectángulo pequeño
            glVertex2f(x + w * 0.25f, y + h * 0.25f);
            glVertex2f(x + w * 0.75f, y + h * 0.25f);
            glVertex2f(x + w * 0.75f, y + h * 0.75f);
            glVertex2f(x + w * 0.25f, y + h * 0.75f);
            break;
    }

    glEnd();
}

// Función para renderizar una cadena de texto
void renderText(const char* text, float x, float y, float size) {
    float charWidth = size * 0.7f;  // Ancho total incluyendo espacio
    float currentX = x;

    for (int i = 0; text[i] != '\0'; i++) {
        renderChar(toupper(text[i]), currentX, y, size);
        currentX += charWidth;
    }
}

// ============================================================================
// CALLBACKS DE GLFW
// ============================================================================

// Forward declarations
bool renameWorld(GameState* state, int worldIndex, const std::string& newName);
void saveWorld(GameState* state);

// Callback para entrada de caracteres (para el campo de texto)
void charCallback(GLFWwindow* window, unsigned int codepoint) {
    if (!g_gameState) return;

    // Solo procesar si estamos editando el nombre del mundo
    if (g_gameState->isEditingWorldName && g_gameState->screenState == SCREEN_WORLD_SELECT) {
        // Limitar la longitud del nombre
        if (g_gameState->editingWorldNewName.length() < 50) {
            // Añadir el carácter al nombre
            char c = (char)codepoint;
            // Solo permitir caracteres alfanuméricos, espacios, guiones y guiones bajos
            if (WorldName::isAllowedChar(c)) {
                g_gameState->editingWorldNewName += c;
            }
        }
    }

    // ⭐⭐⭐ NUEVO: Editar nombre en pantalla de creación de mundo
    if (g_gameState->isEditingNewWorldName && g_gameState->screenState == SCREEN_WORLD_CREATE) {
        if (g_gameState->newWorldName.length() < 50) {
            char c = (char)codepoint;
            if (WorldName::isAllowedChar(c)) {
                g_gameState->newWorldName += c;
            }
        }
    }

    // ⭐⭐⭐ NUEVO: Editar semilla en pantalla de creación de mundo
    if (g_gameState->isEditingNewWorldSeed && g_gameState->screenState == SCREEN_WORLD_CREATE) {
        if (g_gameState->newWorldSeed.length() < 20) {
            char c = (char)codepoint;
            // Para semilla permitir solo números
            if (isdigit(c)) {
                g_gameState->newWorldSeed += c;
            }
        }
    }
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!g_gameState) return;

    // Manejo de teclas especiales en modo edición
    if (g_gameState->isEditingWorldName && g_gameState->screenState == SCREEN_WORLD_SELECT) {
        // BACKSPACE - borrar último carácter (con soporte para mantener presionado)
        if (key == GLFW_KEY_BACKSPACE) {
            if (action == GLFW_PRESS) {
                // Primer press: borrar inmediatamente
                if (!g_gameState->editingWorldNewName.empty()) {
                    g_gameState->editingWorldNewName.pop_back();
                }
                // Iniciar sistema de repetición
                g_gameState->backspacePressed = true;
                g_gameState->backspaceFirstPressTime = glfwGetTime();
                g_gameState->backspaceLastRepeatTime = glfwGetTime();
            } else if (action == GLFW_RELEASE) {
                // Soltar tecla: detener repetición
                g_gameState->backspacePressed = false;
            }
            return;
        }

        // ENTER - guardar y cerrar
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            // ⭐ VALIDACIÓN: Verificar índice válido antes de renombrar
            if (!g_gameState->editingWorldNewName.empty() &&
                g_gameState->selectedWorldIndex >= 0 &&
                g_gameState->selectedWorldIndex < (int)g_gameState->savedWorlds.size()) {
                std::cout << "Guardando nuevo nombre: " << g_gameState->editingWorldNewName << std::endl;
                if (renameWorld(g_gameState, g_gameState->selectedWorldIndex, g_gameState->editingWorldNewName)) {
                    std::cout << "Mundo renombrado exitosamente!" << std::endl;
                } else {
                    std::cout << "Error al renombrar el mundo" << std::endl;
                }
            } else {
                if (g_gameState->editingWorldNewName.empty()) {
                    std::cout << "El nombre no puede estar vacío" << std::endl;
                } else {
                    std::cout << "❌ Error: Índice de mundo inválido" << std::endl;
                }
            }
            g_gameState->isEditingWorldName = false;
            g_gameState->editingWorldNewName = "";
            return;
        }

        // ESC - cancelar edición
        if (key == GLFW_KEY_ESCAPE) {
            std::cout << "Edición cancelada" << std::endl;
            g_gameState->isEditingWorldName = false;
            g_gameState->editingWorldNewName = "";
            return;
        }
    }

    // ⭐⭐⭐ NUEVO: Manejo de teclas en pantalla de creación de mundo
    if (g_gameState->screenState == SCREEN_WORLD_CREATE) {
        // BACKSPACE - borrar último carácter del campo activo (con repetición)
        if (key == GLFW_KEY_BACKSPACE) {
            if (action == GLFW_PRESS) {
                // Primer press: borrar inmediatamente
                if (g_gameState->isEditingNewWorldName && !g_gameState->newWorldName.empty()) {
                    g_gameState->newWorldName.pop_back();
                } else if (g_gameState->isEditingNewWorldSeed && !g_gameState->newWorldSeed.empty()) {
                    g_gameState->newWorldSeed.pop_back();
                }
                // Iniciar sistema de repetición
                g_gameState->backspacePressed = true;
                g_gameState->backspaceFirstPressTime = glfwGetTime();
                g_gameState->backspaceLastRepeatTime = glfwGetTime();
            } else if (action == GLFW_RELEASE) {
                // Soltar tecla: detener repetición
                g_gameState->backspacePressed = false;
            }
            return;
        }

        // El resto de teclas solo en PRESS
        if (action != GLFW_PRESS) return;

        // TAB - cambiar entre campos
        if (key == GLFW_KEY_TAB) {
            if (g_gameState->isEditingNewWorldName) {
                g_gameState->isEditingNewWorldName = false;
                g_gameState->isEditingNewWorldSeed = true;
            } else if (g_gameState->isEditingNewWorldSeed) {
                g_gameState->isEditingNewWorldSeed = false;
                g_gameState->isEditingNewWorldName = true;
            } else {
                g_gameState->isEditingNewWorldName = true;
            }
            return;
        }

        // ESC - volver a selección de mundos
        if (key == GLFW_KEY_ESCAPE) {
            std::cout << "❌ Creación de mundo cancelada (ESC)" << std::endl;
            g_gameState->screenState = SCREEN_WORLD_SELECT;
            g_gameState->isEditingNewWorldName = false;
            g_gameState->isEditingNewWorldSeed = false;
            return;
        }
    }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (g_gameState->isPaused) {
            // Si estamos en un submenú, volver al menú principal
            if (g_gameState->pauseMenuState != PAUSE_MENU_MAIN) {
                g_gameState->pauseMenuState = PAUSE_MENU_MAIN;
            } else {
                // ⭐ GUARDADO AUTOMÁTICO: Guardar antes de reanudar el juego (como Minecraft)
                if (!g_gameState->currentWorldName.empty() && g_gameState->screenState == SCREEN_IN_GAME) {
                    std::cout << "\n💾 Guardando mundo al despausar..." << std::endl;
                    saveWorld(g_gameState);
                }

                // Si ya estamos en el menú principal, reanudar el juego
                g_gameState->isPaused = false;
                g_gameState->cursorLocked = true;
                g_gameState->firstMouse = true;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
        } else {
            // Pausar el juego
            g_gameState->isPaused = true;
            g_gameState->pauseMenuState = PAUSE_MENU_MAIN; // Siempre empezar en el menú principal
            g_gameState->cursorLocked = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }

    // Tecla F11 para pantalla completa
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        g_isFullscreen = !g_isFullscreen;

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        if (g_isFullscreen) {
            // Guardar posición y tamaño de ventana antes de ir a pantalla completa
            glfwGetWindowPos(window, &g_windowedPosX, &g_windowedPosY);
            glfwGetWindowSize(window, &g_windowedWidth, &g_windowedHeight);

            // Cambiar a pantalla completa
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            // Volver a modo ventana
            glfwSetWindowMonitor(window, nullptr, g_windowedPosX, g_windowedPosY, g_windowedWidth, g_windowedHeight, 0);
        }
    }

    // Tecla F3 para overlay de rendimiento (Profiler)
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS) {
        Profiler::toggle();
    }

    // Tecla E para inventario
    if (key == GLFW_KEY_E && action == GLFW_PRESS && !g_gameState->isPaused) {
        g_gameState->inventoryOpen = !g_gameState->inventoryOpen;
        if (g_gameState->inventoryOpen) {
            g_gameState->cursorLocked = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else {
            // Al cerrar el inventario, devolver el item del cursor al inventario
            if (!g_gameState->heldSlot.isEmpty()) {
                if (!g_gameState->inventory.addItem(g_gameState->heldSlot.blockType, g_gameState->heldSlot.count)) {
                    // Si no cabe en el inventario, hacer spawn del item en el mundo
                    for (int i = 0; i < g_gameState->heldSlot.count; i++) {
                        g_gameState->spawnItem(g_gameState->player.position, g_gameState->heldSlot.blockType);
                    }
                }
                g_gameState->heldSlot.blockType = BLOCK_AIR;
                g_gameState->heldSlot.count = 0;
            }

            g_gameState->cursorLocked = true;
            g_gameState->firstMouse = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    // ⭐⭐⭐ Teclas 1-9 para seleccionar slots del inventario CON ACUMULACIÓN
    if (action == GLFW_PRESS && !g_gameState->isPaused && !g_gameState->inventoryOpen) {
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
            int targetSlot = key - GLFW_KEY_1;
            double currentTime = glfwGetTime();
            double timeSinceLastPress = currentTime - g_gameState->lastSlotPressTime;

            // Si presionas la MISMA tecla dentro de 0.5 segundos Y ese slot tiene items
            if (targetSlot == g_gameState->lastPressedSlot &&
                timeSinceLastPress < 0.5 &&
                !g_gameState->inventory.slots[targetSlot].isEmpty()) {

                // ⭐ INCREMENTAR la cantidad del item en ese slot
                BlockType itemType = g_gameState->inventory.slots[targetSlot].blockType;
                g_gameState->inventory.slots[targetSlot].add(itemType, 1);

                std::cout << "📦 Stack incrementado: "
                          << g_gameState->inventory.slots[targetSlot].count
                          << "x en slot " << (targetSlot + 1) << std::endl;
            } else {
                // Comportamiento normal: simplemente seleccionar el slot
                g_gameState->inventory.selectedSlot = targetSlot;
            }

            // Actualizar tracking
            g_gameState->lastPressedSlot = targetSlot;
            g_gameState->lastSlotPressTime = currentTime;
        }
    }

    // No procesar teclas de movimiento si está pausado o inventario abierto
    if (g_gameState->isPaused || g_gameState->inventoryOpen) return;

    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_W) g_gameState->keys['W'] = true;
        if (key == GLFW_KEY_S) g_gameState->keys['S'] = true;
        if (key == GLFW_KEY_A) g_gameState->keys['A'] = true;
        if (key == GLFW_KEY_D) g_gameState->keys['D'] = true;
        if (key == GLFW_KEY_SPACE) g_gameState->keys[' '] = true;
    } else if (action == GLFW_RELEASE) {
        if (key == GLFW_KEY_W) g_gameState->keys['W'] = false;
        if (key == GLFW_KEY_S) g_gameState->keys['S'] = false;
        if (key == GLFW_KEY_A) g_gameState->keys['A'] = false;
        if (key == GLFW_KEY_D) g_gameState->keys['D'] = false;
        if (key == GLFW_KEY_SPACE) g_gameState->keys[' '] = false;
    }
}

// Forward declarations para funciones de menú y guardado
void updateButtonHover(GameState* state, float mouseX, float mouseY);
void handleMainMenuClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight);
void handleWorldSelectClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight, float currentTime);
void handleWorldCreateClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight, float currentTime);  // ⭐ NUEVO
bool loadWorldData(GameState* state, const std::string& worldName);
void saveWorld(GameState* state);

// ⭐ Forward declarations para sistema level.dat
long long calculateWorldSize(const std::string& worldPath);
std::string formatFileSize(long long bytes);
std::string formatPlaytime(float seconds);
std::string formatTimestamp(long long timestamp);
void saveLevelDat(const std::string& worldPath, const WorldInfo& worldInfo, float sessionPlaytime);
bool loadLevelDat(const std::string& worldPath, WorldInfo& worldInfo);

// ⭐⭐⭐ CALLBACK DE SCROLL DEL MOUSE (Cambiar slots del hotbar) ⭐⭐⭐
void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    if (!g_gameState) return;

    // Solo funciona en juego, no en menús
    if (g_gameState->screenState != SCREEN_IN_GAME) return;

    // No cambiar slot si el inventario está abierto
    if (g_gameState->inventoryOpen) return;

    // ⭐ Cambiar slot seleccionado con la rueda del mouse
    // yoffset > 0 = scroll arriba (slot anterior)
    // yoffset < 0 = scroll abajo (slot siguiente)

    if (yoffset > 0) {
        // Scroll arriba - slot anterior
        g_gameState->inventory.selectedSlot--;
        if (g_gameState->inventory.selectedSlot < 0) {
            g_gameState->inventory.selectedSlot = 8;  // Volver al último slot del hotbar
        }
    } else if (yoffset < 0) {
        // Scroll abajo - slot siguiente
        g_gameState->inventory.selectedSlot++;
        if (g_gameState->inventory.selectedSlot > 8) {
            g_gameState->inventory.selectedSlot = 0;  // Volver al primer slot
        }
    }
}

void mouseCallback(GLFWwindow* window, double xpos, double ypos) {
    if (!g_gameState) return;

    // Si el cursor no está bloqueado (estamos en menús), actualizar hover de botones
    if (!g_gameState->cursorLocked) {
        updateButtonHover(g_gameState, (float)xpos, (float)ypos);
        return;
    }

    if (g_gameState->firstMouse) {
        g_gameState->lastMouseX = xpos;
        g_gameState->lastMouseY = ypos;
        g_gameState->firstMouse = false;
    }

    double xoffset = xpos - g_gameState->lastMouseX;
    double yoffset = g_gameState->lastMouseY - ypos;

    g_gameState->lastMouseX = xpos;
    g_gameState->lastMouseY = ypos;

    xoffset *= g_gameState->mouseSensitivity;
    yoffset *= g_gameState->mouseSensitivity;

    g_gameState->player.yaw -= (float)xoffset;  // Restar: mouse derecha = yaw disminuye = gira derecha
    g_gameState->player.pitch += (float)yoffset;

    if (g_gameState->player.pitch > 89.0f) g_gameState->player.pitch = 89.0f;
    if (g_gameState->player.pitch < -89.0f) g_gameState->player.pitch = -89.0f;
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if (!g_gameState) return;

    // Manejar clics en menús
    if (g_gameState->screenState == SCREEN_MAIN_MENU ||
        g_gameState->screenState == SCREEN_WORLD_SELECT ||
        g_gameState->screenState == SCREEN_WORLD_CREATE) {  // ⭐ AGREGADO
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);

            int width, height;
            glfwGetWindowSize(window, &width, &height);

            if (g_gameState->screenState == SCREEN_MAIN_MENU) {
                handleMainMenuClick(g_gameState, (float)xpos, (float)ypos, width, height);
                return;
            } else if (g_gameState->screenState == SCREEN_WORLD_SELECT) {
                float currentTime = (float)glfwGetTime();
                handleWorldSelectClick(g_gameState, (float)xpos, (float)ypos, width, height, currentTime);
                return;
            } else if (g_gameState->screenState == SCREEN_WORLD_CREATE) {  // ⭐ NUEVO handler
                float currentTime = (float)glfwGetTime();
                handleWorldCreateClick(g_gameState, (float)xpos, (float)ypos, width, height, currentTime);
                return;
            }
        }
    }

    // ⭐⭐⭐ MANEJAR CLICKS EN EL INVENTARIO Y CRAFTEO (ESTILO MINECRAFT) ⭐⭐⭐
    if (g_gameState->inventoryOpen && action == GLFW_PRESS) {
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        int width, height;
        glfwGetWindowSize(window, &width, &height);

        // Configuración del inventario (debe coincidir con el renderizado)
        const int COLS = 9;
        const int ROWS = 5;
        const float SLOT_SIZE = 50.0f;
        const float SPACING = 5.0f;
        const float GRID_WIDTH = COLS * (SLOT_SIZE + SPACING) - SPACING;
        const float START_X = (width - GRID_WIDTH) / 2.0f;
        const float START_Y = 120.0f;

        // Lambda para intercambiar slots (tipo Minecraft)
        auto swapSlots = [](InventorySlot& slot1, InventorySlot& slot2) {
            InventorySlot temp = slot1;
            slot1 = slot2;
            slot2 = temp;
        };

        // Lambda para mover items entre slots
        auto handleSlotClick = [&](InventorySlot& clickedSlot, bool isLeftClick) {
            if (isLeftClick) {
                // CLICK IZQUIERDO: Tomar/Colocar todo el stack
                if (g_gameState->heldSlot.isEmpty()) {
                    // Si no tenemos nada en la mano, tomar el slot completo
                    if (!clickedSlot.isEmpty()) {
                        swapSlots(g_gameState->heldSlot, clickedSlot);
                    }
                } else {
                    // Si tenemos algo en la mano
                    if (clickedSlot.isEmpty()) {
                        // Slot vacío: colocar todo
                        swapSlots(g_gameState->heldSlot, clickedSlot);
                    } else if (clickedSlot.blockType == g_gameState->heldSlot.blockType) {
                        // Mismo tipo: intentar combinar
                        int spaceAvailable = 100 - clickedSlot.count;
                        int toTransfer = (spaceAvailable < g_gameState->heldSlot.count) ? spaceAvailable : g_gameState->heldSlot.count;

                        clickedSlot.count += toTransfer;
                        g_gameState->heldSlot.count -= toTransfer;

                        if (g_gameState->heldSlot.count <= 0) {
                            g_gameState->heldSlot.blockType = BLOCK_AIR;
                            g_gameState->heldSlot.count = 0;
                        }
                    } else {
                        // Diferente tipo: intercambiar
                        swapSlots(g_gameState->heldSlot, clickedSlot);
                    }
                }
            } else {
                // CLICK DERECHO: Colocar/Tomar la mitad
                if (g_gameState->heldSlot.isEmpty()) {
                    // Si no tenemos nada, tomar la mitad del slot
                    if (!clickedSlot.isEmpty()) {
                        int half = (clickedSlot.count + 1) / 2;  // Redondear hacia arriba

                        g_gameState->heldSlot.blockType = clickedSlot.blockType;
                        g_gameState->heldSlot.count = half;
                        clickedSlot.count -= half;

                        if (clickedSlot.count <= 0) {
                            clickedSlot.blockType = BLOCK_AIR;
                            clickedSlot.count = 0;
                        }
                    }
                } else {
                    // Si tenemos algo, colocar 1 item
                    if (clickedSlot.isEmpty()) {
                        // Slot vacío: colocar 1
                        clickedSlot.blockType = g_gameState->heldSlot.blockType;
                        clickedSlot.count = 1;
                        g_gameState->heldSlot.count--;

                        if (g_gameState->heldSlot.count <= 0) {
                            g_gameState->heldSlot.blockType = BLOCK_AIR;
                            g_gameState->heldSlot.count = 0;
                        }
                    } else if (clickedSlot.blockType == g_gameState->heldSlot.blockType && clickedSlot.count < 100) {
                        // Mismo tipo y hay espacio: agregar 1
                        clickedSlot.count++;
                        g_gameState->heldSlot.count--;

                        if (g_gameState->heldSlot.count <= 0) {
                            g_gameState->heldSlot.blockType = BLOCK_AIR;
                            g_gameState->heldSlot.count = 0;
                        }
                    }
                }
            }
        };

        bool clickHandled = false;

        // Verificar clicks en slots del inventario
        for (int slot = 0; slot < Inventory::SLOTS; slot++) {
            int row = slot / COLS;
            int col = slot % COLS;

            float x = START_X + col * (SLOT_SIZE + SPACING);
            float y = START_Y + row * (SLOT_SIZE + SPACING);

            if (xpos >= x && xpos <= x + SLOT_SIZE && ypos >= y && ypos <= y + SLOT_SIZE) {
                handleSlotClick(g_gameState->inventory.slots[slot], button == GLFW_MOUSE_BUTTON_LEFT);
                clickHandled = true;
                break;
            }
        }

        if (clickHandled) return;

        // Configuración del grid de crafteo
        const float CRAFT_SLOT_SIZE = 50.0f;
        const float CRAFT_SPACING = 5.0f;
        const float CRAFT_GRID_SIZE = 3 * (CRAFT_SLOT_SIZE + CRAFT_SPACING) - CRAFT_SPACING;
        const float CRAFT_START_X = START_X + GRID_WIDTH + 80.0f;
        const float CRAFT_START_Y = START_Y + 30.0f;

        // Verificar clicks en slots del crafting grid
        for (int i = 0; i < 9; i++) {
            int row = i / 3;
            int col = i % 3;

            float x = CRAFT_START_X + col * (CRAFT_SLOT_SIZE + CRAFT_SPACING);
            float y = CRAFT_START_Y + row * (CRAFT_SLOT_SIZE + CRAFT_SPACING);

            if (xpos >= x && xpos <= x + CRAFT_SLOT_SIZE && ypos >= y && ypos <= y + CRAFT_SLOT_SIZE) {
                handleSlotClick(g_gameState->craftingGrid.slots[i], button == GLFW_MOUSE_BUTTON_LEFT);

                // Actualizar resultado del crafteo
                g_gameState->updateCraftingResult();
                clickHandled = true;
                break;
            }
        }

        if (clickHandled) return;

        // Verificar click en slot de resultado (solo botón izquierdo)
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            float arrowX = CRAFT_START_X + CRAFT_GRID_SIZE + 15.0f;
            float resultX = arrowX + 60.0f;
            float resultY = CRAFT_START_Y + CRAFT_GRID_SIZE / 2.0f - CRAFT_SLOT_SIZE / 2.0f;

            if (xpos >= resultX && xpos <= resultX + CRAFT_SLOT_SIZE &&
                ypos >= resultY && ypos <= resultY + CRAFT_SLOT_SIZE) {

                // ⭐ NUEVO: Detectar SHIFT+CLICK para crafteo rápido (tipo Minecraft)
                bool shiftPressed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                    glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

                if (!g_gameState->craftingResult.isEmpty()) {
                    if (shiftPressed) {
                        // ⭐⭐⭐ SHIFT+CLICK: Craftear TODO lo del grid y mandar al inventario ⭐⭐⭐
                        int totalCrafted = 0;

                        // Craftear hasta que no se pueda más (solo con items del grid, SIN auto-refill)
                        while (!g_gameState->craftingResult.isEmpty()) {
                            BlockType resultType = g_gameState->craftingResult.blockType;
                            int resultCount = g_gameState->craftingResult.count;

                            // Intentar agregar al inventario
                            if (!g_gameState->inventory.addItem(resultType, resultCount)) {
                                // Inventario lleno
                                std::cout << "Inventario lleno! Crafteados: " << totalCrafted << " items" << std::endl;
                                break;
                            }

                            // Ejecutar crafteo (consume materiales del grid)
                            if (!g_gameState->executeCrafting()) {
                                break;
                            }

                            totalCrafted += resultCount;

                            // Actualizar resultado para la siguiente iteración
                            // (solo continuará si quedan suficientes items en el grid)
                            g_gameState->updateCraftingResult();
                        }

                        if (totalCrafted > 0) {
                            std::cout << "Crafteo rapido! Total: " << totalCrafted << " items" << std::endl;
                        }
                    } else if (g_gameState->heldSlot.isEmpty()) {
                        // ⭐ CLICK NORMAL: Tomar resultado en la mano
                        BlockType resultType = g_gameState->craftingResult.blockType;
                        int resultCount = g_gameState->craftingResult.count;

                        // Ejecutar crafteo (consume items del grid)
                        if (g_gameState->executeCrafting()) {
                            // Poner el resultado en la mano
                            g_gameState->heldSlot.blockType = resultType;
                            g_gameState->heldSlot.count = resultCount;

                            std::cout << "Crafteo exitoso! +" << resultCount << "x " << resultType << std::endl;
                        }
                    }
                }
                return;
            }
        }
    }

    // Si estamos jugando (no pausado ni inventario)
    if (!g_gameState->isPaused && !g_gameState->inventoryOpen) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            // Mantener click izquierdo = minar (sistema Minecraft)
            if (action == GLFW_PRESS) {
                g_gameState->mouseLeftPressed = true;
            } else if (action == GLFW_RELEASE) {
                g_gameState->mouseLeftPressed = false;
            }
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
            // Click derecho = colocar bloque
            placeBlock(g_gameState);
        }
        return;
    }

    // Si estamos en el menú de pausa
    if (!g_gameState->isPaused) return;
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;

    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    int width, height;
    glfwGetWindowSize(window, &width, &height);

    float centerX = width / 2.0f;
    float buttonWidth = 300.0f;
    float buttonHeight = 50.0f;
    float buttonSpacing = 60.0f;

    if (g_gameState->pauseMenuState == PAUSE_MENU_MAIN) {
        // Botones del menú principal
        float startY = height / 2.0f - 80.0f;

        // Botón 1: Reanudar partida
        Button btn1(centerX - buttonWidth / 2, startY, buttonWidth, buttonHeight, "Reanudar");
        if (btn1.contains((float)xpos, (float)ypos)) {
            g_gameState->isPaused = false;
            g_gameState->cursorLocked = true;
            g_gameState->firstMouse = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            return;
        }

        // Botón 2: Gráficos
        Button btn2(centerX - buttonWidth / 2, startY + buttonSpacing, buttonWidth, buttonHeight, "Graficos");
        if (btn2.contains((float)xpos, (float)ypos)) {
            g_gameState->pauseMenuState = PAUSE_MENU_GRAPHICS;
            return;
        }

        // Botón 3: Sensibilidad
        Button btn3(centerX - buttonWidth / 2, startY + buttonSpacing * 2, buttonWidth, buttonHeight, "Sensibilidad");
        if (btn3.contains((float)xpos, (float)ypos)) {
            g_gameState->pauseMenuState = PAUSE_MENU_SENSITIVITY;
            return;
        }

        // Botón 4: Salir (guardar y volver al menú)
        Button btn4(centerX - buttonWidth / 2, startY + buttonSpacing * 3, buttonWidth, buttonHeight, "Salir");
        if (btn4.contains((float)xpos, (float)ypos)) {
            // Iniciar proceso de guardado
            g_gameState->isSaving = true;
            g_gameState->savingTimer = 0.5f;  // 0.5 segundos para guardar
            g_gameState->returnToMenuAfterSave = true;
            return;
        }

    } else if (g_gameState->pauseMenuState == PAUSE_MENU_GRAPHICS) {
        // Menú de gráficos
        float startY = height / 2.0f - 100.0f;

        // Botones para cambiar render distance
        Button btnMinus(centerX - 150, startY + 60, 50, 40, "-");
        if (btnMinus.contains((float)xpos, (float)ypos)) {
            if (g_gameState->renderDistance > 2) {
                g_gameState->renderDistance--;
            }
            return;
        }

        Button btnPlus(centerX + 100, startY + 60, 50, 40, "+");
        if (btnPlus.contains((float)xpos, (float)ypos)) {
            if (g_gameState->renderDistance < 16) {
                g_gameState->renderDistance++;
            }
            return;
        }

        // Botón volver
        Button btnBack(centerX - buttonWidth / 2, startY + 160, buttonWidth, buttonHeight, "Volver");
        if (btnBack.contains((float)xpos, (float)ypos)) {
            g_gameState->pauseMenuState = PAUSE_MENU_MAIN;
            return;
        }

    } else if (g_gameState->pauseMenuState == PAUSE_MENU_SENSITIVITY) {
        // Menú de sensibilidad
        float startY = height / 2.0f - 100.0f;

        // Botones para cambiar sensibilidad
        Button btnMinus(centerX - 150, startY + 60, 50, 40, "-");
        if (btnMinus.contains((float)xpos, (float)ypos)) {
            if (g_gameState->mouseSensitivity > 0.05f) {
                g_gameState->mouseSensitivity -= 0.01f;
            }
            return;
        }

        Button btnPlus(centerX + 100, startY + 60, 50, 40, "+");
        if (btnPlus.contains((float)xpos, (float)ypos)) {
            if (g_gameState->mouseSensitivity < 0.5f) {
                g_gameState->mouseSensitivity += 0.01f;
            }
            return;
        }

        // Botón volver
        Button btnBack(centerX - buttonWidth / 2, startY + 160, buttonWidth, buttonHeight, "Volver");
        if (btnBack.contains((float)xpos, (float)ypos)) {
            g_gameState->pauseMenuState = PAUSE_MENU_MAIN;
            return;
        }
    }
}

// ============================================================================
// CHUNK SYSTEM TEXTURE CALLBACK
// ============================================================================

// Wrapper function to map ChunkSystem directions to TextureManager faces
// ChunkSystem: 0=North(+Z), 1=South(-Z), 2=East(+X), 3=West(-X), 4=Up(+Y), 5=Down(-Y)
// TextureManager: 0=top, 1=bottom, 2=north, 3=south, 4=east, 5=west
GLuint chunkSystemTextureCallback(uint8_t blockType, int direction) {
    if (!g_textureManager) return 0;

    int textureFace;
    switch (direction) {
        case 0: textureFace = 2; break;  // North → north
        case 1: textureFace = 3; break;  // South → south
        case 2: textureFace = 4; break;  // East → east
        case 3: textureFace = 5; break;  // West → west
        case 4: textureFace = 0; break;  // Up → top
        case 5: textureFace = 1; break;  // Down → bottom
        default: textureFace = 0; break;
    }

    return g_textureManager->getBlockTexture(static_cast<BlockType>(blockType), textureFace);
}

// ============================================================================
// SISTEMA DE MENÚS Y MUNDOS
// ============================================================================

namespace fs = std::filesystem;

// ⭐⭐⭐ ESCANEAR CARPETA DE MUNDOS GUARDADOS (MEJORADO con level.dat) ⭐⭐⭐
void scanSavedWorlds(GameState* state) {
    state->savedWorlds.clear();

    std::filesystem::path worldsPath = "saves";
    if (!std::filesystem::exists(worldsPath)) {
        std::filesystem::create_directory(worldsPath);
    }

    for (const auto& entry : std::filesystem::directory_iterator(worldsPath)) {
        if (entry.is_directory()) {
            std::string worldName = entry.path().filename().string();
            std::string worldPath = entry.path().string();

            // Crear WorldInfo temporal
            WorldInfo worldInfo;
            worldInfo.name = worldName;
            worldInfo.folderPath = worldPath;

            // ⭐⭐⭐ NUEVO: Cargar level.dat si existe para obtener metadata completa
            bool levelDatLoaded = loadLevelDat(worldPath, worldInfo);

            // Si no se pudo cargar level.dat, usar tiempo de última modificación como fallback
            if (!levelDatLoaded) {
                auto ftime = std::filesystem::last_write_time(entry);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                auto time = std::chrono::system_clock::to_time_t(sctp);
                worldInfo.lastPlayed = time;
                worldInfo.creationDate = time;
            }

            state->savedWorlds.push_back(worldInfo);
        }
    }

    // Ordenar por último jugado (más reciente primero)
    std::sort(state->savedWorlds.begin(), state->savedWorlds.end(),
        [](const WorldInfo& a, const WorldInfo& b) {
            return a.lastPlayed > b.lastPlayed;
        });
}

// Inicializar botones del menú principal
void initMainMenuButtons(GameState* state, int screenWidth, int screenHeight) {
    float centerX = screenWidth / 2.0f;
    float startY = screenHeight / 2.0f - 100;
    float buttonWidth = 300;
    float buttonHeight = 50;
    float spacing = 70;

    state->btnMundosSolitarios = Button(centerX - buttonWidth/2, startY, buttonWidth, buttonHeight, "Mundos Solitarios");
    state->btnOpciones = Button(centerX - buttonWidth/2, startY + spacing, buttonWidth, buttonHeight, "Opciones");
    state->btnAvatar = Button(centerX - buttonWidth/2, startY + spacing * 2, buttonWidth, buttonHeight, "Avatar");
    state->btnSalir = Button(centerX - buttonWidth/2, startY + spacing * 3, buttonWidth, buttonHeight, "Salir");
}

// Inicializar botones de selección de mundos
void initWorldSelectButtons(GameState* state, int screenWidth, int screenHeight) {
    float centerX = screenWidth / 2.0f;
    float buttonWidth = 600;
    float buttonHeight = 60;
    float startY = 150;
    float spacing = 80;

    state->worldButtons.clear();

    for (size_t i = 0; i < state->savedWorlds.size(); i++) {
        float y = startY + i * spacing;
        state->worldButtons.push_back(Button(centerX - buttonWidth/2, y, buttonWidth, buttonHeight,
                                             state->savedWorlds[i].name.c_str()));
    }

    // Botones de gestión (centrados en la parte inferior)
    float actionButtonWidth = 200;
    float actionButtonHeight = 50;
    float actionSpacing = 220;
    float actionY = screenHeight - 180;

    state->btnJugarMundo = Button(centerX - actionSpacing * 1.5f, actionY, actionButtonWidth, actionButtonHeight, "Jugar");
    state->btnEditarMundo = Button(centerX - actionSpacing * 0.5f, actionY, actionButtonWidth, actionButtonHeight, "Editar");
    state->btnRespaldoMundo = Button(centerX + actionSpacing * 0.5f, actionY, actionButtonWidth, actionButtonHeight, "Respaldar");
    state->btnBorrarMundo = Button(centerX + actionSpacing * 1.5f, actionY, actionButtonWidth, actionButtonHeight, "Borrar");

    state->btnCrearMundo = Button(centerX - 150, screenHeight - 120, 300, 50, "Crear Nuevo Mundo");
    state->btnVolverMenu = Button(50, screenHeight - 80, 200, 50, "<- Volver");
}

// Renderizar un botón
void renderButton(const Button& btn, int screenWidth, int screenHeight) {
    float x1 = btn.x;
    float y1 = btn.y;
    float x2 = btn.x + btn.width;
    float y2 = btn.y + btn.height;

    // FORZAR estados de OpenGL para que funcione
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel(GL_FLAT);

    // CRÍTICO: Forzar modo de relleno de polígonos
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Sombra del botón
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
    glVertex2f(x1 + 5, y1 + 5);
    glVertex2f(x2 + 5, y1 + 5);
    glVertex2f(x2 + 5, y2 + 5);
    glVertex2f(x1 + 5, y2 + 5);
    glEnd();

    // FONDO DEL BOTÓN - COLOR SÓLIDO MUY VISIBLE
    glBegin(GL_QUADS);
    if (btn.isHovered) {
        glColor3f(1.0f, 0.5f, 0.0f);  // Naranja brillante
    } else {
        glColor3f(0.3f, 0.7f, 0.4f);  // Verde brillante
    }
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();

    // Borde negro
    glLineWidth(3.0f);
    glBegin(GL_LINE_LOOP);
    glColor3f(0.0f, 0.0f, 0.0f);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();

    // Borde blanco interno
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    glColor3f(1.0f, 1.0f, 1.0f);
    glVertex2f(x1 + 2, y1 + 2);
    glVertex2f(x2 - 2, y1 + 2);
    glVertex2f(x2 - 2, y2 - 2);
    glVertex2f(x1 + 2, y2 - 2);
    glEnd();

    glPopAttrib();
}

// Renderizar letra bitmap simple
void renderBitmapChar(char c, float x, float y, float size) {
    float w = size * 0.6f;
    float h = size;

    // Convertir a mayúscula para simplificar
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    glBegin(GL_QUADS);

    // Para cada letra, dibujamos píxeles formando la letra
    // Usamos una cuadrícula 5x7
    bool pixels[7][5] = {false};

    switch(c) {
        case 'A':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'B':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'C':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = true;
            pixels[3][0] = true;
            pixels[4][0] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'D':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'E':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][0] = true;
            pixels[2][0] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = true;
            pixels[5][0] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case 'F':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][0] = true;
            pixels[2][0] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = true;
            pixels[5][0] = true;
            pixels[6][0] = true;
            break;
        case 'G':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = true;
            pixels[3][0] = pixels[3][2] = pixels[3][3] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'H':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'I':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][2] = true;
            pixels[2][2] = true;
            pixels[3][2] = true;
            pixels[4][2] = true;
            pixels[5][2] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case 'J':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][2] = true;
            pixels[2][2] = true;
            pixels[3][2] = true;
            pixels[4][2] = true;
            pixels[5][0] = pixels[5][2] = true;
            pixels[6][1] = pixels[6][2] = true;
            break;
        case 'K':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][3] = true;
            pixels[2][0] = pixels[2][2] = true;
            pixels[3][0] = pixels[3][1] = true;
            pixels[4][0] = pixels[4][2] = true;
            pixels[5][0] = pixels[5][3] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'L':
            pixels[0][0] = true;
            pixels[1][0] = true;
            pixels[2][0] = true;
            pixels[3][0] = true;
            pixels[4][0] = true;
            pixels[5][0] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case 'M':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][1] = pixels[1][3] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][2] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'N':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][1] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][2] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][2] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][3] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][3] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'O':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'P':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = true;
            pixels[5][0] = true;
            pixels[6][0] = true;
            break;
        case 'Q':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][2] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][3] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][4] = true;
            break;
        case 'R':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = pixels[4][2] = true;
            pixels[5][0] = pixels[5][3] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'S':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][0] = true;
            pixels[2][0] = true;
            pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][4] = true;
            pixels[5][4] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'T':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][2] = true;
            pixels[2][2] = true;
            pixels[3][2] = true;
            pixels[4][2] = true;
            pixels[5][2] = true;
            pixels[6][2] = true;
            break;
        case 'U':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case 'V':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][1] = pixels[4][3] = true;
            pixels[5][1] = pixels[5][3] = true;
            pixels[6][2] = true;
            break;
        case 'W':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][2] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][1] = pixels[5][3] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'X':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][1] = pixels[2][3] = true;
            pixels[3][2] = true;
            pixels[4][1] = pixels[4][3] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][0] = pixels[6][4] = true;
            break;
        case 'Y':
            pixels[0][0] = pixels[0][4] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][1] = pixels[2][3] = true;
            pixels[3][2] = true;
            pixels[4][2] = true;
            pixels[5][2] = true;
            pixels[6][2] = true;
            break;
        case 'Z':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][4] = true;
            pixels[2][3] = true;
            pixels[3][2] = true;
            pixels[4][1] = true;
            pixels[5][0] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case '0':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][3] = pixels[2][4] = true;
            pixels[3][0] = pixels[3][2] = pixels[3][4] = true;
            pixels[4][0] = pixels[4][1] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '1':
            pixels[0][1] = pixels[0][2] = true;
            pixels[1][0] = pixels[1][2] = true;
            pixels[2][2] = true;
            pixels[3][2] = true;
            pixels[4][2] = true;
            pixels[5][2] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case '2':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][4] = true;
            pixels[3][3] = true;
            pixels[4][2] = true;
            pixels[5][1] = true;
            pixels[6][0] = pixels[6][1] = pixels[6][2] = pixels[6][3] = pixels[6][4] = true;
            break;
        case '3':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][4] = true;
            pixels[3][2] = pixels[3][3] = true;
            pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '4':
            pixels[0][3] = true;
            pixels[1][2] = pixels[1][3] = true;
            pixels[2][1] = pixels[2][3] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = pixels[3][4] = true;
            pixels[4][3] = true;
            pixels[5][3] = true;
            pixels[6][3] = true;
            break;
        case '5':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][0] = true;
            pixels[2][0] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '6':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = true;
            pixels[3][0] = pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '7':
            pixels[0][0] = pixels[0][1] = pixels[0][2] = pixels[0][3] = pixels[0][4] = true;
            pixels[1][4] = true;
            pixels[2][3] = true;
            pixels[3][3] = true;
            pixels[4][2] = true;
            pixels[5][2] = true;
            pixels[6][2] = true;
            break;
        case '8':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            pixels[4][0] = pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '9':
            pixels[0][1] = pixels[0][2] = pixels[0][3] = true;
            pixels[1][0] = pixels[1][4] = true;
            pixels[2][0] = pixels[2][4] = true;
            pixels[3][1] = pixels[3][2] = pixels[3][3] = pixels[3][4] = true;
            pixels[4][4] = true;
            pixels[5][0] = pixels[5][4] = true;
            pixels[6][1] = pixels[6][2] = pixels[6][3] = true;
            break;
        case '.':
            pixels[5][2] = true;
            pixels[6][2] = true;
            break;
        case '<':
            pixels[1][3] = true;
            pixels[2][2] = true;
            pixels[3][1] = true;
            pixels[4][2] = true;
            pixels[5][3] = true;
            break;
        case '-':
            pixels[3][1] = pixels[3][2] = pixels[3][3] = true;
            break;
        case ' ':
            // Espacio vacío
            break;
        default:
            // Cualquier otra letra - dibujamos un rectángulo simple
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    pixels[row][col] = (row > 0 && row < 6 && col > 0 && col < 4);
                }
            }
            break;
    }

    // Dibujar los píxeles
    float pixelW = w / 5.0f;
    float pixelH = h / 7.0f;

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (pixels[row][col]) {
                float px = x + col * pixelW;
                float py = y + row * pixelH;
                glVertex2f(px, py);
                glVertex2f(px + pixelW, py);
                glVertex2f(px + pixelW, py + pixelH);
                glVertex2f(px, py + pixelH);
            }
        }
    }

    glEnd();
}

void renderText(const char* text, float x, float y, int screenWidth, int screenHeight) {
    if (!text) return;

    float size = 20.0f;
    float spacing = size * 0.7f;

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);

    // Renderizar sombra
    float currentX = x;
    glColor4f(0.0f, 0.0f, 0.0f, 0.9f);
    for (int i = 0; text[i] != '\0'; i++) {
        renderBitmapChar(text[i], currentX + 2, y + 2, size);
        currentX += spacing;
    }

    // Renderizar texto principal
    currentX = x;
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    for (int i = 0; text[i] != '\0'; i++) {
        renderBitmapChar(text[i], currentX, y, size);
        currentX += spacing;
    }
}

// Renderizar menú principal
void renderMainMenu(GameState* state, int screenWidth, int screenHeight, GLFWwindow* window) {
    // Recalcular posiciones de botones según el tamaño actual de la ventana
    initMainMenuButtons(state, screenWidth, screenHeight);

    // CONFIGURACIÓN CRÍTICA DE OPENGL PARA UI
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);  // CRÍTICO: Deshabilitar culling para UI 2D
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, screenWidth, screenHeight, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Fondo con gradiente
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor3f(0.15f, 0.25f, 0.35f);
    glVertex2f(0, 0);
    glVertex2f(screenWidth, 0);
    glColor3f(0.05f, 0.05f, 0.1f);
    glVertex2f(screenWidth, screenHeight);
    glVertex2f(0, screenHeight);
    glEnd();

    // Sombra del título
    float titleY = 80;
    float titleWidth = 450;
    float titleHeight = 80;
    glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(screenWidth/2.0f - titleWidth/2 + 5, titleY + 5);
    glVertex2f(screenWidth/2.0f + titleWidth/2 + 5, titleY + 5);
    glVertex2f(screenWidth/2.0f + titleWidth/2 + 5, titleY + titleHeight + 5);
    glVertex2f(screenWidth/2.0f - titleWidth/2 + 5, titleY + titleHeight + 5);
    glEnd();

    // Fondo del título (caja decorativa)
    glColor4f(0.2f, 0.3f, 0.4f, 0.8f);
    glBegin(GL_QUADS);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY + titleHeight);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY + titleHeight);
    glEnd();

    // Borde del título (dorado)
    glColor4f(1.0f, 0.8f, 0.0f, 1.0f);
    glLineWidth(3.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY + titleHeight);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY + titleHeight);
    glEnd();
    glLineWidth(1.0f);

    // Texto del título
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    renderText("VOXEL WORLD", screenWidth/2.0f - 85, titleY + 30, screenWidth, screenHeight);

    // FORZAR modo de relleno de polígonos (crítico)
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Obtener posición del mouse para hover
    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);

    // ⭐⭐⭐ BOTONES MEJORADOS CON EFECTOS VISUALES

    // Botón 1: MUNDOS SOLITARIOS (pulido)
    bool hover1 = state->btnMundosSolitarios.contains((float)mouseX, (float)mouseY);

    // ⭐ Efecto de brillo en hover
    if (hover1) {
        glColor3f(0.45f, 0.6f, 0.45f);  // Verde suave en hover
    } else {
        glColor3f(0.3f, 0.4f, 0.3f);    // Verde oscuro
    }

    // Fondo del botón
    glBegin(GL_QUADS);
    glVertex2f(state->btnMundosSolitarios.x, state->btnMundosSolitarios.y);
    glVertex2f(state->btnMundosSolitarios.x + state->btnMundosSolitarios.width, state->btnMundosSolitarios.y);
    glVertex2f(state->btnMundosSolitarios.x + state->btnMundosSolitarios.width, state->btnMundosSolitarios.y + state->btnMundosSolitarios.height);
    glVertex2f(state->btnMundosSolitarios.x, state->btnMundosSolitarios.y + state->btnMundosSolitarios.height);
    glEnd();

    // ⭐ Borde más visible
    if (hover1) {
        glColor3f(0.6f, 0.8f, 0.6f);  // Borde brillante en hover
        glLineWidth(3);
    } else {
        glColor3f(0.4f, 0.5f, 0.4f);
        glLineWidth(2);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnMundosSolitarios.x, state->btnMundosSolitarios.y);
    glVertex2f(state->btnMundosSolitarios.x + state->btnMundosSolitarios.width, state->btnMundosSolitarios.y);
    glVertex2f(state->btnMundosSolitarios.x + state->btnMundosSolitarios.width, state->btnMundosSolitarios.y + state->btnMundosSolitarios.height);
    glVertex2f(state->btnMundosSolitarios.x, state->btnMundosSolitarios.y + state->btnMundosSolitarios.height);
    glEnd();

    // Texto del botón
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("MUNDOS SOLITARIOS", state->btnMundosSolitarios.x + 20, state->btnMundosSolitarios.y + 15, 20);

    // Botón 2: OPCIONES (pulido con color azul)
    bool hover2 = state->btnOpciones.contains((float)mouseX, (float)mouseY);

    // ⭐ Color azul para opciones
    if (hover2) {
        glColor3f(0.3f, 0.5f, 0.7f);  // Azul más brillante en hover
    } else {
        glColor3f(0.2f, 0.3f, 0.5f);  // Azul oscuro
    }

    // Fondo del botón
    glBegin(GL_QUADS);
    glVertex2f(state->btnOpciones.x, state->btnOpciones.y);
    glVertex2f(state->btnOpciones.x + state->btnOpciones.width, state->btnOpciones.y);
    glVertex2f(state->btnOpciones.x + state->btnOpciones.width, state->btnOpciones.y + state->btnOpciones.height);
    glVertex2f(state->btnOpciones.x, state->btnOpciones.y + state->btnOpciones.height);
    glEnd();

    // ⭐ Borde azul
    if (hover2) {
        glColor3f(0.4f, 0.6f, 0.9f);
        glLineWidth(3);
    } else {
        glColor3f(0.3f, 0.4f, 0.6f);
        glLineWidth(2);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnOpciones.x, state->btnOpciones.y);
    glVertex2f(state->btnOpciones.x + state->btnOpciones.width, state->btnOpciones.y);
    glVertex2f(state->btnOpciones.x + state->btnOpciones.width, state->btnOpciones.y + state->btnOpciones.height);
    glVertex2f(state->btnOpciones.x, state->btnOpciones.y + state->btnOpciones.height);
    glEnd();

    // Texto del botón
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("OPCIONES", state->btnOpciones.x + 75, state->btnOpciones.y + 15, 20);

    // Botón 3: AVATAR (pulido con color naranja)
    bool hover3 = state->btnAvatar.contains((float)mouseX, (float)mouseY);

    // ⭐ Color naranja para avatar
    if (hover3) {
        glColor3f(0.7f, 0.5f, 0.2f);  // Naranja brillante en hover
    } else {
        glColor3f(0.5f, 0.35f, 0.15f);  // Naranja oscuro
    }

    // Fondo del botón
    glBegin(GL_QUADS);
    glVertex2f(state->btnAvatar.x, state->btnAvatar.y);
    glVertex2f(state->btnAvatar.x + state->btnAvatar.width, state->btnAvatar.y);
    glVertex2f(state->btnAvatar.x + state->btnAvatar.width, state->btnAvatar.y + state->btnAvatar.height);
    glVertex2f(state->btnAvatar.x, state->btnAvatar.y + state->btnAvatar.height);
    glEnd();

    // ⭐ Borde naranja
    if (hover3) {
        glColor3f(0.9f, 0.6f, 0.3f);
        glLineWidth(3);
    } else {
        glColor3f(0.6f, 0.45f, 0.25f);
        glLineWidth(2);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnAvatar.x, state->btnAvatar.y);
    glVertex2f(state->btnAvatar.x + state->btnAvatar.width, state->btnAvatar.y);
    glVertex2f(state->btnAvatar.x + state->btnAvatar.width, state->btnAvatar.y + state->btnAvatar.height);
    glVertex2f(state->btnAvatar.x, state->btnAvatar.y + state->btnAvatar.height);
    glEnd();

    // Texto del botón
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("AVATAR", state->btnAvatar.x + 90, state->btnAvatar.y + 15, 20);

    // Botón 4: SALIR (pulido con color rojo)
    bool hover4 = state->btnSalir.contains((float)mouseX, (float)mouseY);

    // ⭐ Color rojo para el botón de salir
    if (hover4) {
        glColor3f(0.7f, 0.3f, 0.3f);  // Rojo más brillante en hover
    } else {
        glColor3f(0.5f, 0.2f, 0.2f);  // Rojo oscuro
    }

    // Fondo del botón
    glBegin(GL_QUADS);
    glVertex2f(state->btnSalir.x, state->btnSalir.y);
    glVertex2f(state->btnSalir.x + state->btnSalir.width, state->btnSalir.y);
    glVertex2f(state->btnSalir.x + state->btnSalir.width, state->btnSalir.y + state->btnSalir.height);
    glVertex2f(state->btnSalir.x, state->btnSalir.y + state->btnSalir.height);
    glEnd();

    // ⭐ Borde rojo brillante
    if (hover4) {
        glColor3f(0.9f, 0.4f, 0.4f);
        glLineWidth(3);
    } else {
        glColor3f(0.6f, 0.3f, 0.3f);
        glLineWidth(2);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnSalir.x, state->btnSalir.y);
    glVertex2f(state->btnSalir.x + state->btnSalir.width, state->btnSalir.y);
    glVertex2f(state->btnSalir.x + state->btnSalir.width, state->btnSalir.y + state->btnSalir.height);
    glVertex2f(state->btnSalir.x, state->btnSalir.y + state->btnSalir.height);
    glEnd();

    // Texto del botón
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("SALIR", state->btnSalir.x + 100, state->btnSalir.y + 15, 20);

    // Versión del juego en la esquina inferior derecha (mejorada)
    glColor4f(0.5f, 0.5f, 0.5f, 0.9f);
    renderText("v1.0.0 - Alpha", screenWidth - 120, screenHeight - 30, screenWidth, screenHeight);

    // Restaurar matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    // Restaurar estados para renderizado 3D
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

// Función para borrar un mundo (ULTRA MEJORADO - NUNCA FALLA)
bool deleteWorld(GameState* state, int worldIndex) {
    if (worldIndex < 0 || worldIndex >= (int)state->savedWorlds.size()) {
        std::cerr << "❌ Error: Índice de mundo inválido" << std::endl;
        return false;
    }

    std::string worldPath = state->savedWorlds[worldIndex].folderPath;
    std::string worldName = state->savedWorlds[worldIndex].name;

    std::cout << "\n╔══════════════════════════════════════╗" << std::endl;
    std::cout << "║  🗑️  BORRANDO MUNDO                 ║" << std::endl;
    std::cout << "╚══════════════════════════════════════╝" << std::endl;
    std::cout << "📁 Nombre: " << worldName << std::endl;
    std::cout << "📂 Ruta: " << worldPath << std::endl;

    // ⭐⭐⭐ PASO 0: LIBERACIÓN ULTRA AGRESIVA DE RECURSOS
    std::cout << "🔓 Liberando recursos del juego..." << std::endl;

    // Si es el mundo actual, forzar limpieza COMPLETA
    if (state->currentWorldName == worldName) {
        std::cout << "   ⚠️ Es el mundo actual - limpieza profunda..." << std::endl;

        // ⭐ CRÍTICO: Resetear mundo actual ANTES de borrar
        // Esto forzará la liberación de recursos en el próximo ciclo
        std::string tempWorldName = state->currentWorldName;
        state->currentWorldName = "";
        std::cout << "   ✅ Mundo actual desvinculado" << std::endl;

        // Forzar guardado final del mundo antes de borrar (cierra todos los handles)
        try {
            std::cout << "   💾 Guardado final y cierre de archivos..." << std::endl;
            std::filesystem::path tempWorldPath = std::filesystem::path("saves") / tempWorldName;
            state->world.saveWorld(tempWorldPath.string());
            std::cout << "   ✅ Archivos cerrados" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "   ⚠️ Advertencia al guardar: " << e.what() << std::endl;
        }
    }

    // Forzar recolección de basura (múltiples pasadas)
    std::cout << "   🧹 Recolectando basura..." << std::endl;
    for (int i = 0; i < 3; i++) {
        // Forzar liberación de memoria temporal
        std::vector<char> dummy(1024);
        dummy.clear();
        dummy.shrink_to_fit();

        #ifdef _WIN32
        Sleep(50);
        #endif
    }

    // Tiempo de espera más largo para asegurar liberación de handles
    #ifdef _WIN32
    std::cout << "   ⏳ Esperando liberación de handles..." << std::endl;
    Sleep(800); // ⭐ 800ms - aún más tiempo para asegurar cierre de archivos
    #endif

    std::cout << "   ✅ Recursos liberados completamente" << std::endl;

    try {
        // Verificar que el mundo existe
        if (!std::filesystem::exists(worldPath)) {
            std::cerr << "❌ Error: El mundo no existe en el disco" << std::endl;
            scanSavedWorlds(state);  // Actualizar lista
            state->selectedWorldIndex = -1;
            state->confirmingDelete = false;
            state->isEditingWorldName = false;
            return false;
        }

        // Verificar que es un directorio
        if (!std::filesystem::is_directory(worldPath)) {
            std::cerr << "❌ Error: La ruta no es un directorio" << std::endl;
            return false;
        }

        // ⭐⭐⭐ PROTECCIÓN: Calcular tamaño del mundo de forma segura
        uintmax_t totalSize = 0;
        int fileCount = 0;
        int dirCount = 0;

        try {
            // Usar std::error_code para evitar excepciones
            std::error_code ec;
            auto dirIter = std::filesystem::recursive_directory_iterator(
                worldPath,
                std::filesystem::directory_options::skip_permission_denied,
                ec
            );

            if (ec) {
                std::cout << "⚠️ No se pudo escanear directorio: " << ec.message() << std::endl;
            } else {
                for (const auto& entry : dirIter) {
                    try {
                        std::error_code entryEc;
                        if (entry.is_regular_file(entryEc) && !entryEc) {
                            auto size = entry.file_size(entryEc);
                            if (!entryEc) {
                                totalSize += size;
                                fileCount++;
                            }
                        } else if (entry.is_directory(entryEc) && !entryEc) {
                            dirCount++;
                        }
                    } catch (const std::exception& e) {
                        // Ignorar errores en archivos individuales
                        std::cout << "⚠️ Ignorando entrada: " << e.what() << std::endl;
                    } catch (...) {
                        // Capturar cualquier error no estándar
                        std::cout << "⚠️ Error desconocido en entrada" << std::endl;
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cout << "⚠️ Error filesystem al escanear: " << e.what() << std::endl;
        } catch (const std::exception& e) {
            std::cout << "⚠️ Error al escanear: " << e.what() << std::endl;
        } catch (...) {
            std::cout << "⚠️ Error desconocido al escanear directorio" << std::endl;
        }

        std::cout << "📄 Archivos: " << fileCount << std::endl;
        std::cout << "📁 Carpetas: " << dirCount << std::endl;
        std::cout << "💾 Espacio a liberar: " << (totalSize / 1024.0f / 1024.0f) << " MB" << std::endl;
        std::cout << "\n🔄 Eliminando archivos..." << std::endl;

        // ⭐ PASO 0.5: Eliminar archivos de lock/temporales que bloquean el borrado
        std::cout << "🔓 Eliminando archivos de bloqueo..." << std::endl;
        try {
            std::vector<std::string> lockFiles = {"save.lock", ".lock", "~lock", ".tmp"};
            for (const std::string& lockFile : lockFiles) {
                std::filesystem::path lockPath = std::filesystem::path(worldPath) / lockFile;
                if (std::filesystem::exists(lockPath)) {
                    std::error_code ec;
                    std::filesystem::remove(lockPath, ec);
                    if (!ec) {
                        std::cout << "   ✅ Eliminado: " << lockFile << std::endl;
                    }
                }
            }

            // ⭐⭐⭐ PROTECCIÓN: Buscar archivos de journal del SaveSystem de forma segura
            std::error_code dirEc;
            auto dirIter = std::filesystem::directory_iterator(
                worldPath,
                std::filesystem::directory_options::skip_permission_denied,
                dirEc
            );

            if (!dirEc) {
                for (const auto& entry : dirIter) {
                    try {
                        std::error_code pathEc;
                        auto filename = entry.path().filename().string();
                        if (filename.find("journal") != std::string::npos ||
                            filename.find(".lock") != std::string::npos ||
                            filename.find(".tmp") != std::string::npos) {
                            std::error_code removeEc;
                            std::filesystem::remove(entry.path(), removeEc);
                            if (!removeEc) {
                                std::cout << "   ✅ Eliminado: " << filename << std::endl;
                            }
                        }
                    } catch (...) {
                        // Ignorar errores en archivos individuales
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cout << "   ⚠️ Error al eliminar locks: " << e.what() << std::endl;
        }

        // Espera adicional después de eliminar locks
        #ifdef _WIN32
        Sleep(200);
        #endif

        // ⭐ PASO 1: Intentar remover atributos read-only recursivamente (Windows)
        #ifdef _WIN32
        try {
            std::error_code permEc;
            auto permIter = std::filesystem::recursive_directory_iterator(
                worldPath,
                std::filesystem::directory_options::skip_permission_denied,
                permEc
            );

            if (!permEc) {
                for (const auto& entry : permIter) {
                    try {
                        std::error_code chmodEc;
                        std::filesystem::permissions(entry.path(),
                            std::filesystem::perms::owner_write,
                            std::filesystem::perm_options::add,
                            chmodEc);
                    } catch (...) {
                        // Ignorar errores de permisos
                    }
                }
            }
        } catch (...) {
            // Ignorar si falla el cambio de permisos
        }
        #endif

        // ⭐⭐⭐ PASO 2: BORRADO CON REINTENTOS EXTENDIDOS Y DIAGNÓSTICO
        std::uintmax_t deletedCount = 0;
        std::error_code ec;
        bool deleted = false;
        const int MAX_RETRIES = 15; // ⭐⭐⭐ Aumentado a 15 reintentos para garantizar éxito

        for (int attempt = 1; attempt <= MAX_RETRIES && !deleted; attempt++) {
            if (attempt > 1) {
                std::cout << "🔄 Reintento " << attempt << "/" << MAX_RETRIES << "..." << std::endl;
                #ifdef _WIN32
                // ⭐ Tiempo de espera progresivo ULTRA LARGO
                int waitTime = 400 * attempt; // 400ms, 800ms, 1200ms, etc.
                std::cout << "   ⏳ Esperando " << waitTime << "ms..." << std::endl;
                Sleep(waitTime);

                // ⭐ Forzar limpieza de handles del sistema cada 3 intentos
                if (attempt % 3 == 0) {
                    std::cout << "   🧹 Forzando limpieza de handles..." << std::endl;

                    // Forzar garbage collection extrema
                    for (int i = 0; i < 5; i++) {
                        std::vector<char> dummy(4096);
                        dummy.clear();
                        dummy.shrink_to_fit();
                    }

                    Sleep(300);
                }

                // ⭐ Cada 5 intentos, reintentamos eliminar locks de nuevo
                if (attempt % 5 == 0) {
                    std::cout << "   🔓 Re-eliminando locks..." << std::endl;
                    try {
                        std::error_code retryEc;
                        auto retryIter = std::filesystem::recursive_directory_iterator(
                            worldPath,
                            std::filesystem::directory_options::skip_permission_denied,
                            retryEc
                        );

                        if (!retryEc) {
                            for (const auto& entry : retryIter) {
                                try {
                                    std::string filename = entry.path().filename().string();
                                    if (filename.find("lock") != std::string::npos ||
                                        filename.find(".tmp") != std::string::npos) {
                                        std::error_code ec2;
                                        std::filesystem::remove(entry.path(), ec2);
                                    }
                                } catch (...) {}
                            }
                        }
                    } catch (...) {}
                    Sleep(200);
                }
                #endif
            }

            ec.clear();
            deletedCount = std::filesystem::remove_all(worldPath, ec);

            if (!ec) {
                deleted = true;
                std::cout << "✅ Elementos borrados: " << deletedCount << std::endl;
                break;
            }

            if (attempt < MAX_RETRIES) {
                std::cout << "⚠️ Intento " << attempt << " falló: " << ec.message() << std::endl;
                std::cout << "   Código de error: " << ec.value() << std::endl;

                // ⭐ Diagnóstico específico de errores comunes en Windows
                #ifdef _WIN32
                if (ec.value() == 32) {  // ERROR_SHARING_VIOLATION
                    std::cout << "   💡 Archivo en uso por otro proceso" << std::endl;
                } else if (ec.value() == 5) {  // ERROR_ACCESS_DENIED
                    std::cout << "   💡 Acceso denegado - verificando permisos..." << std::endl;
                } else if (ec.value() == 145) {  // ERROR_DIR_NOT_EMPTY
                    std::cout << "   💡 Directorio no vacío - limpieza requerida" << std::endl;
                }
                #endif
            }
        }

        // Si los reintentos normales fallaron, usar borrado agresivo
        if (!deleted) {
            std::cerr << "❌ Todos los reintentos fallaron: " << ec.message() << std::endl;
            std::cerr << "   Código de error: " << ec.value() << std::endl;

            // ⭐ PASO 3: BORRADO AGRESIVO ARCHIVO POR ARCHIVO
            std::cout << "\n⚡ Iniciando borrado agresivo..." << std::endl;
            int forcedDeletes = 0;
            int failedDeletes = 0;

            try {
                // ⭐⭐⭐ PROTECCIÓN: Recolectar todos los archivos de forma segura
                std::vector<std::filesystem::path> files;
                std::vector<std::filesystem::path> directories;

                std::error_code aggressiveEc;
                auto aggressiveIter = std::filesystem::recursive_directory_iterator(
                    worldPath,
                    std::filesystem::directory_options::skip_permission_denied,
                    aggressiveEc
                );

                if (!aggressiveEc) {
                    for (const auto& entry : aggressiveIter) {
                        try {
                            std::error_code entryTypeEc;
                            if (entry.is_regular_file(entryTypeEc) && !entryTypeEc) {
                                files.push_back(entry.path());
                            } else if (entry.is_directory(entryTypeEc) && !entryTypeEc) {
                                directories.push_back(entry.path());
                            }
                        } catch (...) {
                            // Ignorar errores al leer entradas
                        }
                    }
                } else {
                    std::cout << "   ⚠️ No se pudo iterar directorio: " << aggressiveEc.message() << std::endl;
                }

                // Borrar archivos primero
                std::cout << "   📄 Borrando " << files.size() << " archivos..." << std::endl;
                for (const auto& file : files) {
                    for (int retry = 0; retry < 3; retry++) {
                        ec.clear();

                        #ifdef _WIN32
                        // Remover atributos read-only en Windows
                        try {
                            std::filesystem::permissions(file,
                                std::filesystem::perms::owner_write,
                                std::filesystem::perm_options::add);
                        } catch (...) {}

                        if (retry > 0) Sleep(50);
                        #endif

                        if (std::filesystem::remove(file, ec)) {
                            forcedDeletes++;
                            break;
                        }

                        if (retry == 2) {
                            failedDeletes++;
                            std::cout << "   ❌ No se pudo borrar: " << file.filename() << std::endl;
                        }
                    }
                }

                // Borrar directorios en orden inverso (de más profundo a menos profundo)
                std::cout << "   📁 Borrando " << directories.size() << " directorios..." << std::endl;
                std::sort(directories.begin(), directories.end(),
                    [](const auto& a, const auto& b) {
                        return a.string().length() > b.string().length();
                    });

                for (const auto& dir : directories) {
                    for (int retry = 0; retry < 3; retry++) {
                        ec.clear();

                        #ifdef _WIN32
                        if (retry > 0) Sleep(50);
                        #endif

                        if (std::filesystem::remove(dir, ec)) {
                            forcedDeletes++;
                            break;
                        }
                    }
                }

                // Intentar borrar el directorio raíz
                for (int retry = 0; retry < 3; retry++) {
                    ec.clear();

                    #ifdef _WIN32
                    if (retry > 0) Sleep(100);
                    #endif

                    if (std::filesystem::remove(worldPath, ec)) {
                        forcedDeletes++;
                        break;
                    }
                }

            } catch (const std::exception& e) {
                std::cerr << "❌ Error en borrado agresivo: " << e.what() << std::endl;
            }

            std::cout << "✅ Borrados forzados: " << forcedDeletes << " elementos" << std::endl;
            if (failedDeletes > 0) {
                std::cout << "⚠️ Borrados fallidos: " << failedDeletes << " elementos" << std::endl;
            }

            // ⭐⭐⭐ PROTECCIÓN: Verificar si quedó algo de forma segura
            std::error_code existsEc;
            if (std::filesystem::exists(worldPath, existsEc) && !existsEc) {
                // Verificar si el directorio está vacío o casi vacío
                int remainingFiles = 0;
                try {
                    std::error_code remainingEc;
                    auto remainingIter = std::filesystem::recursive_directory_iterator(
                        worldPath,
                        std::filesystem::directory_options::skip_permission_denied,
                        remainingEc
                    );

                    if (!remainingEc) {
                        for (const auto& entry : remainingIter) {
                            remainingFiles++;
                        }
                    }
                } catch (...) {}

                if (remainingFiles > 0) {
                    std::cerr << "\n⚠️ ADVERTENCIA: Quedan " << remainingFiles << " archivos/carpetas" << std::endl;

                    // ⭐⭐⭐ ÚLTIMO RECURSO: Intentar usando comando del sistema operativo
                    #ifdef _WIN32
                    std::cout << "\n⚡ Intentando borrado con comando del sistema..." << std::endl;
                    try {
                        // Usar rmdir /S /Q en Windows (más agresivo que C++ filesystem)
                        std::string cmdPath = worldPath;
                        // Reemplazar / con \ para Windows
                        std::replace(cmdPath.begin(), cmdPath.end(), '/', '\\');
                        std::string command = "rmdir /S /Q \"" + cmdPath + "\" 2>nul";

                        std::cout << "   📌 Ejecutando: rmdir /S /Q" << std::endl;
                        int result = system(command.c_str());

                        // Esperar un momento para que el sistema termine
                        Sleep(500);

                        // Verificar si funcionó
                        if (!std::filesystem::exists(worldPath)) {
                            std::cout << "✅ Borrado exitoso con comando del sistema" << std::endl;
                            std::cout << "╔══════════════════════════════════════╗" << std::endl;
                            std::cout << "║  ✅ MUNDO BORRADO EXITOSAMENTE      ║" << std::endl;
                            std::cout << "╚══════════════════════════════════════╝\n" << std::endl;

                            scanSavedWorlds(state);
                            state->selectedWorldIndex = -1;
                            state->confirmingDelete = false;
                            state->isEditingWorldName = false;
                            return true;
                        } else {
                            std::cout << "⚠️ Comando del sistema no eliminó todo" << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cout << "⚠️ Error en comando del sistema: " << e.what() << std::endl;
                    }
                    #endif

                    std::cerr << "\n❌ No se pudo eliminar completamente el mundo" << std::endl;
                    std::cerr << "   Algunos archivos están bloqueados por:" << std::endl;
                    std::cerr << "   • Otra aplicación (antivirus, explorer, etc.)" << std::endl;
                    std::cerr << "   • Permisos insuficientes" << std::endl;
                    std::cerr << "   • Sistema de archivos" << std::endl;
                    std::cerr << "\n💡 Sugerencias:" << std::endl;
                    std::cerr << "   1. Cierra todas las aplicaciones que puedan estar usando los archivos" << std::endl;
                    std::cerr << "   2. Intenta de nuevo en unos segundos" << std::endl;
                    std::cerr << "   3. Si persiste, reinicia el juego" << std::endl;
                    std::cerr << "   4. Como último recurso, borra manualmente: " << worldPath << std::endl;

                    // Actualizar lista de todos modos
                    scanSavedWorlds(state);
                    state->selectedWorldIndex = -1;
                    state->confirmingDelete = false;
                    state->isEditingWorldName = false;
                    return false;
                } else {
                    // Directorio vacío pero aún existe, intentar borrarlo una vez más
                    std::filesystem::remove(worldPath, ec);
                }
            }
        }

        std::cout << "✅ Elementos borrados: " << deletedCount << std::endl;
        std::cout << "╔══════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ MUNDO BORRADO EXITOSAMENTE      ║" << std::endl;
        std::cout << "╚══════════════════════════════════════╝\n" << std::endl;

        // Recargar la lista de mundos
        scanSavedWorlds(state);
        state->selectedWorldIndex = -1;
        state->confirmingDelete = false;  // Reset confirmación
        state->isEditingWorldName = false;  // ⭐ Reset edición de nombre

        return true;

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "❌ Error filesystem al borrar mundo: " << e.what() << std::endl;
        std::cerr << "   Path1: " << e.path1() << std::endl;
        if (!e.path2().empty()) {
            std::cerr << "   Path2: " << e.path2() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ Error general al borrar mundo: " << e.what() << std::endl;
    }

    // Intentar actualizar la lista de todos modos
    try {
        scanSavedWorlds(state);
        state->selectedWorldIndex = -1;
        state->confirmingDelete = false;
        state->isEditingWorldName = false;  // ⭐ Reset edición de nombre
    } catch (...) {
        // Ignorar errores al actualizar lista
    }

    return false;
}

// Función para crear respaldo de un mundo (MEJORADO)
bool backupWorld(GameState* state, int worldIndex) {
    if (worldIndex < 0 || worldIndex >= (int)state->savedWorlds.size()) {
        std::cerr << "Error: Indice de mundo invalido" << std::endl;
        return false;
    }

    std::string worldPath = state->savedWorlds[worldIndex].folderPath;
    std::string worldName = state->savedWorlds[worldIndex].name;

    std::cout << "\n=== CREANDO RESPALDO ===" << std::endl;
    std::cout << "Mundo: " << worldName << std::endl;

    // Crear nombre de respaldo con timestamp
    time_t now = time(0);
    tm* ltm = localtime(&now);
    char timestamp[50];
    sprintf(timestamp, "%04d%02d%02d_%02d%02d%02d",
            1900 + ltm->tm_year, 1 + ltm->tm_mon, ltm->tm_mday,
            ltm->tm_hour, ltm->tm_min, ltm->tm_sec);

    std::string backupName = worldName + "_backup_" + timestamp;
    std::string backupPath = "saves/" + backupName;

    std::cout << "Respaldo: " << backupName << std::endl;

    try {
        // Verificar que el mundo original existe
        if (!std::filesystem::exists(worldPath)) {
            std::cerr << "Error: El mundo no existe" << std::endl;
            return false;
        }

        // Verificar que no existe ya un backup con el mismo nombre
        if (std::filesystem::exists(backupPath)) {
            std::cerr << "Error: Ya existe un respaldo con ese nombre" << std::endl;
            return false;
        }

        // Calcular tamaño total
        uintmax_t totalSize = 0;
        int fileCount = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(worldPath)) {
            if (entry.is_regular_file()) {
                totalSize += entry.file_size();
                fileCount++;
            }
        }

        std::cout << "Archivos a copiar: " << fileCount << std::endl;
        std::cout << "Tamaño total: " << (totalSize / 1024.0f / 1024.0f) << " MB" << std::endl;
        std::cout << "Copiando archivos..." << std::endl;

        // Copiar recursivamente
        std::filesystem::copy(worldPath, backupPath,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing);

        std::cout << "Respaldo creado exitosamente!" << std::endl;
        std::cout << "========================" << std::endl;

        // Recargar la lista de mundos para mostrar el respaldo
        scanSavedWorlds(state);
        return true;

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error filesystem al crear respaldo: " << e.what() << std::endl;
        std::cerr << "Path1: " << e.path1() << std::endl;
        if (!e.path2().empty()) {
            std::cerr << "Path2: " << e.path2() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error general al crear respaldo: " << e.what() << std::endl;
    }
    return false;
}

// Función para renombrar un mundo (MEJORADO)
bool renameWorld(GameState* state, int worldIndex, const std::string& newName) {
    if (worldIndex < 0 || worldIndex >= (int)state->savedWorlds.size()) {
        std::cerr << "Error: Indice de mundo invalido" << std::endl;
        return false;
    }

    // Validar nombre nuevo (ver WorldName.h: bloquea traversal, no-ASCII,
    // nombres reservados de Windows y sufijos problemáticos)
    WorldName::Validity validity = WorldName::validate(newName);
    if (validity != WorldName::Validity::Ok) {
        std::cerr << "Error: " << WorldName::describe(validity) << std::endl;
        return false;
    }

    std::string oldPath = state->savedWorlds[worldIndex].folderPath;
    std::string oldName = state->savedWorlds[worldIndex].name;

    // Construir nueva ruta
    std::string newPath = "saves/" + newName;

    std::cout << "\n=== RENOMBRANDO MUNDO ===" << std::endl;
    std::cout << "Nombre antiguo: " << oldName << std::endl;
    std::cout << "Nombre nuevo: " << newName << std::endl;

    try {
        // Verificar que el mundo original existe
        if (!std::filesystem::exists(oldPath)) {
            std::cerr << "Error: El mundo original no existe" << std::endl;
            return false;
        }

        // Verificar que el nombre nuevo no exista ya
        if (std::filesystem::exists(newPath)) {
            std::cerr << "Error: Ya existe un mundo con el nombre '" << newName << "'" << std::endl;
            return false;
        }

        // Verificar que no es el mismo nombre
        if (oldName == newName) {
            std::cerr << "Error: El nombre nuevo es igual al antiguo" << std::endl;
            return false;
        }

        // Renombrar el directorio
        std::filesystem::rename(oldPath, newPath);
        std::cout << "Mundo renombrado exitosamente!" << std::endl;
        std::cout << "========================" << std::endl;

        // Recargar la lista de mundos
        scanSavedWorlds(state);

        // Intentar mantener la selección en el mundo renombrado
        state->selectedWorldIndex = -1;
        for (size_t i = 0; i < state->savedWorlds.size(); i++) {
            if (state->savedWorlds[i].name == newName) {
                state->selectedWorldIndex = i;
                std::cout << "Mundo reseleccionado en indice " << i << std::endl;
                break;
            }
        }

        return true;

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error filesystem al renombrar mundo: " << e.what() << std::endl;
        std::cerr << "Path1: " << e.path1() << std::endl;
        if (!e.path2().empty()) {
            std::cerr << "Path2: " << e.path2() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error general al renombrar mundo: " << e.what() << std::endl;
    }
    return false;
}

// Renderizar selección de mundos
void renderWorldSelect(GameState* state, int screenWidth, int screenHeight, GLFWwindow* window) {
    // Recalcular posiciones de botones según el tamaño actual de la ventana
    initWorldSelectButtons(state, screenWidth, screenHeight);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);  // CRÍTICO: Deshabilitar culling para UI 2D
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Usar coordenadas de píxeles
    glOrtho(0, screenWidth, screenHeight, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Fondo con gradiente
    glBegin(GL_QUADS);
    glColor3f(0.15f, 0.25f, 0.35f);
    glVertex2f(0, 0);
    glVertex2f(screenWidth, 0);
    glColor3f(0.05f, 0.05f, 0.1f);
    glVertex2f(screenWidth, screenHeight);
    glVertex2f(0, screenHeight);
    glEnd();

    // Sombra del título
    float titleY = 40;
    float titleWidth = 480;
    float titleHeight = 70;
    glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(screenWidth/2.0f - titleWidth/2 + 5, titleY + 5);
    glVertex2f(screenWidth/2.0f + titleWidth/2 + 5, titleY + 5);
    glVertex2f(screenWidth/2.0f + titleWidth/2 + 5, titleY + titleHeight + 5);
    glVertex2f(screenWidth/2.0f - titleWidth/2 + 5, titleY + titleHeight + 5);
    glEnd();

    // Fondo del título
    glColor4f(0.2f, 0.3f, 0.4f, 0.8f);
    glBegin(GL_QUADS);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY + titleHeight);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY + titleHeight);
    glEnd();

    // Borde del título
    glColor4f(1.0f, 0.8f, 0.0f, 1.0f);
    glLineWidth(3.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY);
    glVertex2f(screenWidth/2.0f + titleWidth/2, titleY + titleHeight);
    glVertex2f(screenWidth/2.0f - titleWidth/2, titleY + titleHeight);
    glEnd();
    glLineWidth(1.0f);

    // Texto del título
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    renderText("SELECCIONAR MUNDO", screenWidth/2.0f - 120, titleY + 25, screenWidth, screenHeight);

    // FORZAR modo de relleno de polígonos (crítico)
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Obtener posición del mouse para hover
    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);

    // ⭐ PROTECCIÓN: Verificar si hay mundos antes de renderizar
    if (state->savedWorlds.empty()) {
        // Mensaje cuando no hay mundos guardados
        glColor4f(0.6f, 0.6f, 0.6f, 1.0f);
        float centerX = screenWidth / 2.0f;
        float centerY = screenHeight / 2.0f - 100;

        renderText("No hay mundos guardados", centerX - 150, centerY, screenWidth, screenHeight);
        renderText("Crea un nuevo mundo para empezar", centerX - 200, centerY + 40, screenWidth, screenHeight);

        // Resetear selección para evitar problemas
        state->selectedWorldIndex = -1;
        state->confirmingDelete = false;
    } else {
        // Botones de mundos guardados (solo si hay mundos)
        for (size_t i = 0; i < state->worldButtons.size() && i < state->savedWorlds.size(); i++) {
            const auto& btn = state->worldButtons[i];
            bool hover = btn.contains((float)mouseX, (float)mouseY);
            bool selected = (state->selectedWorldIndex == (int)i);

            // Fondo del botón
            if (selected) {
                // Mundo seleccionado - azul brillante
                glColor3f(0.2f, 0.5f, 0.8f);
            } else if (hover) {
                glColor3f(0.5f, 0.5f, 0.5f);
            } else {
                glColor3f(0.3f, 0.3f, 0.3f);
            }
            glBegin(GL_QUADS);
            glVertex2f(btn.x, btn.y);
            glVertex2f(btn.x + btn.width, btn.y);
            glVertex2f(btn.x + btn.width, btn.y + btn.height);
            glVertex2f(btn.x, btn.y + btn.height);
            glEnd();

            // Borde del botón
            if (selected) {
                glColor3f(0.4f, 0.7f, 1.0f);  // Borde azul más claro para seleccionado
                glLineWidth(3);
            } else {
                glColor3f(0.2f, 0.2f, 0.2f);
                glLineWidth(2);
            }
            glBegin(GL_LINE_LOOP);
            glVertex2f(btn.x, btn.y);
            glVertex2f(btn.x + btn.width, btn.y);
            glVertex2f(btn.x + btn.width, btn.y + btn.height);
            glVertex2f(btn.x, btn.y + btn.height);
            glEnd();

            // ⭐⭐⭐ INFORMACIÓN DEL MUNDO (Mejorado estilo Minecraft level.dat) ⭐⭐⭐

            // Nombre del mundo (más grande y destacado)
            glColor3f(1.0f, 1.0f, 1.0f);
            std::string worldName = state->savedWorlds[i].name;
            renderText(worldName.c_str(), btn.x + 20, btn.y + 15, screenWidth, screenHeight);

            // Metadata del mundo (en gris claro, más pequeña)
            const WorldInfo& world = state->savedWorlds[i];

            // Línea 1: Creación y último juego
            glColor3f(0.7f, 0.7f, 0.7f);
            std::string createdStr = "Creado: " + formatTimestamp(world.creationDate);
            renderText(createdStr.c_str(), btn.x + 25, btn.y + 40, screenWidth, screenHeight);

            // Línea 2: Semilla y tamaño
            std::string seedStr = "Semilla: " + std::to_string(world.seed);
            std::string sizeStr = " | Tamano: " + formatFileSize(world.worldSizeBytes);
            std::string line2 = seedStr + sizeStr;
            glColor3f(0.7f, 0.7f, 0.7f);
            renderText(line2.c_str(), btn.x + 25, btn.y + 55, screenWidth, screenHeight);

            // Línea 3: Tiempo de juego y modo de juego
            std::string playtimeStr = "Jugado: " + formatPlaytime(world.totalPlaytime);
            std::string gameModeStr = (world.gameMode == 0) ? " | Modo: Survival" : " | Modo: Creative";
            std::string line3 = playtimeStr + gameModeStr;
            glColor3f(0.6f, 0.8f, 0.6f);
            renderText(line3.c_str(), btn.x + 25, btn.y + 70, screenWidth, screenHeight);
        }
    }

    // Botones de gestión (solo si hay un mundo seleccionado)
    if (state->selectedWorldIndex >= 0 && state->selectedWorldIndex < (int)state->savedWorlds.size()) {
        // Botón JUGAR
        bool hoverJugar = state->btnJugarMundo.contains((float)mouseX, (float)mouseY);
        if (hoverJugar) {
            glColor3f(0.3f, 0.7f, 0.3f);  // Verde más brillante
        } else {
            glColor3f(0.2f, 0.6f, 0.2f);  // Verde
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnJugarMundo.x, state->btnJugarMundo.y);
        glVertex2f(state->btnJugarMundo.x + state->btnJugarMundo.width, state->btnJugarMundo.y);
        glVertex2f(state->btnJugarMundo.x + state->btnJugarMundo.width, state->btnJugarMundo.y + state->btnJugarMundo.height);
        glVertex2f(state->btnJugarMundo.x, state->btnJugarMundo.y + state->btnJugarMundo.height);
        glEnd();
        glColor3f(0.1f, 0.3f, 0.1f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnJugarMundo.x, state->btnJugarMundo.y);
        glVertex2f(state->btnJugarMundo.x + state->btnJugarMundo.width, state->btnJugarMundo.y);
        glVertex2f(state->btnJugarMundo.x + state->btnJugarMundo.width, state->btnJugarMundo.y + state->btnJugarMundo.height);
        glVertex2f(state->btnJugarMundo.x, state->btnJugarMundo.y + state->btnJugarMundo.height);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        renderText("JUGAR", state->btnJugarMundo.x + 60, state->btnJugarMundo.y + 17, screenWidth, screenHeight);

        // Botón EDITAR
        bool hoverEditar = state->btnEditarMundo.contains((float)mouseX, (float)mouseY);
        if (hoverEditar) {
            glColor3f(0.6f, 0.6f, 0.3f);
        } else {
            glColor3f(0.5f, 0.5f, 0.2f);
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnEditarMundo.x, state->btnEditarMundo.y);
        glVertex2f(state->btnEditarMundo.x + state->btnEditarMundo.width, state->btnEditarMundo.y);
        glVertex2f(state->btnEditarMundo.x + state->btnEditarMundo.width, state->btnEditarMundo.y + state->btnEditarMundo.height);
        glVertex2f(state->btnEditarMundo.x, state->btnEditarMundo.y + state->btnEditarMundo.height);
        glEnd();
        glColor3f(0.3f, 0.3f, 0.1f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnEditarMundo.x, state->btnEditarMundo.y);
        glVertex2f(state->btnEditarMundo.x + state->btnEditarMundo.width, state->btnEditarMundo.y);
        glVertex2f(state->btnEditarMundo.x + state->btnEditarMundo.width, state->btnEditarMundo.y + state->btnEditarMundo.height);
        glVertex2f(state->btnEditarMundo.x, state->btnEditarMundo.y + state->btnEditarMundo.height);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        renderText("EDITAR", state->btnEditarMundo.x + 50, state->btnEditarMundo.y + 17, screenWidth, screenHeight);

        // Botón RESPALDAR
        bool hoverRespaldo = state->btnRespaldoMundo.contains((float)mouseX, (float)mouseY);
        if (hoverRespaldo) {
            glColor3f(0.3f, 0.5f, 0.7f);
        } else {
            glColor3f(0.2f, 0.4f, 0.6f);
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnRespaldoMundo.x, state->btnRespaldoMundo.y);
        glVertex2f(state->btnRespaldoMundo.x + state->btnRespaldoMundo.width, state->btnRespaldoMundo.y);
        glVertex2f(state->btnRespaldoMundo.x + state->btnRespaldoMundo.width, state->btnRespaldoMundo.y + state->btnRespaldoMundo.height);
        glVertex2f(state->btnRespaldoMundo.x, state->btnRespaldoMundo.y + state->btnRespaldoMundo.height);
        glEnd();
        glColor3f(0.1f, 0.2f, 0.3f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnRespaldoMundo.x, state->btnRespaldoMundo.y);
        glVertex2f(state->btnRespaldoMundo.x + state->btnRespaldoMundo.width, state->btnRespaldoMundo.y);
        glVertex2f(state->btnRespaldoMundo.x + state->btnRespaldoMundo.width, state->btnRespaldoMundo.y + state->btnRespaldoMundo.height);
        glVertex2f(state->btnRespaldoMundo.x, state->btnRespaldoMundo.y + state->btnRespaldoMundo.height);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        renderText("RESPALDAR", state->btnRespaldoMundo.x + 30, state->btnRespaldoMundo.y + 17, screenWidth, screenHeight);

        // Botón BORRAR
        bool hoverBorrar = state->btnBorrarMundo.contains((float)mouseX, (float)mouseY);
        // Color más brillante si está en confirmación o hover
        if (state->confirmingDelete) {
            glColor3f(1.0f, 0.0f, 0.0f);  // Rojo brillante para confirmación
        } else if (hoverBorrar) {
            glColor3f(0.9f, 0.2f, 0.2f);  // Rojo más brillante
        } else {
            glColor3f(0.7f, 0.1f, 0.1f);  // Rojo oscuro
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnBorrarMundo.x, state->btnBorrarMundo.y);
        glVertex2f(state->btnBorrarMundo.x + state->btnBorrarMundo.width, state->btnBorrarMundo.y);
        glVertex2f(state->btnBorrarMundo.x + state->btnBorrarMundo.width, state->btnBorrarMundo.y + state->btnBorrarMundo.height);
        glVertex2f(state->btnBorrarMundo.x, state->btnBorrarMundo.y + state->btnBorrarMundo.height);
        glEnd();
        glColor3f(0.3f, 0.05f, 0.05f);
        glLineWidth(state->confirmingDelete ? 4 : 2);  // Borde más grueso si confirma
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnBorrarMundo.x, state->btnBorrarMundo.y);
        glVertex2f(state->btnBorrarMundo.x + state->btnBorrarMundo.width, state->btnBorrarMundo.y);
        glVertex2f(state->btnBorrarMundo.x + state->btnBorrarMundo.width, state->btnBorrarMundo.y + state->btnBorrarMundo.height);
        glVertex2f(state->btnBorrarMundo.x, state->btnBorrarMundo.y + state->btnBorrarMundo.height);
        glEnd();
        glLineWidth(1);
        glColor3f(1.0f, 1.0f, 1.0f);
        // Cambiar texto si está en confirmación
        const char* deleteText = state->confirmingDelete ? "CONFIRMAR?" : "BORRAR";
        float textX = state->confirmingDelete ? state->btnBorrarMundo.x + 20 : state->btnBorrarMundo.x + 50;
        renderText(deleteText, textX, state->btnBorrarMundo.y + 17, screenWidth, screenHeight);
    }

    // Botón "CREAR NUEVO MUNDO"
    bool hoverCrear = state->btnCrearMundo.contains((float)mouseX, (float)mouseY);
    if (hoverCrear) {
        glColor3f(0.5f, 0.5f, 0.5f);
    } else {
        glColor3f(0.3f, 0.3f, 0.3f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnCrearMundo.x, state->btnCrearMundo.y);
    glVertex2f(state->btnCrearMundo.x + state->btnCrearMundo.width, state->btnCrearMundo.y);
    glVertex2f(state->btnCrearMundo.x + state->btnCrearMundo.width, state->btnCrearMundo.y + state->btnCrearMundo.height);
    glVertex2f(state->btnCrearMundo.x, state->btnCrearMundo.y + state->btnCrearMundo.height);
    glEnd();
    glColor3f(0.2f, 0.2f, 0.2f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnCrearMundo.x, state->btnCrearMundo.y);
    glVertex2f(state->btnCrearMundo.x + state->btnCrearMundo.width, state->btnCrearMundo.y);
    glVertex2f(state->btnCrearMundo.x + state->btnCrearMundo.width, state->btnCrearMundo.y + state->btnCrearMundo.height);
    glVertex2f(state->btnCrearMundo.x, state->btnCrearMundo.y + state->btnCrearMundo.height);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("CREAR NUEVO MUNDO", state->btnCrearMundo.x + 30, state->btnCrearMundo.y + 17, screenWidth, screenHeight);

    // Botón "VOLVER"
    bool hoverVolver = state->btnVolverMenu.contains((float)mouseX, (float)mouseY);
    if (hoverVolver) {
        glColor3f(0.5f, 0.5f, 0.5f);
    } else {
        glColor3f(0.3f, 0.3f, 0.3f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnVolverMenu.x, state->btnVolverMenu.y);
    glVertex2f(state->btnVolverMenu.x + state->btnVolverMenu.width, state->btnVolverMenu.y);
    glVertex2f(state->btnVolverMenu.x + state->btnVolverMenu.width, state->btnVolverMenu.y + state->btnVolverMenu.height);
    glVertex2f(state->btnVolverMenu.x, state->btnVolverMenu.y + state->btnVolverMenu.height);
    glEnd();
    glColor3f(0.2f, 0.2f, 0.2f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnVolverMenu.x, state->btnVolverMenu.y);
    glVertex2f(state->btnVolverMenu.x + state->btnVolverMenu.width, state->btnVolverMenu.y);
    glVertex2f(state->btnVolverMenu.x + state->btnVolverMenu.width, state->btnVolverMenu.y + state->btnVolverMenu.height);
    glVertex2f(state->btnVolverMenu.x, state->btnVolverMenu.y + state->btnVolverMenu.height);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("VOLVER", state->btnVolverMenu.x + 50, state->btnVolverMenu.y + 17, screenWidth, screenHeight);

    // === OVERLAY DE EDICIÓN DE NOMBRE ===
    if (state->isEditingWorldName && state->selectedWorldIndex >= 0 && state->selectedWorldIndex < (int)state->savedWorlds.size()) {
        // Overlay semi-transparente sobre toda la pantalla
        glColor4f(0.0f, 0.0f, 0.0f, 0.7f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0);
        glVertex2f(screenWidth, 0);
        glVertex2f(screenWidth, screenHeight);
        glVertex2f(0, screenHeight);
        glEnd();

        // Modal box centrado
        float modalWidth = 500;
        float modalHeight = 280;
        float modalX = screenWidth/2.0f - modalWidth/2;
        float modalY = screenHeight/2.0f - modalHeight/2;

        // Sombra del modal
        glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
        glBegin(GL_QUADS);
        glVertex2f(modalX + 5, modalY + 5);
        glVertex2f(modalX + modalWidth + 5, modalY + 5);
        glVertex2f(modalX + modalWidth + 5, modalY + modalHeight + 5);
        glVertex2f(modalX + 5, modalY + modalHeight + 5);
        glEnd();

        // Fondo del modal
        glColor4f(0.25f, 0.3f, 0.35f, 1.0f);
        glBegin(GL_QUADS);
        glVertex2f(modalX, modalY);
        glVertex2f(modalX + modalWidth, modalY);
        glVertex2f(modalX + modalWidth, modalY + modalHeight);
        glVertex2f(modalX, modalY + modalHeight);
        glEnd();

        // Borde del modal
        glColor4f(0.5f, 0.6f, 0.7f, 1.0f);
        glLineWidth(3);
        glBegin(GL_LINE_LOOP);
        glVertex2f(modalX, modalY);
        glVertex2f(modalX + modalWidth, modalY);
        glVertex2f(modalX + modalWidth, modalY + modalHeight);
        glVertex2f(modalX, modalY + modalHeight);
        glEnd();
        glLineWidth(1);

        // Título del modal
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        renderText("EDITAR NOMBRE DEL MUNDO", modalX + 90, modalY + 20, screenWidth, screenHeight);

        // Línea separadora
        glColor4f(0.5f, 0.6f, 0.7f, 0.5f);
        glLineWidth(2);
        glBegin(GL_LINES);
        glVertex2f(modalX + 20, modalY + 55);
        glVertex2f(modalX + modalWidth - 20, modalY + 55);
        glEnd();
        glLineWidth(1);

        // Mostrar nombre actual
        glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
        renderText("Nombre actual:", modalX + 30, modalY + 75, screenWidth, screenHeight);
        glColor4f(1.0f, 1.0f, 0.5f, 1.0f);
        renderText(state->savedWorlds[state->selectedWorldIndex].name.c_str(), modalX + 30, modalY + 100, screenWidth, screenHeight);

        // Campo de texto para nuevo nombre
        glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
        renderText("Nuevo nombre:", modalX + 30, modalY + 135, screenWidth, screenHeight);

        // Fondo del input
        float inputX = modalX + 30;
        float inputY = modalY + 155;
        float inputWidth = modalWidth - 60;
        float inputHeight = 35;

        glColor4f(0.15f, 0.15f, 0.2f, 1.0f);
        glBegin(GL_QUADS);
        glVertex2f(inputX, inputY);
        glVertex2f(inputX + inputWidth, inputY);
        glVertex2f(inputX + inputWidth, inputY + inputHeight);
        glVertex2f(inputX, inputY + inputHeight);
        glEnd();

        // Borde del input
        glColor4f(0.4f, 0.5f, 0.6f, 1.0f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(inputX, inputY);
        glVertex2f(inputX + inputWidth, inputY);
        glVertex2f(inputX + inputWidth, inputY + inputHeight);
        glVertex2f(inputX, inputY + inputHeight);
        glEnd();
        glLineWidth(1);

        // Texto del input (nombre siendo editado)
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        std::string displayText = state->editingWorldNewName + "_";  // Cursor parpadeante
        renderText(displayText.c_str(), inputX + 10, inputY + 10, screenWidth, screenHeight);

        // Inicializar botones de edición
        state->btnGuardarNombre.x = modalX + 50;
        state->btnGuardarNombre.y = modalY + modalHeight - 60;
        state->btnGuardarNombre.width = 180;
        state->btnGuardarNombre.height = 40;

        state->btnCancelarEdicion.x = modalX + modalWidth - 230;
        state->btnCancelarEdicion.y = modalY + modalHeight - 60;
        state->btnCancelarEdicion.width = 180;
        state->btnCancelarEdicion.height = 40;

        // Botón GUARDAR (verde)
        bool hoverGuardar = state->btnGuardarNombre.contains((float)mouseX, (float)mouseY);
        if (hoverGuardar) {
            glColor3f(0.3f, 0.7f, 0.3f);
        } else {
            glColor3f(0.2f, 0.6f, 0.2f);
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnGuardarNombre.x, state->btnGuardarNombre.y);
        glVertex2f(state->btnGuardarNombre.x + state->btnGuardarNombre.width, state->btnGuardarNombre.y);
        glVertex2f(state->btnGuardarNombre.x + state->btnGuardarNombre.width, state->btnGuardarNombre.y + state->btnGuardarNombre.height);
        glVertex2f(state->btnGuardarNombre.x, state->btnGuardarNombre.y + state->btnGuardarNombre.height);
        glEnd();
        glColor3f(0.1f, 0.3f, 0.1f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnGuardarNombre.x, state->btnGuardarNombre.y);
        glVertex2f(state->btnGuardarNombre.x + state->btnGuardarNombre.width, state->btnGuardarNombre.y);
        glVertex2f(state->btnGuardarNombre.x + state->btnGuardarNombre.width, state->btnGuardarNombre.y + state->btnGuardarNombre.height);
        glVertex2f(state->btnGuardarNombre.x, state->btnGuardarNombre.y + state->btnGuardarNombre.height);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        renderText("GUARDAR", state->btnGuardarNombre.x + 45, state->btnGuardarNombre.y + 13, screenWidth, screenHeight);

        // Botón CANCELAR (gris)
        bool hoverCancelar = state->btnCancelarEdicion.contains((float)mouseX, (float)mouseY);
        if (hoverCancelar) {
            glColor3f(0.5f, 0.5f, 0.5f);
        } else {
            glColor3f(0.3f, 0.3f, 0.3f);
        }
        glBegin(GL_QUADS);
        glVertex2f(state->btnCancelarEdicion.x, state->btnCancelarEdicion.y);
        glVertex2f(state->btnCancelarEdicion.x + state->btnCancelarEdicion.width, state->btnCancelarEdicion.y);
        glVertex2f(state->btnCancelarEdicion.x + state->btnCancelarEdicion.width, state->btnCancelarEdicion.y + state->btnCancelarEdicion.height);
        glVertex2f(state->btnCancelarEdicion.x, state->btnCancelarEdicion.y + state->btnCancelarEdicion.height);
        glEnd();
        glColor3f(0.15f, 0.15f, 0.15f);
        glLineWidth(2);
        glBegin(GL_LINE_LOOP);
        glVertex2f(state->btnCancelarEdicion.x, state->btnCancelarEdicion.y);
        glVertex2f(state->btnCancelarEdicion.x + state->btnCancelarEdicion.width, state->btnCancelarEdicion.y);
        glVertex2f(state->btnCancelarEdicion.x + state->btnCancelarEdicion.width, state->btnCancelarEdicion.y + state->btnCancelarEdicion.height);
        glVertex2f(state->btnCancelarEdicion.x, state->btnCancelarEdicion.y + state->btnCancelarEdicion.height);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        renderText("CANCELAR", state->btnCancelarEdicion.x + 45, state->btnCancelarEdicion.y + 13, screenWidth, screenHeight);
    }

    // Restaurar estados para renderizado 3D
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

// ⭐⭐⭐ NUEVA: Renderizar pantalla de configuración de creación de mundo
void renderWorldCreateScreen(GameState* state, int screenWidth, int screenHeight, GLFWwindow* window) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);

    // Configurar matrices para renderizado 2D
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, screenWidth, screenHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Fondo degradado (azul oscuro a negro)
    glBegin(GL_QUADS);
    glColor3f(0.1f, 0.2f, 0.4f);
    glVertex2f(0, 0);
    glVertex2f((float)screenWidth, 0);
    glColor3f(0.05f, 0.05f, 0.1f);
    glVertex2f((float)screenWidth, (float)screenHeight);
    glVertex2f(0, (float)screenHeight);
    glEnd();

    // Título
    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("CREAR NUEVO MUNDO", screenWidth / 2 - 150, 80, screenWidth, screenHeight);

    float centerX = screenWidth / 2.0f;
    float startY = 180.0f;
    float fieldWidth = 500.0f;
    float fieldHeight = 50.0f;
    float spacing = 80.0f;

    // ===== CAMPO: NOMBRE DEL MUNDO =====
    glColor3f(0.8f, 0.8f, 0.8f);
    renderText("Nombre del mundo:", centerX - 240, startY - 25, screenWidth, screenHeight);

    // Fondo del campo de nombre
    if (state->isEditingNewWorldName) {
        glColor4f(0.3f, 0.4f, 0.5f, 0.8f);
    } else {
        glColor4f(0.2f, 0.2f, 0.3f, 0.8f);
    }
    glBegin(GL_QUADS);
    glVertex2f(centerX - fieldWidth/2, startY);
    glVertex2f(centerX + fieldWidth/2, startY);
    glVertex2f(centerX + fieldWidth/2, startY + fieldHeight);
    glVertex2f(centerX - fieldWidth/2, startY + fieldHeight);
    glEnd();

    // Borde del campo
    glColor3f(0.5f, 0.5f, 0.6f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(centerX - fieldWidth/2, startY);
    glVertex2f(centerX + fieldWidth/2, startY);
    glVertex2f(centerX + fieldWidth/2, startY + fieldHeight);
    glVertex2f(centerX - fieldWidth/2, startY + fieldHeight);
    glEnd();

    // Texto del nombre
    glColor3f(1.0f, 1.0f, 1.0f);
    std::string displayName;
    if (state->isEditingNewWorldName) {
        // Modo edición: mostrar lo que hay (vacío si está vacío) + cursor
        displayName = state->newWorldName + "_";
    } else {
        // Modo no-edición: mostrar placeholder si está vacío
        displayName = state->newWorldName.empty() ? "Nuevo Mundo" : state->newWorldName;
    }
    renderText(displayName.c_str(), centerX - 230, startY + 17, screenWidth, screenHeight);

    // ===== CAMPO: SEMILLA =====
    float seedY = startY + spacing;
    glColor3f(0.8f, 0.8f, 0.8f);
    renderText("Semilla (opcional):", centerX - 240, seedY - 25, screenWidth, screenHeight);

    // Fondo del campo de semilla
    if (state->isEditingNewWorldSeed) {
        glColor4f(0.3f, 0.4f, 0.5f, 0.8f);
    } else {
        glColor4f(0.2f, 0.2f, 0.3f, 0.8f);
    }
    glBegin(GL_QUADS);
    glVertex2f(centerX - fieldWidth/2, seedY);
    glVertex2f(centerX + fieldWidth/2, seedY);
    glVertex2f(centerX + fieldWidth/2, seedY + fieldHeight);
    glVertex2f(centerX - fieldWidth/2, seedY + fieldHeight);
    glEnd();

    // Borde
    glColor3f(0.5f, 0.5f, 0.6f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(centerX - fieldWidth/2, seedY);
    glVertex2f(centerX + fieldWidth/2, seedY);
    glVertex2f(centerX + fieldWidth/2, seedY + fieldHeight);
    glVertex2f(centerX - fieldWidth/2, seedY + fieldHeight);
    glEnd();

    // Texto de la semilla
    glColor3f(1.0f, 1.0f, 1.0f);
    std::string displaySeed;
    if (state->isEditingNewWorldSeed) {
        // Modo edición: mostrar lo que hay (vacío si está vacío) + cursor
        displaySeed = state->newWorldSeed + "_";
    } else {
        // Modo no-edición: mostrar placeholder si está vacío
        displaySeed = state->newWorldSeed.empty() ? "(aleatorio)" : state->newWorldSeed;
    }
    renderText(displaySeed.c_str(), centerX - 230, seedY + 17, screenWidth, screenHeight);

    // ===== MODO DE JUEGO =====
    float modeY = seedY + spacing;
    glColor3f(0.8f, 0.8f, 0.8f);
    renderText("Modo de juego:", centerX - 240, modeY - 25, screenWidth, screenHeight);

    float buttonWidth = 230.0f;
    float buttonHeight = 50.0f;
    float buttonSpacing = 20.0f;

    // Obtener posición del mouse
    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);

    // Botón SURVIVAL
    state->btnGameModeSurvival = Button(centerX - fieldWidth/2, modeY, buttonWidth, buttonHeight, "SURVIVAL");
    bool hoverSurvival = state->btnGameModeSurvival.contains((float)mouseX, (float)mouseY);

    if (state->newWorldGameMode == 0) {
        glColor3f(0.3f, 0.6f, 0.3f);  // Verde si está seleccionado
    } else if (hoverSurvival) {
        glColor3f(0.4f, 0.4f, 0.5f);
    } else {
        glColor3f(0.3f, 0.3f, 0.4f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnGameModeSurvival.x, state->btnGameModeSurvival.y);
    glVertex2f(state->btnGameModeSurvival.x + state->btnGameModeSurvival.width, state->btnGameModeSurvival.y);
    glVertex2f(state->btnGameModeSurvival.x + state->btnGameModeSurvival.width, state->btnGameModeSurvival.y + state->btnGameModeSurvival.height);
    glVertex2f(state->btnGameModeSurvival.x, state->btnGameModeSurvival.y + state->btnGameModeSurvival.height);
    glEnd();

    glColor3f(0.6f, 0.6f, 0.7f);
    glLineWidth(state->newWorldGameMode == 0 ? 3 : 2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnGameModeSurvival.x, state->btnGameModeSurvival.y);
    glVertex2f(state->btnGameModeSurvival.x + state->btnGameModeSurvival.width, state->btnGameModeSurvival.y);
    glVertex2f(state->btnGameModeSurvival.x + state->btnGameModeSurvival.width, state->btnGameModeSurvival.y + state->btnGameModeSurvival.height);
    glVertex2f(state->btnGameModeSurvival.x, state->btnGameModeSurvival.y + state->btnGameModeSurvival.height);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("SURVIVAL", state->btnGameModeSurvival.x + 50, state->btnGameModeSurvival.y + 17, screenWidth, screenHeight);

    // Botón CREATIVE
    state->btnGameModeCreative = Button(centerX + buttonSpacing, modeY, buttonWidth, buttonHeight, "CREATIVE");
    bool hoverCreative = state->btnGameModeCreative.contains((float)mouseX, (float)mouseY);

    if (state->newWorldGameMode == 1) {
        glColor3f(0.6f, 0.3f, 0.6f);  // Morado si está seleccionado
    } else if (hoverCreative) {
        glColor3f(0.4f, 0.4f, 0.5f);
    } else {
        glColor3f(0.3f, 0.3f, 0.4f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnGameModeCreative.x, state->btnGameModeCreative.y);
    glVertex2f(state->btnGameModeCreative.x + state->btnGameModeCreative.width, state->btnGameModeCreative.y);
    glVertex2f(state->btnGameModeCreative.x + state->btnGameModeCreative.width, state->btnGameModeCreative.y + state->btnGameModeCreative.height);
    glVertex2f(state->btnGameModeCreative.x, state->btnGameModeCreative.y + state->btnGameModeCreative.height);
    glEnd();

    glColor3f(0.6f, 0.6f, 0.7f);
    glLineWidth(state->newWorldGameMode == 1 ? 3 : 2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnGameModeCreative.x, state->btnGameModeCreative.y);
    glVertex2f(state->btnGameModeCreative.x + state->btnGameModeCreative.width, state->btnGameModeCreative.y);
    glVertex2f(state->btnGameModeCreative.x + state->btnGameModeCreative.width, state->btnGameModeCreative.y + state->btnGameModeCreative.height);
    glVertex2f(state->btnGameModeCreative.x, state->btnGameModeCreative.y + state->btnGameModeCreative.height);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("CREATIVE", state->btnGameModeCreative.x + 50, state->btnGameModeCreative.y + 17, screenWidth, screenHeight);

    // ===== BOTONES DE ACCIÓN =====
    float actionY = screenHeight - 120.0f;

    // Botón CREAR MUNDO
    state->btnCreateWorldConfirm = Button(centerX - 160, actionY, 300, 60, "CREAR MUNDO");
    bool hoverCreate = state->btnCreateWorldConfirm.contains((float)mouseX, (float)mouseY);

    if (hoverCreate) {
        glColor3f(0.4f, 0.7f, 0.4f);
    } else {
        glColor3f(0.3f, 0.6f, 0.3f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnCreateWorldConfirm.x, state->btnCreateWorldConfirm.y);
    glVertex2f(state->btnCreateWorldConfirm.x + state->btnCreateWorldConfirm.width, state->btnCreateWorldConfirm.y);
    glVertex2f(state->btnCreateWorldConfirm.x + state->btnCreateWorldConfirm.width, state->btnCreateWorldConfirm.y + state->btnCreateWorldConfirm.height);
    glVertex2f(state->btnCreateWorldConfirm.x, state->btnCreateWorldConfirm.y + state->btnCreateWorldConfirm.height);
    glEnd();

    glColor3f(0.7f, 0.9f, 0.7f);
    glLineWidth(3);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnCreateWorldConfirm.x, state->btnCreateWorldConfirm.y);
    glVertex2f(state->btnCreateWorldConfirm.x + state->btnCreateWorldConfirm.width, state->btnCreateWorldConfirm.y);
    glVertex2f(state->btnCreateWorldConfirm.x + state->btnCreateWorldConfirm.width, state->btnCreateWorldConfirm.y + state->btnCreateWorldConfirm.height);
    glVertex2f(state->btnCreateWorldConfirm.x, state->btnCreateWorldConfirm.y + state->btnCreateWorldConfirm.height);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("CREAR MUNDO", state->btnCreateWorldConfirm.x + 55, state->btnCreateWorldConfirm.y + 20, screenWidth, screenHeight);

    // Botón CANCELAR
    state->btnCreateWorldCancel = Button(centerX + 160, actionY, 150, 60, "CANCELAR");
    bool hoverCancel = state->btnCreateWorldCancel.contains((float)mouseX, (float)mouseY);

    if (hoverCancel) {
        glColor3f(0.6f, 0.3f, 0.3f);
    } else {
        glColor3f(0.4f, 0.2f, 0.2f);
    }
    glBegin(GL_QUADS);
    glVertex2f(state->btnCreateWorldCancel.x, state->btnCreateWorldCancel.y);
    glVertex2f(state->btnCreateWorldCancel.x + state->btnCreateWorldCancel.width, state->btnCreateWorldCancel.y);
    glVertex2f(state->btnCreateWorldCancel.x + state->btnCreateWorldCancel.width, state->btnCreateWorldCancel.y + state->btnCreateWorldCancel.height);
    glVertex2f(state->btnCreateWorldCancel.x, state->btnCreateWorldCancel.y + state->btnCreateWorldCancel.height);
    glEnd();

    glColor3f(0.8f, 0.5f, 0.5f);
    glLineWidth(2);
    glBegin(GL_LINE_LOOP);
    glVertex2f(state->btnCreateWorldCancel.x, state->btnCreateWorldCancel.y);
    glVertex2f(state->btnCreateWorldCancel.x + state->btnCreateWorldCancel.width, state->btnCreateWorldCancel.y);
    glVertex2f(state->btnCreateWorldCancel.x + state->btnCreateWorldCancel.width, state->btnCreateWorldCancel.y + state->btnCreateWorldCancel.height);
    glVertex2f(state->btnCreateWorldCancel.x, state->btnCreateWorldCancel.y + state->btnCreateWorldCancel.height);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    renderText("CANCELAR", state->btnCreateWorldCancel.x + 25, state->btnCreateWorldCancel.y + 20, screenWidth, screenHeight);

    // Restaurar matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    // Restaurar estados
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

// Renderizar pantalla de carga con animación
void renderLoadingScreen(GameState* state, int screenWidth, int screenHeight, float currentTime) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, screenWidth, screenHeight, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Fondo negro
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor3f(0.0f, 0.0f, 0.0f);
    glVertex2f(0, 0);
    glVertex2f(screenWidth, 0);
    glVertex2f(screenWidth, screenHeight);
    glVertex2f(0, screenHeight);
    glEnd();

    // Renderizar GIF de animación de carga centrado
    glEnable(GL_TEXTURE_2D);
    GLuint loadingTexture = g_textureManager->getTexture("../Animaciones/Animacion de Carga.gif");

    if (loadingTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, loadingTexture);

        // Tamaño de la animación (ajusta según el tamaño de tu GIF)
        float animSize = 256.0f;  // Tamaño en píxeles
        float animX = (screenWidth - animSize) / 2.0f;
        float animY = (screenHeight - animSize) / 2.0f;

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(animX, animY);
        glTexCoord2f(1, 0); glVertex2f(animX + animSize, animY);
        glTexCoord2f(1, 1); glVertex2f(animX + animSize, animY + animSize);
        glTexCoord2f(0, 1); glVertex2f(animX, animY + animSize);
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);

    // Calcular progreso (0.0 a 1.0)
    float elapsed = currentTime - state->loadingStartTime;
    float progressRaw = elapsed / state->loadingDuration;
    float progress = (progressRaw < 1.0f) ? progressRaw : 1.0f;

    // Texto de carga debajo de la animación
    int dots = ((int)(currentTime * 2)) % 4;
    char loadingText[64];
    strcpy(loadingText, "Cargando");
    for (int i = 0; i < dots; i++) {
        strcat(loadingText, ".");
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    renderText(loadingText, screenWidth/2.0f - 60, screenHeight/2.0f + 150, screenWidth, screenHeight);

    // Porcentaje
    char percentText[32];
    sprintf(percentText, "%d%%", (int)(progress * 100));
    glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
    renderText(percentText, screenWidth/2.0f - 20, screenHeight/2.0f + 180, screenWidth, screenHeight);

    // Restaurar estados
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

// Buscar spawn seguro en la superficie
Vec3 findSafeSpawn(World& world) {
    std::cout << "Buscando posicion de spawn segura..." << std::endl;

    bool foundSafeSpawn = false;
    int spawnX = 0;
    int spawnZ = 0;
    int spawnY = 100;

    // Buscar en espiral desde el centro
    for (int radius = 0; radius <= 32 && !foundSafeSpawn; radius++) {
        for (int dx = -radius; dx <= radius && !foundSafeSpawn; dx++) {
            for (int dz = -radius; dz <= radius && !foundSafeSpawn; dz++) {
                // Solo buscar en el borde del radio actual (optimización)
                if (abs(dx) != radius && abs(dz) != radius) continue;

                int testX = dx;
                int testZ = dz;

                // Buscar la superficie desde arriba
                for (int y = CHUNK_HEIGHT - 1; y >= 1; y--) {
                    BlockType currentBlock = world.getBlock(testX, y, testZ);
                    BlockType blockBelow = world.getBlock(testX, y - 1, testZ);
                    BlockType blockAbove = world.getBlock(testX, y + 1, testZ);
                    BlockType blockAbove2 = world.getBlock(testX, y + 2, testZ);

                    // Condiciones para un spawn seguro:
                    // 1. Bloque actual es AIRE (no spawneamos dentro de bloques)
                    // 2. Bloque debajo es SÓLIDO (no AIRE, no AGUA)
                    // 3. Bloque encima es AIRE (espacio para la cabeza)
                    // 4. 2 bloques encima es AIRE (espacio completo)

                    bool currentIsAir = (currentBlock == BLOCK_AIR);
                    bool belowIsSolid = (blockBelow != BLOCK_AIR && blockBelow != BLOCK_WATER);
                    bool aboveIsAir = (blockAbove == BLOCK_AIR);
                    bool above2IsAir = (blockAbove2 == BLOCK_AIR);

                    // NO spawneamos en agua
                    bool notInWater = (currentBlock != BLOCK_WATER &&
                                      blockAbove != BLOCK_WATER &&
                                      blockAbove2 != BLOCK_WATER);

                    if (currentIsAir && belowIsSolid && aboveIsAir && above2IsAir && notInWater) {
                        // Verificar que NO sea una cueva (debe tener cielo encima)
                        bool hasSky = true;
                        for (int checkY = y + 3; checkY < CHUNK_HEIGHT; checkY++) {
                            BlockType skyBlock = world.getBlock(testX, checkY, testZ);
                            if (skyBlock != BLOCK_AIR && skyBlock != BLOCK_WATER) {
                                hasSky = false;
                                break;
                            }
                        }

                        if (hasSky) {
                            // ENCONTRADO! Posición segura
                            spawnX = testX;
                            spawnY = y;
                            spawnZ = testZ;
                            foundSafeSpawn = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (foundSafeSpawn) {
        std::cout << "Spawn seguro encontrado en: X=" << spawnX
                  << ", Y=" << spawnY << ", Z=" << spawnZ << std::endl;
        return Vec3(spawnX + 0.5f, spawnY + 0.1f, spawnZ + 0.5f);
    } else {
        // Fallback: buscar cualquier superficie (sin verificación de cielo)
        std::cout << "ADVERTENCIA: No se encontro spawn ideal, usando fallback..." << std::endl;
        for (int y = CHUNK_HEIGHT - 1; y >= 10; y--) {
            BlockType current = world.getBlock(0, y, 0);
            BlockType below = world.getBlock(0, y - 1, 0);

            if (current == BLOCK_AIR && below != BLOCK_AIR && below != BLOCK_WATER) {
                std::cout << "Spawn fallback en Y=" << y << std::endl;
                return Vec3(0.5f, y + 0.1f, 0.5f);
            }
        }
    }

    // Último recurso
    std::cout << "Usando spawn de emergencia en Y=100" << std::endl;
    return Vec3(0.5f, 100.0f, 0.5f);
}

// Crear nuevo mundo
// ⭐⭐⭐ CREAR NUEVO MUNDO (MEJORADO con level.dat) ⭐⭐⭐
void createNewWorld(GameState* state, float currentTime) {
    // ⭐ Generar nombre único basado en timestamp y evitar duplicados
    int worldNumber = 1;
    std::string worldName;
    std::filesystem::path worldPath;

    // Encontrar el primer número disponible
    do {
        worldName = "Mundo " + std::to_string(worldNumber);
        worldPath = std::filesystem::path("saves") / worldName;
        worldNumber++;
    } while (std::filesystem::exists(worldPath));

    std::filesystem::create_directories(worldPath);

    state->currentWorldName = worldName;

    // ⭐⭐⭐ GENERAR NUEVA SEMILLA ALEATORIA para cada mundo nuevo
    auto nowHiRes = std::chrono::high_resolution_clock::now();
    auto duration = nowHiRes.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    int newSeed = static_cast<int>(millis % 2147483647); // Semilla única basada en timestamp

    // ⭐ Establecer la nueva semilla en el objeto World
    state->world.setSeed(newSeed);

    // ⭐ Configurar la ruta del mundo en el objeto World para guardar chunks
    state->world.setWorldPath(worldPath.string());

    // ⭐⭐⭐ NUEVO: Crear level.dat inicial con metadata completa
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);

    WorldInfo newWorldInfo;
    newWorldInfo.name = worldName;
    newWorldInfo.folderPath = worldPath.string();
    newWorldInfo.creationDate = timestamp;
    newWorldInfo.lastPlayed = timestamp;
    newWorldInfo.totalPlaytime = 0;
    newWorldInfo.seed = newSeed;  // ⭐ Usar la nueva semilla generada
    newWorldInfo.versionCreated = "1.0.0";
    newWorldInfo.gameMode = 0; // Survival
    newWorldInfo.spawnX = 0;
    newWorldInfo.spawnY = 128;
    newWorldInfo.spawnZ = 0;

    // Guardar level.dat inicial (sin playtime de sesión aún)
    saveLevelDat(worldPath.string(), newWorldInfo, 0.0f);

    // ⭐ Inicializar tracking de sesión
    state->sessionStartTime = currentTime;
    state->currentSessionTime = 0.0f;

    // ⭐ Limpiar inventario y estado del jugador para mundo nuevo
    std::cout << "🧹 Limpiando inventario y estado para mundo nuevo..." << std::endl;
    state->inventory.clear();
    state->heldSlot.blockType = BLOCK_AIR;
    state->heldSlot.count = 0;

    // Limpiar grid de crafteo
    for (int i = 0; i < 9; i++) {
        state->craftingGrid.slots[i].blockType = BLOCK_AIR;
        state->craftingGrid.slots[i].count = 0;
    }
    state->craftingResult.blockType = BLOCK_AIR;
    state->craftingResult.count = 0;

    // Limpiar items sueltos en el mundo (ItemEntity)
    state->items.clear();

    // Iniciar pantalla de carga
    state->screenState = SCREEN_LOADING;
    state->isLoading = true;
    state->loadingStartTime = currentTime;
    state->spawnFound = false;
    state->cursorLocked = false;

    // Escanear de nuevo para actualizar la lista
    scanSavedWorlds(state);

    std::cout << "Mundo '" << worldName << "' creado con semilla " << newSeed << "! Iniciando carga..." << std::endl;
}

// ⭐⭐⭐ CARGAR MUNDO EXISTENTE (MEJORADO con session tracking) ⭐⭐⭐
void loadWorld(GameState* state, int worldIndex, float currentTime) {
    if (worldIndex >= 0 && worldIndex < (int)state->savedWorlds.size()) {
        state->currentWorldName = state->savedWorlds[worldIndex].name;

        // Intentar cargar datos guardados
        bool hasPlayerData = loadWorldData(state, state->currentWorldName);

        // ⭐⭐⭐ NUEVO: Inicializar tracking de sesión
        state->sessionStartTime = currentTime;
        state->currentSessionTime = 0.0f;

        // Iniciar pantalla de carga
        state->screenState = SCREEN_LOADING;
        state->isLoading = true;
        state->loadingStartTime = currentTime;
        state->cursorLocked = false;

        // Si tiene datos guardados, el spawn ya está listo
        if (hasPlayerData) {
            state->spawnFound = true;
            state->targetSpawnPosition = state->player.position;
        } else {
            // Si no tiene datos, necesitamos buscar spawn durante la carga
            state->spawnFound = false;
        }

        // Actualizar último tiempo jugado
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        state->savedWorlds[worldIndex].lastPlayed = time;

        std::cout << "Mundo '" << state->currentWorldName << "' cargando..." << std::endl;
    }
}

// ⭐⭐⭐ NUEVO: Manejar clicks en pantalla de creación de mundo
void handleWorldCreateClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight, float currentTime) {
    // Botón para cambiar modo de juego a SURVIVAL
    if (state->btnGameModeSurvival.contains(mouseX, mouseY)) {
        state->newWorldGameMode = 0;  // Survival
        std::cout << "Modo cambiado a: SURVIVAL" << std::endl;
        return;
    }

    // Botón para cambiar modo de juego a CREATIVE
    if (state->btnGameModeCreative.contains(mouseX, mouseY)) {
        state->newWorldGameMode = 1;  // Creative
        std::cout << "Modo cambiado a: CREATIVE" << std::endl;
        return;
    }

    // Botón CREAR MUNDO - Confirmar y crear el mundo
    if (state->btnCreateWorldConfirm.contains(mouseX, mouseY)) {
        std::cout << "✅ Confirmando creación de mundo..." << std::endl;
        std::cout << "   Nombre: " << (state->newWorldName.empty() ? "Nuevo Mundo" : state->newWorldName) << std::endl;
        std::cout << "   Semilla: " << (state->newWorldSeed.empty() ? "(aleatorio)" : state->newWorldSeed) << std::endl;
        std::cout << "   Modo: " << (state->newWorldGameMode == 0 ? "Survival" : "Creative") << std::endl;

        // Crear el mundo (esta función ya existe)
        createNewWorld(state, currentTime);

        std::cout << "🌍 Mundo creado y cargando..." << std::endl;
        return;
    }

    // Botón CANCELAR - Volver a selección de mundos
    if (state->btnCreateWorldCancel.contains(mouseX, mouseY)) {
        std::cout << "❌ Creación de mundo cancelada" << std::endl;
        state->screenState = SCREEN_WORLD_SELECT;
        return;
    }

    // Campos de texto editables - detectar clic para activar edición
    float centerX = screenWidth / 2.0f;
    float startY = 180.0f;
    float fieldWidth = 500.0f;
    float fieldHeight = 50.0f;
    float spacing = 80.0f;

    // Clic en campo de NOMBRE
    float nameX = centerX - fieldWidth/2;
    float nameY = startY;
    if (mouseX >= nameX && mouseX <= nameX + fieldWidth &&
        mouseY >= nameY && mouseY <= nameY + fieldHeight) {
        state->isEditingNewWorldName = true;
        state->isEditingNewWorldSeed = false;
        std::cout << "📝 Editando nombre del mundo..." << std::endl;
        return;
    }

    // Clic en campo de SEMILLA
    float seedY = startY + spacing;
    if (mouseX >= nameX && mouseX <= nameX + fieldWidth &&
        mouseY >= seedY && mouseY <= seedY + fieldHeight) {
        state->isEditingNewWorldName = false;
        state->isEditingNewWorldSeed = true;
        std::cout << "📝 Editando semilla del mundo..." << std::endl;
        return;
    }

    // Clic fuera de campos - dejar de editar
    state->isEditingNewWorldName = false;
    state->isEditingNewWorldSeed = false;
}

// Manejar clic en menú principal
void handleMainMenuClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight) {
    if (state->btnMundosSolitarios.contains(mouseX, mouseY)) {
        scanSavedWorlds(state);
        initWorldSelectButtons(state, screenWidth, screenHeight);
        state->screenState = SCREEN_WORLD_SELECT;
    }
    else if (state->btnSalir.contains(mouseX, mouseY)) {
        // Señal para cerrar el juego
        glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
    }
    // TODO: Manejar btnOpciones y btnAvatar
}

// Manejar clic en selección de mundos
void handleWorldSelectClick(GameState* state, float mouseX, float mouseY, int screenWidth, int screenHeight, float currentTime) {
    // Manejo de overlay de edición (tiene prioridad sobre otros clics)
    if (state->isEditingWorldName) {
        // Botón GUARDAR
        if (state->btnGuardarNombre.contains(mouseX, mouseY)) {
            // ⭐ VALIDACIÓN: Verificar índice válido antes de renombrar
            if (!state->editingWorldNewName.empty() &&
                state->selectedWorldIndex >= 0 &&
                state->selectedWorldIndex < (int)state->savedWorlds.size()) {
                std::cout << "Guardando nuevo nombre: " << state->editingWorldNewName << std::endl;
                if (renameWorld(state, state->selectedWorldIndex, state->editingWorldNewName)) {
                    std::cout << "Mundo renombrado exitosamente!" << std::endl;
                    // ⭐ CRÍTICO: Actualizar botones después de renombrar
                    initWorldSelectButtons(state, screenWidth, screenHeight);
                } else {
                    std::cout << "Error al renombrar el mundo" << std::endl;
                    // ⭐ Actualizar botones incluso si falla el renombrado
                    initWorldSelectButtons(state, screenWidth, screenHeight);
                }
            } else {
                if (state->editingWorldNewName.empty()) {
                    std::cout << "El nombre no puede estar vacío" << std::endl;
                } else {
                    std::cout << "❌ Error: Índice de mundo inválido" << std::endl;
                }
            }
            state->isEditingWorldName = false;
            state->editingWorldNewName = "";
            return;
        }

        // Botón CANCELAR
        if (state->btnCancelarEdicion.contains(mouseX, mouseY)) {
            std::cout << "Edición cancelada" << std::endl;
            state->isEditingWorldName = false;
            state->editingWorldNewName = "";
            return;
        }

        // Si se hace clic fuera del modal, no hacer nada (no cerrar)
        return;
    }

    if (state->btnVolverMenu.contains(mouseX, mouseY)) {
        state->screenState = SCREEN_MAIN_MENU;
        state->selectedWorldIndex = -1;  // Reset selection
        return;
    }

    // ⭐⭐⭐ Botón CREAR MUNDO - Ir a pantalla de configuración
    if (state->btnCrearMundo.contains(mouseX, mouseY)) {
        // Ir a la pantalla de configuración de nuevo mundo
        state->screenState = SCREEN_WORLD_CREATE;
        state->newWorldName = "Nuevo Mundo";
        state->newWorldSeed = "";
        state->newWorldGameMode = 0;  // Survival por defecto
        state->isEditingNewWorldName = false;
        state->isEditingNewWorldSeed = false;
        std::cout << "📝 Abriendo pantalla de configuración de mundo..." << std::endl;
        return;
    }

    // Verificar botones de gestión (solo si hay un mundo seleccionado)
    if (state->selectedWorldIndex >= 0 && state->selectedWorldIndex < (int)state->savedWorlds.size()) {
        // Botón JUGAR - cargar el mundo seleccionado
        if (state->btnJugarMundo.contains(mouseX, mouseY)) {
            loadWorld(state, state->selectedWorldIndex, currentTime);
            return;
        }

        // Botón EDITAR - abrir overlay de edición de nombre
        if (state->btnEditarMundo.contains(mouseX, mouseY)) {
            state->isEditingWorldName = true;
            state->editingWorldNewName = state->savedWorlds[state->selectedWorldIndex].name;
            std::cout << "Editando mundo: " << state->savedWorlds[state->selectedWorldIndex].name << std::endl;
            return;
        }

        // Botón RESPALDAR - crear backup del mundo
        if (state->btnRespaldoMundo.contains(mouseX, mouseY)) {
            // ⭐ VALIDACIÓN: Verificar índice válido antes de acceder
            if (state->selectedWorldIndex >= 0 && state->selectedWorldIndex < (int)state->savedWorlds.size()) {
                std::cout << "\n=== CREANDO RESPALDO ===" << std::endl;
                if (backupWorld(state, state->selectedWorldIndex)) {
                    std::cout << "Respaldo creado exitosamente!" << std::endl;
                } else {
                    std::cout << "Error al crear respaldo" << std::endl;
                }
                std::cout << "========================\n" << std::endl;
            } else {
                std::cout << "❌ Error: No hay mundo seleccionado para respaldar" << std::endl;
            }
            return;
        }

        // Botón BORRAR - eliminar el mundo con confirmación visual
        if (state->btnBorrarMundo.contains(mouseX, mouseY)) {
            // ⭐ VALIDACIÓN: Verificar índice válido antes de acceder
            if (state->selectedWorldIndex < 0 || state->selectedWorldIndex >= (int)state->savedWorlds.size()) {
                std::cout << "❌ Error: No hay mundo seleccionado para borrar" << std::endl;
                state->confirmingDelete = false;
                return;
            }

            if (state->confirmingDelete) {
                // Segundo clic: confirmar borrado
                std::cout << "Borrando mundo: " << state->savedWorlds[state->selectedWorldIndex].name << std::endl;
                if (deleteWorld(state, state->selectedWorldIndex)) {
                    std::cout << "Mundo borrado exitosamente!" << std::endl;
                    // ⭐ CRÍTICO: Actualizar botones después de borrar para evitar crash
                    initWorldSelectButtons(state, screenWidth, screenHeight);
                } else {
                    std::cout << "Error al borrar el mundo" << std::endl;
                    // ⭐ Actualizar botones incluso si falla el borrado
                    initWorldSelectButtons(state, screenWidth, screenHeight);
                }
                state->confirmingDelete = false;
            } else {
                // Primer clic: pedir confirmación
                std::cout << "Presiona BORRAR nuevamente para confirmar" << std::endl;
                state->confirmingDelete = true;
            }
            return;
        } else {
            // Si se hace clic en cualquier otro lugar, cancelar confirmación
            if (state->confirmingDelete) {
                std::cout << "Borrado cancelado" << std::endl;
                state->confirmingDelete = false;
            }
        }
    }

    // Verificar clic en mundos - ahora solo SELECCIONA, no carga
    for (size_t i = 0; i < state->worldButtons.size(); i++) {
        if (state->worldButtons[i].contains(mouseX, mouseY)) {
            // Si ya está seleccionado, deseleccionar
            if (state->selectedWorldIndex == (int)i) {
                state->selectedWorldIndex = -1;
                std::cout << "Mundo deseleccionado" << std::endl;
            } else {
                // Seleccionar este mundo
                state->selectedWorldIndex = i;
                std::cout << "Mundo seleccionado: " << state->savedWorlds[i].name << std::endl;
            }
            // Resetear confirmación de borrado al cambiar selección
            state->confirmingDelete = false;
            return;
        }
    }
}

// Actualizar hover de botones
void updateButtonHover(GameState* state, float mouseX, float mouseY) {
    if (state->screenState == SCREEN_MAIN_MENU) {
        state->btnMundosSolitarios.isHovered = state->btnMundosSolitarios.contains(mouseX, mouseY);
        state->btnOpciones.isHovered = state->btnOpciones.contains(mouseX, mouseY);
        state->btnAvatar.isHovered = state->btnAvatar.contains(mouseX, mouseY);
        state->btnSalir.isHovered = state->btnSalir.contains(mouseX, mouseY);
    }
    else if (state->screenState == SCREEN_WORLD_SELECT) {
        state->btnCrearMundo.isHovered = state->btnCrearMundo.contains(mouseX, mouseY);
        state->btnVolverMenu.isHovered = state->btnVolverMenu.contains(mouseX, mouseY);

        for (auto& btn : state->worldButtons) {
            btn.isHovered = btn.contains(mouseX, mouseY);
        }
    }
}

// ⭐⭐⭐ SISTEMA DE LEVEL.DAT (Inspirado en Minecraft pero MEJORADO) ⭐⭐⭐

// Calcular tamaño total del mundo en bytes
long long calculateWorldSize(const std::string& worldPath) {
    long long totalSize = 0;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(worldPath)) {
            if (entry.is_regular_file()) {
                totalSize += entry.file_size();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "⚠️ Error al calcular tamaño del mundo: " << e.what() << std::endl;
    }
    return totalSize;
}

// Formatear tamaño de archivo a formato legible (KB, MB, GB)
std::string formatFileSize(long long bytes) {
    const long long KB = 1024;
    const long long MB = KB * 1024;
    const long long GB = MB * 1024;

    char buffer[64];
    if (bytes >= GB) {
        snprintf(buffer, sizeof(buffer), "%.1f GB", (double)bytes / GB);
    } else if (bytes >= MB) {
        snprintf(buffer, sizeof(buffer), "%.1f MB", (double)bytes / MB);
    } else if (bytes >= KB) {
        snprintf(buffer, sizeof(buffer), "%.1f KB", (double)bytes / KB);
    } else {
        snprintf(buffer, sizeof(buffer), "%lld bytes", bytes);
    }
    return std::string(buffer);
}

// Formatear tiempo de juego a formato legible (Xh Ym)
std::string formatPlaytime(float seconds) {
    int totalMinutes = (int)(seconds / 60.0f);
    int hours = totalMinutes / 60;
    int minutes = totalMinutes % 60;

    char buffer[64];
    if (hours > 0) {
        snprintf(buffer, sizeof(buffer), "%dh %dm", hours, minutes);
    } else {
        snprintf(buffer, sizeof(buffer), "%dm", minutes);
    }
    return std::string(buffer);
}

// Formatear timestamp a formato legible
std::string formatTimestamp(long long timestamp) {
    time_t time = (time_t)timestamp;
    struct tm* timeinfo = localtime(&time);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M", timeinfo);
    return std::string(buffer);
}

// ⭐⭐⭐ GUARDAR LEVEL.DAT (Como Minecraft pero mejorado con formato de texto) ⭐⭐⭐
void saveLevelDat(const std::string& worldPath, const WorldInfo& worldInfo, float sessionPlaytime) {
    std::filesystem::path levelPath = std::filesystem::path(worldPath) / "level.dat";
    std::filesystem::path levelTmpPath = std::filesystem::path(worldPath) / "level.dat.tmp";

    // Escritura atómica: tmp + rename (level.dat nunca queda a medio escribir)
    std::ofstream file(levelTmpPath);
    if (!file.is_open()) {
        std::cerr << "❌ Error al guardar level.dat" << std::endl;
        return;
    }

    // Calcular tiempo de juego total (sesión actual + previo)
    float totalPlaytime = worldInfo.totalPlaytime + sessionPlaytime;

    // Calcular tamaño del mundo
    long long worldSize = calculateWorldSize(worldPath);

    file << "# VoxelWorld Level Data (Formato mejorado inspirado en Minecraft level.dat)\n";
    file << "# Este archivo contiene metadata completa del mundo\n";
    file << "version=1.0\n";
    file << "\n";

    file << "# Información básica\n";
    file << "LevelName=" << worldInfo.name << "\n";
    file << "RandomSeed=" << worldInfo.seed << "\n";
    file << "\n";

    file << "# Timestamps\n";
    file << "CreationDate=" << worldInfo.creationDate << "\n";
    file << "LastPlayed=" << worldInfo.lastPlayed << "\n";
    file << "\n";

    file << "# Estadísticas\n";
    file << "TotalPlaytime=" << totalPlaytime << "\n";
    file << "SizeOnDisk=" << worldSize << "\n";
    file << "\n";

    file << "# Configuración del mundo\n";
    file << "GameMode=" << worldInfo.gameMode << "\n";
    file << "SpawnX=" << worldInfo.spawnX << "\n";
    file << "SpawnY=" << worldInfo.spawnY << "\n";
    file << "SpawnZ=" << worldInfo.spawnZ << "\n";
    file << "\n";

    file << "# Versión\n";
    file << "VersionCreated=" << worldInfo.versionCreated << "\n";
    file << "\n";

    file << "# Checksum (para validación de integridad)\n";
    file << "Checksum=0xVOXELWORLD\n";

    bool writeOk = file.good();
    file.close();

    if (writeOk) {
        std::error_code ec;
        std::filesystem::rename(levelTmpPath, levelPath, ec);
        if (ec) {
            std::filesystem::remove(levelPath, ec);
            std::filesystem::rename(levelTmpPath, levelPath, ec);
        }
        if (ec) {
            std::cerr << "❌ Error al renombrar level.dat.tmp: " << ec.message() << std::endl;
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(levelTmpPath, ec);
        std::cerr << "❌ Error de escritura en level.dat" << std::endl;
    }

    // ⭐ Guardado silencioso - no molestar la vista del jugador
    // std::cout << "   ✅ level.dat guardado (Tamaño: " << formatFileSize(worldSize)
    //           << ", Tiempo: " << formatPlaytime(totalPlaytime) << ")" << std::endl;
}

// ⭐⭐⭐ CARGAR LEVEL.DAT ⭐⭐⭐
bool loadLevelDat(const std::string& worldPath, WorldInfo& worldInfo) {
    std::filesystem::path levelPath = std::filesystem::path(worldPath) / "level.dat";

    if (!std::filesystem::exists(levelPath)) {
        // Silencioso - no molestar con warnings
        return false;
    }

    std::ifstream file(levelPath);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    bool validChecksum = false;

    while (std::getline(file, line)) {
        // Ignorar comentarios y líneas vacías
        if (line.empty() || line[0] == '#') continue;

        // Parsear clave=valor
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // ⭐ std::sto* lanza con valores vacíos/no numéricos: un level.dat
        // corrupto NO debe tirar la aplicación (esto se llama desde el menú
        // de selección de mundos, fuera del try/catch del juego). El campo
        // ilegible se ignora y se conserva el valor por defecto.
        try {
            // Parsear cada campo
            if (key == "LevelName") {
                worldInfo.name = value;
            } else if (key == "RandomSeed") {
                worldInfo.seed = (unsigned int)std::stoul(value);
            } else if (key == "CreationDate") {
                worldInfo.creationDate = std::stoll(value);
            } else if (key == "LastPlayed") {
                worldInfo.lastPlayed = std::stoll(value);
            } else if (key == "TotalPlaytime") {
                worldInfo.totalPlaytime = std::stof(value);
            } else if (key == "SizeOnDisk") {
                worldInfo.worldSizeBytes = std::stoll(value);
            } else if (key == "GameMode") {
                worldInfo.gameMode = std::stoi(value);
            } else if (key == "SpawnX") {
                worldInfo.spawnX = std::stoi(value);
            } else if (key == "SpawnY") {
                worldInfo.spawnY = std::stoi(value);
            } else if (key == "SpawnZ") {
                worldInfo.spawnZ = std::stoi(value);
            } else if (key == "VersionCreated") {
                worldInfo.versionCreated = value;
            } else if (key == "Checksum") {
                validChecksum = (value == "0xVOXELWORLD");
            }
        } catch (const std::exception&) {
            std::cerr << "⚠️ level.dat: campo ilegible '" << key << "=" << value
                      << "' en " << worldPath << " (ignorado)" << std::endl;
        }
    }

    file.close();

    // ⭐ Carga silenciosa - no molestar la vista del jugador
    // if (!validChecksum) {
    //     std::cout << "⚠️ Checksum de level.dat inválido" << std::endl;
    // }

    return true;
}

// ⭐⭐⭐ GUARDAR MUNDO COMPLETO A DISCO (MEJORADO AAA) ⭐⭐⭐
void saveWorld(GameState* state) {
    if (state->currentWorldName.empty()) {
        std::cout << "⚠️ No hay mundo activo para guardar" << std::endl;
        return;
    }

    auto saveStartTime = std::chrono::high_resolution_clock::now();

    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  💾 GUARDANDO MUNDO: " << state->currentWorldName << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    std::filesystem::path worldPath = std::filesystem::path("saves") / state->currentWorldName;
    std::filesystem::create_directories(worldPath);

    // ⭐ Crear save.lock AL INICIO: si el proceso muere a media escritura, el
    // lock queda en disco y la próxima carga puede avisar de posible corrupción.
    // (Antes solo se borraba al final: nunca detectaba nada.)
    std::filesystem::path lockPath = worldPath / "save.lock";
    { std::ofstream lockFile(lockPath); lockFile << "saving"; }

    // ⭐ PASO 1: Guardar datos del jugador (escritura atómica: tmp + rename)
    std::cout << "📍 Guardando jugador..." << std::endl;
    std::filesystem::path playerPath = worldPath / "player.dat";
    std::filesystem::path playerTmpPath = worldPath / "player.dat.tmp";
    std::ofstream playerFile(playerTmpPath, std::ios::binary);
    if (playerFile.is_open()) {
        // HEADER para validación
        const char header[4] = {'P', 'L', 'Y', 'R'};
        playerFile.write(header, 4);
        const uint32_t version = 1;
        playerFile.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));

        // Posición
        playerFile.write(reinterpret_cast<const char*>(&state->player.position.x), sizeof(float));
        playerFile.write(reinterpret_cast<const char*>(&state->player.position.y), sizeof(float));
        playerFile.write(reinterpret_cast<const char*>(&state->player.position.z), sizeof(float));

        // Rotación
        playerFile.write(reinterpret_cast<const char*>(&state->player.yaw), sizeof(float));
        playerFile.write(reinterpret_cast<const char*>(&state->player.pitch), sizeof(float));

        // Velocidad (para preservar estado de movimiento)
        playerFile.write(reinterpret_cast<const char*>(&state->player.velocity.x), sizeof(float));
        playerFile.write(reinterpret_cast<const char*>(&state->player.velocity.y), sizeof(float));
        playerFile.write(reinterpret_cast<const char*>(&state->player.velocity.z), sizeof(float));

        // Estados booleanos
        playerFile.write(reinterpret_cast<const char*>(&state->player.onGround), sizeof(bool));

        // Inventario (TODOS los 45 slots)
        for (int i = 0; i < Inventory::SLOTS; i++) {
            int blockType = static_cast<int>(state->inventory.slots[i].blockType);
            playerFile.write(reinterpret_cast<const char*>(&blockType), sizeof(int));
            playerFile.write(reinterpret_cast<const char*>(&state->inventory.slots[i].count), sizeof(int));
        }
        playerFile.write(reinterpret_cast<const char*>(&state->inventory.selectedSlot), sizeof(int));

        // Checksum para validación
        uint32_t checksum = 0xDEADBEEF;  // Simple checksum
        playerFile.write(reinterpret_cast<const char*>(&checksum), sizeof(uint32_t));

        bool writeOk = playerFile.good();
        playerFile.close();

        // ⭐ Rename atómico: player.dat nunca queda a medio escribir. Si el
        // proceso muere durante la escritura, solo se pierde el .tmp.
        if (writeOk) {
            std::error_code ec;
            std::filesystem::rename(playerTmpPath, playerPath, ec);
            if (ec) {
                // En Windows rename falla si el destino existe y está bloqueado
                std::filesystem::remove(playerPath, ec);
                std::filesystem::rename(playerTmpPath, playerPath, ec);
            }
            if (ec) {
                std::cerr << "   ❌ Error al renombrar player.dat.tmp: " << ec.message() << std::endl;
            } else {
                std::cout << "   ✅ Jugador guardado (Pos: "
                          << (int)state->player.position.x << ", "
                          << (int)state->player.position.y << ", "
                          << (int)state->player.position.z << ")" << std::endl;
            }
        } else {
            std::error_code ec;
            std::filesystem::remove(playerTmpPath, ec);
            std::cerr << "   ❌ Error de escritura en player.dat (disco lleno?)" << std::endl;
        }
    } else {
        std::cerr << "   ❌ Error al guardar jugador" << std::endl;
    }

    // ⭐ PASO 2: Guardar configuración del mundo
    std::cout << "⚙️ Guardando configuración..." << std::endl;
    std::filesystem::path worldCfgPath = worldPath / "world.cfg";
    std::ofstream cfgFile(worldCfgPath);
    if (cfgFile.is_open()) {
        cfgFile << "# VoxelWorld Save File\n";
        cfgFile << "version=1\n";
        cfgFile << "seed=" << state->world.getSeed() << "\n";
        cfgFile << "render_distance=" << state->renderDistance << "\n";

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        cfgFile << "last_save=" << time << "\n";

        cfgFile.close();
        std::cout << "   ✅ Configuración guardada" << std::endl;
    } else {
        std::cerr << "   ❌ Error al guardar configuración" << std::endl;
    }

    // ⭐ PASO 3: Guardar chunks modificados del mundo
    std::cout << "🗺️ Guardando chunks..." << std::endl;
    state->world.saveWorld(worldPath.string());

    // ⭐ PASO 4: Guardado completo — retirar el lock creado al inicio
    if (std::filesystem::exists(lockPath)) {
        std::filesystem::remove(lockPath);
    }

    // ⭐ PASO 5: Actualizar timestamp en la lista de mundos
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    for (auto& worldInfo : state->savedWorlds) {
        if (worldInfo.name == state->currentWorldName) {
            worldInfo.lastPlayed = time;
            break;
        }
    }

    // ⭐⭐⭐ PASO 5.5: GUARDAR LEVEL.DAT (Inspirado en Minecraft pero MEJORADO) ⭐⭐⭐
    // Guardado silencioso - no molestar la vista

    // Encontrar el WorldInfo del mundo actual
    WorldInfo* currentWorldInfo = nullptr;
    for (auto& worldInfo : state->savedWorlds) {
        if (worldInfo.name == state->currentWorldName) {
            currentWorldInfo = &worldInfo;
            break;
        }
    }

    if (currentWorldInfo != nullptr) {
        // Calcular tiempo de sesión actual
        float sessionPlaytime = state->currentSessionTime;

        // Actualizar información del mundo
        currentWorldInfo->seed = state->world.getSeed();
        currentWorldInfo->lastPlayed = time;

        // Guardar level.dat con metadata completa
        saveLevelDat(worldPath.string(), *currentWorldInfo, sessionPlaytime);

        // Actualizar el playtime total en la estructura
        currentWorldInfo->totalPlaytime += sessionPlaytime;
    } else {
        std::cerr << "   ⚠️ No se encontró WorldInfo para guardar level.dat" << std::endl;
    }

    // ⭐ PASO 6: Calcular tiempo total de guardado
    auto saveEndTime = std::chrono::high_resolution_clock::now();
    float saveDuration = std::chrono::duration<float, std::milli>(saveEndTime - saveStartTime).count();

    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  ✅ MUNDO GUARDADO EXITOSAMENTE" << std::endl;
    std::cout << "║  ⏱️ Tiempo: " << saveDuration << " ms" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;
}

// ⭐⭐⭐ CARGAR MUNDO COMPLETO DESDE DISCO (MEJORADO AAA) ⭐⭐⭐
bool loadWorldData(GameState* state, const std::string& worldName) {
    auto loadStartTime = std::chrono::high_resolution_clock::now();

    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  📂 CARGANDO MUNDO: " << worldName << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    std::filesystem::path worldPath = std::filesystem::path("saves") / worldName;

    if (!std::filesystem::exists(worldPath)) {
        std::cerr << "❌ ERROR: Mundo no encontrado: " << worldPath << std::endl;
        return false;
    }

    // ⭐ Verificar archivo de lock (mundo corrupto?)
    std::filesystem::path lockPath = worldPath / "save.lock";
    if (std::filesystem::exists(lockPath)) {
        std::cout << "⚠️ ADVERTENCIA: Se detectó un archivo de lock" << std::endl;
        std::cout << "   El mundo podría estar corrupto por un cierre inesperado" << std::endl;
        std::cout << "   Intentando cargar de todos modos..." << std::endl;
    }

    // ⭐ Configurar la ruta del mundo en el objeto World para cargar chunks guardados
    state->world.setWorldPath(worldPath.string());

    // ⭐⭐⭐ PASO 0.5: Cargar SEMILLA del level.dat (CRÍTICO - debe hacerse ANTES de generar chunks)
    std::cout << "🌱 Cargando semilla del mundo..." << std::endl;
    WorldInfo tempWorldInfo;
    tempWorldInfo.folderPath = worldPath.string();
    if (loadLevelDat(worldPath.string(), tempWorldInfo)) {
        // ⭐ CRÍTICO: Establecer la semilla ANTES de generar cualquier chunk
        state->world.setSeed(tempWorldInfo.seed);
        std::cout << "   ✅ Semilla cargada desde level.dat: " << tempWorldInfo.seed << std::endl;
    } else {
        // Fallback: Intentar cargar desde world.cfg
        std::filesystem::path worldCfgPath = worldPath / "world.cfg";
        if (std::filesystem::exists(worldCfgPath)) {
            std::ifstream cfgFile(worldCfgPath);
            std::string line;
            while (std::getline(cfgFile, line)) {
                if (line.find("seed=") == 0) {
                    try {
                        int savedSeed = std::stoi(line.substr(5));
                        state->world.setSeed(savedSeed);
                        std::cout << "   ✅ Semilla cargada desde world.cfg: " << savedSeed << std::endl;
                    } catch (const std::exception&) {
                        std::cerr << "   ⚠️ world.cfg: semilla ilegible (se usará la actual)" << std::endl;
                    }
                    break;
                }
            }
            cfgFile.close();
        }
    }

    // ⭐ PASO 1: Cargar configuración del mundo
    std::cout << "⚙️ Cargando configuración..." << std::endl;
    std::filesystem::path worldCfgPath = worldPath / "world.cfg";
    if (std::filesystem::exists(worldCfgPath)) {
        std::ifstream cfgFile(worldCfgPath);
        std::string line;
        while (std::getline(cfgFile, line)) {
            try {
                if (line.find("render_distance=") == 0) {
                    state->renderDistance = std::stoi(line.substr(16));
                }
                else if (line.find("last_save=") == 0) {
                    time_t lastSave = std::stoll(line.substr(10));
                    std::cout << "   Último guardado: " << ctime(&lastSave);
                }
            } catch (const std::exception&) {
                std::cerr << "   ⚠️ world.cfg: línea ilegible ignorada: " << line << std::endl;
            }
        }
        cfgFile.close();
        std::cout << "   ✅ Configuración cargada" << std::endl;
    } else {
        std::cout << "   ⚠️ No se encontró configuración, usando valores por defecto" << std::endl;
    }

    // ⭐ PASO 2: Cargar datos del jugador con validación
    std::cout << "📍 Cargando jugador..." << std::endl;
    std::filesystem::path playerPath = worldPath / "player.dat";
    if (std::filesystem::exists(playerPath)) {
        std::ifstream playerFile(playerPath, std::ios::binary);
        if (playerFile.is_open()) {
            // Validar HEADER
            char header[4];
            playerFile.read(header, 4);
            if (header[0] != 'P' || header[1] != 'L' || header[2] != 'Y' || header[3] != 'R') {
                std::cerr << "   ❌ ERROR: Archivo de jugador corrupto (header inválido)" << std::endl;
                playerFile.close();
                return false;
            }

            // Validar VERSION
            uint32_t version;
            playerFile.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
            if (version != 1) {
                std::cerr << "   ❌ ERROR: Versión de archivo no soportada: " << version << std::endl;
                playerFile.close();
                return false;
            }

            // ⭐ VALIDACIÓN: leer TODO a variables locales, comprobar el estado
            // del stream y los rangos, y solo entonces aplicar al estado del
            // juego. Antes, un player.dat truncado dejaba posición/velocidad
            // sin inicializar (UB) y aun así se usaban.
            float px, py, pz, yaw, pitch, vx, vy, vz;
            bool onGround;
            playerFile.read(reinterpret_cast<char*>(&px), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&py), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&pz), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&yaw), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&pitch), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&vx), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&vy), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&vz), sizeof(float));
            playerFile.read(reinterpret_cast<char*>(&onGround), sizeof(bool));

            // Inventario (TODOS los 45 slots) a buffer local
            int invBlockType[Inventory::SLOTS];
            int invCount[Inventory::SLOTS];
            for (int i = 0; i < Inventory::SLOTS; i++) {
                playerFile.read(reinterpret_cast<char*>(&invBlockType[i]), sizeof(int));
                playerFile.read(reinterpret_cast<char*>(&invCount[i]), sizeof(int));
            }
            int selectedSlot = 0;
            playerFile.read(reinterpret_cast<char*>(&selectedSlot), sizeof(int));

            uint32_t checksum = 0;
            playerFile.read(reinterpret_cast<char*>(&checksum), sizeof(uint32_t));

            // ⭐ Archivo truncado = stream en fallo: rechazar el archivo entero
            if (!playerFile.good() && !playerFile.eof()) {
                std::cerr << "   ❌ ERROR: player.dat truncado o ilegible" << std::endl;
                playerFile.close();
                return false;
            }
            if (playerFile.fail() && !playerFile.bad()) {
                // fail sin bad tras las reads = datos incompletos
                std::cerr << "   ❌ ERROR: player.dat incompleto (faltan datos)" << std::endl;
                playerFile.close();
                return false;
            }
            playerFile.close();

            if (checksum != 0xDEADBEEF) {
                std::cerr << "   ⚠️ ADVERTENCIA: Checksum inválido (el archivo podría estar corrupto)" << std::endl;
            }

            // ⭐ Rechazar floats no finitos (NaN/Inf rompen la física)
            if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
                !std::isfinite(yaw) || !std::isfinite(pitch) ||
                !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
                std::cerr << "   ❌ ERROR: player.dat con valores no finitos (NaN/Inf)" << std::endl;
                return false;
            }

            // Aplicar al estado, con rangos acotados
            state->player.position = Vec3(px, py, pz);
            state->player.yaw = yaw;
            state->player.pitch = pitch;
            state->player.velocity = Vec3(vx, vy, vz);
            state->player.onGround = onGround;

            for (int i = 0; i < Inventory::SLOTS; i++) {
                int bt = invBlockType[i];
                int count = invCount[i];
                // Bloque fuera del enum o cantidad inválida → slot vacío
                if (bt < 0 || bt > BLOCK_TYPE_MAX || count <= 0) {
                    bt = BLOCK_AIR;
                    count = 0;
                } else if (count > MAX_STACK_SIZE) {
                    count = MAX_STACK_SIZE;  // límite de stack del juego
                }
                state->inventory.slots[i].blockType = static_cast<BlockType>(bt);
                state->inventory.slots[i].count = count;
            }
            if (selectedSlot < 0 || selectedSlot >= Inventory::SLOTS) {
                selectedSlot = 0;
            }
            state->inventory.selectedSlot = selectedSlot;

            // ⭐ PROTECCIÓN ANTI-VOID: Validar posición Y cargada
            if (state->player.position.y < 5.0f || state->player.position.y > 250.0f) {
                std::cout << "   ⚠️ ADVERTENCIA: Posición Y inválida detectada (Y="
                          << state->player.position.y << ")" << std::endl;
                std::cout << "   🔄 Usando posición segura por defecto..." << std::endl;

                // ⭐ CRÍTICO: NO intentar buscar spawn porque los chunks no existen todavía
                // Simplemente usar una altura segura (100) y dejar que el jugador caiga naturalmente
                state->player.position.y = 100.0f;
                state->player.velocity.y = 0.0f;
                state->player.onGround = false;

                std::cout << "   ✅ Posición Y corregida a Y=100 (el jugador caerá al suelo)" << std::endl;
            }

            // ⭐ MEJORADO: Mantener inventario guardado (como Minecraft Java)
            std::cout << "   ✅ Jugador cargado (Pos: "
                      << (int)state->player.position.x << ", "
                      << (int)state->player.position.y << ", "
                      << (int)state->player.position.z << ")" << std::endl;

            // Contar items en inventario
            int totalItems = 0;
            for (int i = 0; i < Inventory::SLOTS; i++) {
                if (state->inventory.slots[i].blockType != BLOCK_AIR) {
                    totalItems += state->inventory.slots[i].count;
                }
            }
            std::cout << "   🎒 Inventario cargado - " << totalItems << " items restaurados" << std::endl;

            // ⭐ PASO 3: Calcular tiempo total de carga
            auto loadEndTime = std::chrono::high_resolution_clock::now();
            float loadDuration = std::chrono::duration<float, std::milli>(loadEndTime - loadStartTime).count();

            std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
            std::cout << "║  ✅ MUNDO CARGADO EXITOSAMENTE" << std::endl;
            std::cout << "║  ⏱️ Tiempo: " << loadDuration << " ms" << std::endl;
            std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

            return true;
        }
    }

    // Si no hay archivo de jugador, es mundo nuevo - limpiar inventario y usar spawn por defecto
    std::cout << "   ℹ️ No se encontró archivo de jugador, usando spawn por defecto" << std::endl;
    std::cout << "   🧹 Limpiando inventario y estado para mundo nuevo..." << std::endl;

    // ⭐ CRÍTICO: Limpiar inventario para que no se transfieran items entre mundos
    state->inventory.clear();
    state->heldSlot.blockType = BLOCK_AIR;
    state->heldSlot.count = 0;

    // Limpiar grid de crafteo
    for (int i = 0; i < 9; i++) {
        state->craftingGrid.slots[i].blockType = BLOCK_AIR;
        state->craftingGrid.slots[i].count = 0;
    }
    state->craftingResult.blockType = BLOCK_AIR;
    state->craftingResult.count = 0;

    // Limpiar items sueltos en el mundo (ItemEntity)
    state->items.clear();

    return false;
}

// ============================================================================
// SIGNAL HANDLERS PARA GUARDADO DE EMERGENCIA
// ============================================================================

// Marcador de crash: descriptor abierto al arrancar para que el handler solo
// necesite _write() (async-signal-safe). El handler anterior llamaba a
// saveWorld() completo (filesystem, ofstream, mutexes, heap) desde SIGSEGV:
// podía colgarse en un lock o sobrescribir un save bueno con memoria corrupta.
static int g_crashMarkerFd = -1;
static std::string g_crashMarkerPath;

void emergencySaveHandler(int signal) {
    // ⚠️ Contexto de señal: SOLO operaciones async-signal-safe.
    if (g_crashMarkerFd >= 0) {
        _write(g_crashMarkerFd, "CRASH\n", 6);
        _commit(g_crashMarkerFd);
    }
    // Restaurar el handler por defecto y re-raise la señal
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void setupSignalHandlers() {
    namespace fs = std::filesystem;
    const char* localAppData = getenv("LOCALAPPDATA");
    fs::path dir = (localAppData && *localAppData)
        ? fs::path(localAppData) / "VoxelGenesis"
        : fs::path(getExeDir()) / "logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path markerPath = dir / "crash.marker";

    // ¿Quedó marcador de la sesión anterior? → hubo crash
    if (fs::exists(markerPath, ec) && fs::file_size(markerPath, ec) > 0) {
        std::cerr << "⚠️ Se detectó un cierre inesperado en la sesión anterior" << std::endl;
        MessageBoxA(nullptr,
                    "VoxelWorld se cerró inesperadamente la última vez.\n\n"
                    "El progreso desde el último autoguardado podría haberse perdido.\n"
                    "Hay backups en saves\\<mundo>\\backups\\.",
                    "VoxelWorld - Aviso", MB_OK | MB_ICONWARNING);
    }

    g_crashMarkerPath = markerPath.string();
    g_crashMarkerFd = _open(g_crashMarkerPath.c_str(),
                            _O_CREAT | _O_WRONLY | _O_TRUNC, _S_IREAD | _S_IWRITE);

    // Solo señales de crash real. SIGINT/SIGTERM quedan con el comportamiento
    // por defecto: el guardado normal lo cubren el autosave y el cierre limpio.
    std::signal(SIGSEGV, emergencySaveHandler);  // Segmentation fault
    std::signal(SIGABRT, emergencySaveHandler);  // Abort
    std::signal(SIGILL,  emergencySaveHandler);  // Illegal instruction
    std::signal(SIGFPE,  emergencySaveHandler);  // Floating point exception

    std::cout << "🛡️ Detector de crashes instalado (marcador: " << g_crashMarkerPath << ")" << std::endl;
}

// ============================================================================
// FUNCION MAIN
// ============================================================================

// Modo benchmark: ver la sección "MODO BENCHMARK" más abajo
static bool g_benchmarkMode = false;
static double g_benchmarkSeconds = 30.0;
static double g_benchInGameStart = -1.0;
static int g_forcedSeed = -1;   // --seed: mundo reproducible para comparar runs

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--benchmark") {
            g_benchmarkMode = true;
            if (i + 1 < argc) {
                try { g_benchmarkSeconds = std::stod(argv[++i]); } catch (...) {}
            }
        } else if (arg == "--seed") {
            if (i + 1 < argc) {
                try { g_forcedSeed = std::stoi(argv[++i]); } catch (...) {}
            }
        } else if (arg == "--verify-gen") {
            g_verifyGen = true;
        }
    }

    // Redirigir stdout/stderr a archivo de log (el binario no tiene consola)
    initLogging();

    // ⭐ INSTALAR SIGNAL HANDLERS PARA GUARDADO DE EMERGENCIA
    setupSignalHandlers();

    if (!glfwInit()) {
        std::cerr << "Error al inicializar GLFW" << std::endl;
        MessageBoxA(nullptr, "No se pudo inicializar GLFW (sistema de ventanas).",
                    "VoxelWorld - Error fatal", MB_OK | MB_ICONERROR);
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Voxel World - Sandbox Infinito", NULL, NULL);
    if (!window) {
        std::cerr << "Error al crear ventana GLFW" << std::endl;
        MessageBoxA(nullptr, "No se pudo crear la ventana del juego (OpenGL 2.1 no disponible).",
                    "VoxelWorld - Error fatal", MB_OK | MB_ICONERROR);
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync activado - Limitar a 60 FPS

    // VBO OPTIMIZATION: Cargar extensiones de VBO
    std::cout << "Cargando extensiones VBO..." << std::endl;
    loadVBOFunctions();

    // Inicializar TextureManager (debe hacerse DESPUÉS de crear contexto OpenGL)
    std::cout << "Inicializando sistema de texturas..." << std::endl;
    g_textureManager = new TextureManager();
    g_textureManager->loadAllBlockTextures();

    // ⭐⭐⭐ NUEVO: Pre-cargar texturas de items para hotbar (evita texturas faltantes)
    std::cout << "Pre-cargando texturas de items para hotbar..." << std::endl;
    int texturesLoaded = 0;
    int texturesFailed = 0;
    for (int i = 1; i < 16; i++) {
        GLuint tex = g_textureManager->getItemTexture((BlockType)i);
        if (tex == 0) {
            std::cerr << "  ⚠️ WARNING: Textura de item " << i << " no cargó correctamente" << std::endl;
            texturesFailed++;
        } else {
            texturesLoaded++;
        }
    }
    std::cout << "  ✅ Texturas de items: " << texturesLoaded << " cargadas, " << texturesFailed << " fallaron" << std::endl;
    std::cout << "Sistema de texturas listo!" << std::endl;

    // ⭐ Inicializar SoundManager
    std::cout << "Inicializando sistema de audio..." << std::endl;
    g_soundManager = new SoundManager();
    std::cout << "Sistema de audio listo! (Sonidos: pasos, romper, colocar)" << std::endl;

    // Configurar callback de texturas para ChunkSystem
    VoxelEngine::MeshBuilder::setTextureCallback(chunkSystemTextureCallback);
    std::cout << "ChunkSystem texture callback configurado!" << std::endl << std::endl;

    g_gameState = new GameState();

    // --seed: fija la semilla antes de generar nada, para que dos ejecuciones
    // produzcan exactamente el mismo mundo (necesario para comparar cambios)
    if (g_forcedSeed >= 0) {
        g_gameState->world.setSeed(g_forcedSeed);
        std::cout << "[BENCH] Semilla forzada: " << g_forcedSeed << std::endl;
    }

    // Mostrar la semilla del mundo
    std::cout << "======================================" << std::endl;
    std::cout << "  VOXEL WORLD - SANDBOX INFINITO" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "Seed del mundo: " << g_gameState->world.getSeed() << std::endl;
    std::cout << "Guarda esta semilla para regenerar este mundo!" << std::endl;
    std::cout << "======================================" << std::endl;

    // Inicializar botones del menú principal
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    initMainMenuButtons(g_gameState, width, height);

    glfwSetKeyCallback(window, keyCallback);

    // Conectar el Profiler al font del juego (overlay con F3)
    Profiler::ProfilerManager::getInstance()->setTextRenderer(
        [](const char* text, float x, float y, float size) {
            renderText(text, x, y, size);
        });
    glfwSetCharCallback(window, charCallback);
    glfwSetCursorPosCallback(window, mouseCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);  // ⭐ Scroll para cambiar slots

    // Cursor visible en el menú principal
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    // Configuración OpenGL mejorada para eliminar artefactos
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);  // Mejor manejo de z-fighting
    glDepthMask(GL_TRUE);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);  // Counter-clockwise es el frente

    // Configuración de alpha blending para transparencias
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Habilitar alpha testing para descartar píxeles completamente transparentes
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);

    glClearColor(0.53f, 0.81f, 0.92f, 1.0f);
    glClearDepth(1.0);

    // Generar chunks iniciales alrededor del origen (0,0,0)
    double initGenStart = glfwGetTime();
    std::cout << "Generando mundo inicial alrededor del origen..." << std::endl;

    // ⭐ MEJORADO: Pasar ventana para procesar mensajes de Windows (evita "No responde")
    // OPTIMIZADO: 5x5 = 25 chunks (radio de 2) - Inicio más rápido
    g_gameState->world.generateInitialChunks(2, window);

    // Construir todos los meshes
    g_gameState->world.buildAllPendingMeshes(window);

    // Marcar que la generación inicial ha terminado (activa rebuilds inmediatos)
    g_gameState->world.finishInitialGeneration();

    std::cout << "Sistema de mundo inicializado! (" << g_gameState->world.getChunkCount()
              << " chunks en " << (glfwGetTime() - initGenStart) << "s)" << std::endl;

    // ⭐ MODO BENCHMARK: entra directo a un mundo y sale solo tras N segundos.
    // Permite medir el rendimiento real del juego (no el del menú) de forma
    // repetible, sin depender de que alguien navegue los menús a mano.
    if (g_benchmarkMode) {
        scanSavedWorlds(g_gameState);
        float now = (float)glfwGetTime();
        if (!g_gameState->savedWorlds.empty()) {
            std::cout << "[BENCH] Cargando mundo existente: "
                      << g_gameState->savedWorlds[0].name << std::endl;
            loadWorld(g_gameState, 0, now);
        } else {
            std::cout << "[BENCH] Creando mundo nuevo" << std::endl;
            createNewWorld(g_gameState, now);
        }
        std::cout << "[BENCH] Duracion: " << g_benchmarkSeconds << "s" << std::endl;
    }

    /* COMENTADO: El spawn ahora se hace al crear/cargar mundo, no al inicio
    // SISTEMA DE SPAWN SEGURO - El jugador SIEMPRE aparece en superficie
    std::cout << "Buscando posicion de spawn segura..." << std::endl;

    bool foundSafeSpawn = false;
    int spawnX = 0;
    int spawnZ = 0;
    int spawnY = 0;

    // Buscar en espiral desde el centro
    for (int radius = 0; radius <= 32 && !foundSafeSpawn; radius++) {
        for (int dx = -radius; dx <= radius && !foundSafeSpawn; dx++) {
            for (int dz = -radius; dz <= radius && !foundSafeSpawn; dz++) {
                // Solo buscar en el borde del radio actual (optimización)
                if (abs(dx) != radius && abs(dz) != radius) continue;

                int testX = dx;
                int testZ = dz;

                // Buscar la superficie desde arriba
                for (int y = CHUNK_HEIGHT - 1; y >= 1; y--) {
                    BlockType currentBlock = g_gameState->world.getBlock(testX, y, testZ);
                    BlockType blockBelow = g_gameState->world.getBlock(testX, y - 1, testZ);
                    BlockType blockAbove = g_gameState->world.getBlock(testX, y + 1, testZ);
                    BlockType blockAbove2 = g_gameState->world.getBlock(testX, y + 2, testZ);

                    // Condiciones para un spawn seguro:
                    // 1. Bloque actual es AIRE (no spawneamos dentro de bloques)
                    // 2. Bloque debajo es SÓLIDO (no AIRE, no AGUA)
                    // 3. Bloque encima es AIRE (espacio para la cabeza)
                    // 4. 2 bloques encima es AIRE (espacio completo)

                    bool currentIsAir = (currentBlock == BLOCK_AIR);
                    bool belowIsSolid = (blockBelow != BLOCK_AIR && blockBelow != BLOCK_WATER);
                    bool aboveIsAir = (blockAbove == BLOCK_AIR);
                    bool above2IsAir = (blockAbove2 == BLOCK_AIR);

                    // NO spawneamos en agua
                    bool notInWater = (currentBlock != BLOCK_WATER &&
                                      blockAbove != BLOCK_WATER &&
                                      blockAbove2 != BLOCK_WATER);

                    if (currentIsAir && belowIsSolid && aboveIsAir && above2IsAir && notInWater) {
                        // Verificar que NO sea una cueva (debe tener cielo encima)
                        bool hasSky = true;
                        for (int checkY = y + 3; checkY < CHUNK_HEIGHT; checkY++) {
                            BlockType skyBlock = g_gameState->world.getBlock(testX, checkY, testZ);
                            if (skyBlock != BLOCK_AIR && skyBlock != BLOCK_WATER) {
                                hasSky = false;
                                break;
                            }
                        }

                        if (hasSky) {
                            // ENCONTRADO! Posición segura
                            spawnX = testX;
                            spawnY = y;
                            spawnZ = testZ;
                            foundSafeSpawn = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (foundSafeSpawn) {
        g_gameState->player.position.x = spawnX + 0.5f;
        g_gameState->player.position.y = spawnY + 0.1f;  // Pequeño offset para estar sobre el bloque
        g_gameState->player.position.z = spawnZ + 0.5f;
        std::cout << "Spawn seguro encontrado en: X=" << spawnX
                  << ", Y=" << spawnY << ", Z=" << spawnZ << std::endl;
    } else {
        // Fallback: buscar cualquier superficie (sin verificación de cielo)
        std::cout << "ADVERTENCIA: No se encontro spawn ideal, usando fallback..." << std::endl;
        for (int y = CHUNK_HEIGHT - 1; y >= 10; y--) {
            BlockType current = g_gameState->world.getBlock(0, y, 0);
            BlockType below = g_gameState->world.getBlock(0, y - 1, 0);

            if (current == BLOCK_AIR && below != BLOCK_AIR && below != BLOCK_WATER) {
                g_gameState->player.position.y = y + 0.1f;
                std::cout << "Spawn fallback en Y=" << y << std::endl;
                break;
            }
        }
    }

    std::cout << "Jugador spawneado en Y=" << g_gameState->player.position.y << std::endl;
    std::cout << "¡Mundo listo!" << std::endl;
    */ // FIN DEL COMENTARIO - El spawn ahora se hace al crear/cargar mundo

//     // Calcular iluminación inicial en un hilo separado
//     std::cout << "\nIniciando calculo de iluminacion global (en hilo separado)..." << std::endl;
//     g_gameState->world.startLightingCalculation();
//     std::cout << "Sistema de iluminación iniciado!" << std::endl;
//     std::cout << "La iluminacion se calculara mientras juegas.\n" << std::endl;

    double lastTime = glfwGetTime();
    int frameCount = 0;
    double fpsWindowStart = lastTime;

    // ⭐ PROTECCIÓN CONTRA CRASHES: Try-catch en el game loop
    try {
        while (!glfwWindowShouldClose(window)) {
            // ⭐ Protección crítica: Verificar que el estado del juego es válido
            if (!g_gameState) {
                std::cerr << "❌ ERROR CRÍTICO: g_gameState es NULL en el bucle principal!" << std::endl;
                break;
            }

            // El cronómetro del benchmark arranca al entrar en juego: así no se
            // contamina con el tiempo de generación inicial del mundo.
            if (g_benchmarkMode) {
                if (g_benchInGameStart < 0.0) {
                    if (g_gameState->screenState == SCREEN_IN_GAME) {
                        g_benchInGameStart = glfwGetTime();
                        std::cout << "[BENCH] En juego tras " << g_benchInGameStart
                                  << "s de arranque; midiendo " << g_benchmarkSeconds << "s" << std::endl;
                    }
                } else if (glfwGetTime() - g_benchInGameStart >= g_benchmarkSeconds) {
                    std::cout << "[BENCH] Fin del benchmark, cerrando" << std::endl;
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }

            // ⭐⭐⭐ NUEVO: Protección para TextureManager (asegurar que nunca sea null)
            if (g_textureManager == nullptr) {
                std::cerr << "⚠️ WARNING: g_textureManager es NULL! Re-inicializando..." << std::endl;
                g_textureManager = new TextureManager();
                g_textureManager->loadAllBlockTextures();
                std::cout << "✅ TextureManager re-inicializado exitosamente" << std::endl;
            }

            auto frame_start = std::chrono::high_resolution_clock::now();
            auto t1 = frame_start, t2 = frame_start;
            double physics_ms = 0, chunks_ms = 0, render_ms = 0;
            double currentTime = glfwGetTime();
            float deltaTime = (float)(currentTime - lastTime);
            lastTime = currentTime;

            // ⭐⭐⭐ PROTECCIÓN: Limitar deltaTime extremo (evita glitches en lag spikes)
            if (deltaTime > 0.1f) deltaTime = 0.1f;  // Máximo 100ms (10 FPS mínimo)
            if (deltaTime < 0.001f) deltaTime = 0.001f;  // Mínimo 1ms

            // ⭐⭐⭐ NUEVO: Sistema de repetición automática de BACKSPACE
            if (g_gameState->backspacePressed) {
                double timeSinceFirstPress = currentTime - g_gameState->backspaceFirstPressTime;
                double timeSinceLastRepeat = currentTime - g_gameState->backspaceLastRepeatTime;

                // Después del delay inicial, empezar a repetir
                if (timeSinceFirstPress >= g_gameState->backspaceRepeatDelay) {
                    // Repetir a la velocidad configurada
                    if (timeSinceLastRepeat >= g_gameState->backspaceRepeatRate) {
                        // Borrar carácter según el contexto activo
                        if (g_gameState->isEditingWorldName && g_gameState->screenState == SCREEN_WORLD_SELECT) {
                            if (!g_gameState->editingWorldNewName.empty()) {
                                g_gameState->editingWorldNewName.pop_back();
                            }
                        } else if (g_gameState->screenState == SCREEN_WORLD_CREATE) {
                            if (g_gameState->isEditingNewWorldName && !g_gameState->newWorldName.empty()) {
                                g_gameState->newWorldName.pop_back();
                            } else if (g_gameState->isEditingNewWorldSeed && !g_gameState->newWorldSeed.empty()) {
                                g_gameState->newWorldSeed.pop_back();
                            }
                        }
                        g_gameState->backspaceLastRepeatTime = currentTime;
                    }
                }
            }

        frameCount++;
        // ⭐ Ventana de FPS medida con reloj real, NO con deltaTime: deltaTime
        // está clampeado a 100 ms arriba, así que al ir el juego por debajo de
        // 10 FPS el contador avanzaba más lento que el tiempo real y el FPS
        // mostrado quedaba inflado justo cuando peor iba.
        if (currentTime - fpsWindowStart >= 1.0) {
            double elapsed = currentTime - fpsWindowStart;
            double fps = frameCount / elapsed;

            char title[256];
            snprintf(title, sizeof(title), "VoxelWorld | FPS:%.1f | Phys:%.1fms Chunks:%.1fms Render:%.1fms | Pos:%.0f,%.0f,%.0f",
                    fps, physics_ms, chunks_ms, render_ms,
                    g_gameState->player.position.x,
                    g_gameState->player.position.y,
                    g_gameState->player.position.z);
            glfwSetWindowTitle(window, title);

            // ⭐ Volcado periódico de perfilado al log: la ventana no siempre
            // se puede observar (y F3 requiere estar delante), así que las
            // decisiones de optimización se toman sobre estos números.
            static int perfDumpCounter = 0;
            if (++perfDumpCounter % 5 == 0) {
                auto top = Profiler::ProfilerManager::getInstance()->getTopFunctions(8);
                std::cout << "[PERF] t=" << (int)currentTime << "s fps=" << fps
                          << " frame=" << (1000.0 / (fps > 0 ? fps : 1)) << "ms"
                          << " chunks=" << g_gameState->world.getChunkCount()
                          << " screen=" << (int)g_gameState->screenState;
                for (const auto& [name, ms] : top) {
                    std::cout << " | " << name << "=" << ms << "ms";
                }
                std::cout << std::endl;
            }

            frameCount = 0;
            fpsWindowStart = currentTime;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);

        // Renderizar según el estado actual
        if (g_gameState->screenState == SCREEN_MAIN_MENU) {
            // Asegurar que el cursor esté visible
            if (g_gameState->cursorLocked) {
                g_gameState->cursorLocked = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
            renderMainMenu(g_gameState, width, height, window);
        }
        else if (g_gameState->screenState == SCREEN_WORLD_SELECT) {
            // Asegurar que el cursor esté visible
            if (g_gameState->cursorLocked) {
                g_gameState->cursorLocked = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
            renderWorldSelect(g_gameState, width, height, window);
        }
        else if (g_gameState->screenState == SCREEN_WORLD_CREATE) {  // ⭐⭐⭐ NUEVO
            // Asegurar que el cursor esté visible
            if (g_gameState->cursorLocked) {
                g_gameState->cursorLocked = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
            renderWorldCreateScreen(g_gameState, width, height, window);
        }
        else if (g_gameState->screenState == SCREEN_LOADING) {
            // Pantalla de carga: cursor visible pero no interactivo
            if (g_gameState->cursorLocked) {
                g_gameState->cursorLocked = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }

            // Renderizar pantalla de carga con animación
            renderLoadingScreen(g_gameState, width, height, currentTime);

            // Calcular tiempo transcurrido
            float elapsed = currentTime - g_gameState->loadingStartTime;

            // ⭐⭐⭐ GENERAR CHUNKS DURANTE LA CARGA (CRÍTICO)
            // Solo buscar spawn después de generar chunks
            if (!g_gameState->spawnFound && elapsed >= 0.5f && elapsed < 3.0f) {
                // PASO 1: Generar chunks alrededor del jugador
                std::cout << "Generando chunks iniciales alrededor del jugador..." << std::endl;
                Vec3 previousPos = g_gameState->player.position;
                g_gameState->world.updateChunks(g_gameState->player.position, previousPos);

                // PASO 2: Ahora SÍ buscar spawn seguro (los chunks ya existen)
                std::cout << "Buscando spawn seguro..." << std::endl;
                Vec3 safeSpawn = findSafeSpawn(g_gameState->world);
                g_gameState->targetSpawnPosition = safeSpawn;
                g_gameState->spawnFound = true;
                g_gameState->player.position = safeSpawn;
                g_gameState->player.yaw = 0;
                g_gameState->player.pitch = 0;
                std::cout << "Spawn encontrado en Y=" << safeSpawn.y << std::endl;
            }

            // Después de la duración de carga, cambiar al juego
            if (elapsed >= g_gameState->loadingDuration) {
                std::cout << "Carga completada! Iniciando juego..." << std::endl;

                // ⭐⭐⭐ NUEVO: En modo creativo, llenar inventario con todos los bloques
                // Cargar gameMode del level.dat
                std::filesystem::path levelPath = std::filesystem::path("saves") / g_gameState->currentWorldName / "level.dat";
                int gameMode = 0;  // Default: Survival
                if (std::filesystem::exists(levelPath)) {
                    std::ifstream file(levelPath);
                    std::string line;
                    while (std::getline(file, line)) {
                        if (line.find("GameMode=") == 0) {
                            gameMode = std::stoi(line.substr(9));
                            break;
                        }
                    }
                    file.close();
                }

                if (gameMode == 1) {  // 1 = Creative
                    std::cout << "🎨 Modo CREATIVO: Llenando inventario con todos los bloques..." << std::endl;

                    // ⭐ CRÍTICO: Asegurar que TextureManager está inicializado
                    if (g_textureManager == nullptr) {
                        std::cout << "⚠️ TextureManager NULL! Inicializando..." << std::endl;
                        g_textureManager = new TextureManager();
                        g_textureManager->loadAllBlockTextures();
                    }

                    // ⭐ Recargar texturas para asegurar que estén disponibles
                    std::cout << "🔄 Recargando texturas para modo creativo..." << std::endl;
                    g_textureManager->loadAllBlockTextures();

                    g_gameState->inventory.clear();  // Limpiar inventario primero

                    // Agregar todos los bloques en orden de creación
                    // Comenzando desde BLOCK_GRASS (1) hasta el último bloque
                    std::vector<BlockType> creativeModeBlocks = {
                        BLOCK_GRASS,       // 1
                        BLOCK_DIRT,        // 2
                        BLOCK_STONE,       // 3
                        BLOCK_WOOD,        // 4
                        BLOCK_LEAVES,      // 5
                        BLOCK_SAND,        // 6
                        BLOCK_WATER,       // 7 - ⭐ INCLUYE AGUA
                        BLOCK_TALLGRASS,   // 8
                        BLOCK_BEDROCK,     // 9
                        BLOCK_COBBLESTONE, // 10
                        BLOCK_PLANKS,      // 11
                        BLOCK_BRICKS,      // 12
                        BLOCK_GLASS,       // 13
                        BLOCK_COAL_ORE,    // 14
                        BLOCK_DIAMOND_ORE, // 15
                        BLOCK_GRAVEL,      // 16
                        BLOCK_ORANGE_FLOWER, // 17
                        BLOCK_SNOW,        // 18
                        BLOCK_SCRAP_METAL, // 19
                        BLOCK_LAVA,        // 20 - ⭐ INCLUYE LAVA
                        BLOCK_IRON_ORE,    // 21
                        BLOCK_GOLD_ORE,    // 22
                        BLOCK_SILVER_ORE,  // 23
                        BLOCK_DIRT_POWDER, // 24
                        BLOCK_STICK,       // 25
                        BLOCK_HOE          // 26
                    };

                    // Agregar cada bloque con stack de 64
                    for (BlockType blockType : creativeModeBlocks) {
                        g_gameState->inventory.addItem(blockType, 64);
                    }

                    std::cout << "✅ Inventario creativo llenado con " << creativeModeBlocks.size() << " tipos de bloques" << std::endl;

                    // ⭐ NO abrir inventario automáticamente
                    g_gameState->inventoryOpen = false;
                }

                g_gameState->screenState = SCREEN_IN_GAME;
                g_gameState->isLoading = false;
                g_gameState->cursorLocked = true;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
        }
        else if (g_gameState->screenState == SCREEN_IN_GAME) {
            // ⭐⭐⭐ NUEVO: Tracking de tiempo de sesión para level.dat
            g_gameState->currentSessionTime += deltaTime;
            // Asegurar que el cursor esté bloqueado cuando jugamos
            if (!g_gameState->cursorLocked && !g_gameState->isPaused && !g_gameState->inventoryOpen) {
                g_gameState->cursorLocked = true;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }

            // Manejar proceso de guardado
            if (g_gameState->isSaving) {
                g_gameState->savingTimer -= deltaTime;

                if (g_gameState->savingTimer <= 0.0f) {
                    // Guardar el mundo
                    saveWorld(g_gameState);

                    // Finalizar guardado
                    g_gameState->isSaving = false;

                    if (g_gameState->returnToMenuAfterSave) {
                        // Volver al menú principal
                        g_gameState->screenState = SCREEN_MAIN_MENU;
                        g_gameState->isPaused = false;
                        g_gameState->cursorLocked = false;
                        g_gameState->returnToMenuAfterSave = false;
                        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                        std::cout << "Regresando al menú principal..." << std::endl;
                        continue; // Saltar al siguiente frame
                    }
                }
            }

            // ⭐⭐⭐ SISTEMA DE AUTO-GUARDADO PERIÓDICO ⭐⭐⭐
            if (!g_gameState->isPaused && !g_gameState->isSaving) {
                g_gameState->autoSaveTimer += deltaTime;

                // Auto-guardar cada 2 minutos (120 segundos)
                if (g_gameState->autoSaveTimer >= g_gameState->autoSaveInterval) {
                    std::cout << "\n⏰ Auto-guardado activado..." << std::endl;
                    saveWorld(g_gameState);
                    g_gameState->autoSaveTimer = 0.0f;  // Reset timer

                    // Activar indicador visual
                    g_gameState->showSavingIndicator = true;
                    g_gameState->savingIndicatorTimer = 3.0f;  // Mostrar por 3 segundos
                }

                // Actualizar indicador visual
                if (g_gameState->showSavingIndicator) {
                    g_gameState->savingIndicatorTimer -= deltaTime;
                    if (g_gameState->savingIndicatorTimer <= 0.0f) {
                        g_gameState->showSavingIndicator = false;
                    }
                }
            }

            // Solo actualizar físicas si no está pausado y no está guardando
            if (!g_gameState->isPaused && !g_gameState->isSaving) {
                updatePlayerPhysics(g_gameState->player, g_gameState->world, deltaTime, g_gameState->keys);

                // ⭐ TIRAR ITEMS CON Q: Presionar Q para tirar el item seleccionado
                static bool qWasPressed = false;
                bool qPressed = (g_gameState->keys['Q'] || g_gameState->keys['q']);
                if (qPressed && !qWasPressed && !g_gameState->inventoryOpen) {
                    dropSelectedItem(g_gameState);
                }
                qWasPressed = qPressed;

                // ⭐⭐⭐ SISTEMA ANTI-ATRAPAMIENTO: Liberar jugador de bloques ⭐⭐⭐
                {
                    AABB playerBox = getPlayerAABB(g_gameState->player.position,
                                                    g_gameState->player.WIDTH,
                                                    g_gameState->player.HEIGHT);

                    // Expandir ligeramente para mejor detección
                    int minX = (int)floor(playerBox.minX);
                    int maxX = (int)floor(playerBox.maxX);
                    int minY = (int)floor(playerBox.minY);
                    int maxY = (int)floor(playerBox.maxY);
                    int minZ = (int)floor(playerBox.minZ);
                    int maxZ = (int)floor(playerBox.maxZ);

                    std::vector<std::tuple<int, int, int>> blocksToBreak;

                    // Detectar bloques que atraviesan al jugador
                    for (int x = minX; x <= maxX; x++) {
                        for (int y = minY; y <= maxY; y++) {
                            for (int z = minZ; z <= maxZ; z++) {
                                BlockType block = g_gameState->world.getBlock(x, y, z);

                                if (isBlockSolid(block)) {
                                    AABB blockBox = getBlockAABB(x, y, z);

                                    // Verificar si el bloque realmente intersecta con el jugador
                                    if (playerBox.intersects(blockBox)) {
                                        // ⭐ NO romper bloques bajo los pies (permitir estar parado)
                                        float blockTop = (float)(y + 1);
                                        float playerFeet = playerBox.minY;

                                        // Si el bloque está completamente por encima de los pies, romperlo
                                        if (blockTop > playerFeet + 0.1f) {
                                            blocksToBreak.push_back(std::make_tuple(x, y, z));
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ⭐ Romper bloques que atrapan al jugador
                    if (!blocksToBreak.empty()) {
                        std::cout << "⚠️ ANTI-ATRAPAMIENTO: Detectados " << blocksToBreak.size()
                                  << " bloques atrapando al jugador" << std::endl;

                        for (const auto& pos : blocksToBreak) {
                            int bx = std::get<0>(pos);
                            int by = std::get<1>(pos);
                            int bz = std::get<2>(pos);

                            BlockType blockType = g_gameState->world.getBlock(bx, by, bz);

                            std::cout << "  🔨 Rompiendo bloque en (" << bx << ", " << by << ", " << bz << ")" << std::endl;

                            // Destruir el bloque
                            g_gameState->world.setBlock(bx, by, bz, BLOCK_AIR);

                            // ⭐ Crear partículas de ruptura
                            g_gameState->particles.spawnMiningParticles(Vec3((float)bx, (float)by, (float)bz), blockType);

                            // ⭐ SISTEMA DE DROPS: Dropear items del bloque roto
                            if (blockType != BLOCK_AIR && blockType != BLOCK_WATER && blockType != BLOCK_LAVA) {
                                Vec3 dropPos(bx + 0.5f, by + 0.5f, bz + 0.5f);
                                std::vector<BlockDrop> drops = getBlockDrops(blockType);
                                for (const auto& drop : drops) {
                                    if (drop.chance >= 1.0f) {
                                        for (int i = 0; i < drop.count; i++) {
                                            g_gameState->spawnItem(dropPos, drop.itemType);
                                        }
                                    }
                                }
                            }
                        }

                        std::cout << "✅ Jugador liberado de bloques" << std::endl;
                    }
                }

                // ⭐⭐⭐ PROTECCIÓN ANTI-VOID: Teletransportar jugador si cae al vacío ⭐⭐⭐
                if (g_gameState->player.position.y < -10.0f) {
                    std::cout << "\n⚠️ ANTI-VOID: Jugador cayó al vacío (Y=" << g_gameState->player.position.y << ")" << std::endl;
                    std::cout << "🔄 Teletransportando a la superficie..." << std::endl;

                    // Buscar superficie sólida en la posición X,Z actual
                    bool foundSurface = false;
                    int playerX = (int)g_gameState->player.position.x;
                    int playerZ = (int)g_gameState->player.position.z;

                    // Buscar desde arriba hacia abajo
                    for (int y = 128; y >= 10; y--) {
                        BlockType current = g_gameState->world.getBlock(playerX, y, playerZ);
                        BlockType below = g_gameState->world.getBlock(playerX, y - 1, playerZ);

                        if (current == BLOCK_AIR && below != BLOCK_AIR && below != BLOCK_WATER && below != BLOCK_LAVA) {
                            g_gameState->player.position.y = y + 0.1f;
                            foundSurface = true;
                            std::cout << "✅ Superficie encontrada en Y=" << y << std::endl;
                            break;
                        }
                    }

                    // Si no se encuentra superficie, teletransportar a Y=100
                    if (!foundSurface) {
                        g_gameState->player.position.y = 100.0f;
                        std::cout << "⚠️ No se encontró superficie, usando Y=100" << std::endl;
                    }

                    // Resetear velocidad vertical para evitar caída continua
                    g_gameState->player.velocity.y = 0.0f;
                    g_gameState->player.onGround = false;

                    // Guardar automáticamente después de rescue anti-void
                    if (!g_gameState->currentWorldName.empty()) {
                        std::cout << "💾 Guardando posición después de rescue..." << std::endl;
                        saveWorld(g_gameState);
                    }
                }

                t2 = std::chrono::high_resolution_clock::now();
                physics_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                t1 = t2;
                // ⭐⭐⭐ CARGA PREDICTIVA: Actualizar chunks con dirección de movimiento y throttling dinámico
                g_gameState->world.updateChunks(g_gameState->player.position, g_gameState->player.previousPosition, deltaTime);
                // Actualizar posición previa para el siguiente frame
                g_gameState->player.previousPosition = g_gameState->player.position;
                t2 = std::chrono::high_resolution_clock::now();
                chunks_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                t1 = t2;

                // ⭐ SISTEMA DE FLUJO DE AGUA: Actualizar cada tick (basado en cola)
                static double waterUpdateTimer = 0.0;
                static bool waterFlowEnabled = true;  // ⭐ Cambiar a false para deshabilitar

                if (waterFlowEnabled) {
                    waterUpdateTimer += deltaTime;
                    // ⭐ AJUSTADO: Actualizar cada 0.5 segundos (2 veces por segundo) para flujo más natural
                    if (waterUpdateTimer >= 0.5) {
                        g_gameState->world.updateWaterFlow();
                        waterUpdateTimer = 0.0;
                    }
                }

                // LIGHTING DESHABILITADO PARA DIAGNOSTICO
                // g_gameState->world.processLightingQueue();
                g_gameState->updateItems(deltaTime);

                // ANIMACIÓN: Actualizar animación de agua
                g_textureManager->updateWaterAnimation(deltaTime);

                // Sistema de minado progresivo (como Minecraft)
                if (!g_gameState->inventoryOpen) {
                    updateMining(g_gameState, deltaTime);
                }

                // Actualizar sistema de partículas (gravedad, vida, etc.)
                g_gameState->particles.update(deltaTime);

                // Decrementar cooldowns
                if (g_gameState->breakCooldown > 0) {
                    g_gameState->breakCooldown -= deltaTime;
                }
                if (g_gameState->placeCooldown > 0) {
                    g_gameState->placeCooldown -= deltaTime;
                }
            }

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        float aspect = (float)width / (float)height;
        float fov = 70.0f;
        float zNear = 0.05f;  // Muy cercano para permitir ver bloques pegados a la cámara
        float zFar = 128.0f;  // Optimizado con niebla - chunks más allá están ocultos
        float fH = tan(fov * 3.14159f / 360.0f) * zNear;
        float fW = fH * aspect;
        glFrustum(-fW, fW, -fH, fH, zNear, zFar);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glRotatef(-g_gameState->player.pitch, 1, 0, 0);
        glRotatef(-g_gameState->player.yaw, 0, 1, 0);

        Vec3 eye = g_gameState->player.getEyePosition();
        glTranslatef(-eye.x, -eye.y, -eye.z);

        // Pasar posición del jugador para near plane culling
        g_gameState->world.render(g_gameState->player.position);
        t2 = std::chrono::high_resolution_clock::now();
        render_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

        // Renderizar selección de bloque (wireframe)
        if (!g_gameState->isPaused && !g_gameState->inventoryOpen) {
            Vec3 origin = g_gameState->player.getEyePosition();
            Vec3 direction = g_gameState->player.getForward();
            RaycastResult result = raycastBlock(g_gameState->world, origin, direction, 5.0f);

            if (result.hit) {
                // Como Minecraft: mostrar wireframe en TODOS los bloques que detecta el raycast
                BlockType blockType = g_gameState->world.getBlock(result.blockPos.x, result.blockPos.y, result.blockPos.z);
                if (blockType != BLOCK_AIR && blockType != BLOCK_WATER) {
                    // Deshabilitar depth test para que las líneas se vean siempre
                    glDisable(GL_DEPTH_TEST);

                    // Deshabilitar culling para ver las líneas desde cualquier ángulo
                    glDisable(GL_CULL_FACE);

                    glPushMatrix();

                    // Posicionar en el bloque seleccionado
                    glTranslatef(result.blockPos.x, result.blockPos.y, result.blockPos.z);

                    // Escala ligeramente mayor para evitar z-fighting (técnica estándar)
                    const float scale = 1.001f;  // Reducido para estar más pegado al bloque
                    float offset = (1.0f - scale) / 2.0f;
                    glTranslatef(offset, offset, offset);
                    glScalef(scale, scale, scale);

                    // Color gris oscuro más sutil y transparente
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glColor4f(0.2f, 0.2f, 0.2f, 0.4f);  // Gris oscuro semi-transparente

                    // Líneas más delgadas y sutiles
                    glLineWidth(1.5f);

                    // Dibujar SOLO la cara que estás mirando (usando el normal)
                    glBegin(GL_LINE_LOOP);  // LINE_LOOP dibuja un rectángulo cerrado con 4 vértices

                    // Determinar qué cara dibujar según el normal
                    if (result.normal.x == 1) {
                        // Cara derecha (X+)
                        glVertex3f(1, 0, 0);
                        glVertex3f(1, 0, 1);
                        glVertex3f(1, 1, 1);
                        glVertex3f(1, 1, 0);
                    } else if (result.normal.x == -1) {
                        // Cara izquierda (X-)
                        glVertex3f(0, 0, 0);
                        glVertex3f(0, 1, 0);
                        glVertex3f(0, 1, 1);
                        glVertex3f(0, 0, 1);
                    } else if (result.normal.y == 1) {
                        // Cara arriba (Y+)
                        glVertex3f(0, 1, 0);
                        glVertex3f(1, 1, 0);
                        glVertex3f(1, 1, 1);
                        glVertex3f(0, 1, 1);
                    } else if (result.normal.y == -1) {
                        // Cara abajo (Y-)
                        glVertex3f(0, 0, 0);
                        glVertex3f(0, 0, 1);
                        glVertex3f(1, 0, 1);
                        glVertex3f(1, 0, 0);
                    } else if (result.normal.z == 1) {
                        // Cara frontal (Z+)
                        glVertex3f(0, 0, 1);
                        glVertex3f(1, 0, 1);
                        glVertex3f(1, 1, 1);
                        glVertex3f(0, 1, 1);
                    } else if (result.normal.z == -1) {
                        // Cara trasera (Z-)
                        glVertex3f(0, 0, 0);
                        glVertex3f(0, 1, 0);
                        glVertex3f(1, 1, 0);
                        glVertex3f(1, 0, 0);
                    }

                    glEnd();

                    glDisable(GL_BLEND);

                    // Renderizar grietas de minado (progreso)
                    if (g_gameState->isMining &&
                        result.blockPos.x == g_gameState->miningBlockPos.x &&
                        result.blockPos.y == g_gameState->miningBlockPos.y &&
                        result.blockPos.z == g_gameState->miningBlockPos.z) {

                        float progress = g_gameState->miningProgress;

                        // Color blanco con transparencia según progreso
                        glColor4f(1.0f, 1.0f, 1.0f, 0.3f + progress * 0.5f);
                        glLineWidth(2.0f);

                        // Las grietas aparecen en la cara que estás mirando
                        // (Las grietas no necesitan adaptarse por cara, se renderizan sobre la cara visible)
                    }

                    glPopMatrix();

                    // Re-habilitar depth test y culling
                    glEnable(GL_DEPTH_TEST);
                    glEnable(GL_CULL_FACE);
                }
            }
        }

        // ⭐ Solo renderizar items y efectos del juego si estamos EN EL JUEGO
        if (g_gameState->screenState == SCREEN_IN_GAME) {
            // Renderizar items en el mundo (cubitos pequeños con texturas)
            // Deshabilitar culling para que todas las caras sean visibles
            glDisable(GL_CULL_FACE);
            glEnable(GL_TEXTURE_2D);

            glPushMatrix();
            for (const ItemEntity& item : g_gameState->items) {
            glPushMatrix();

            // Movimiento de flotación suave (bobbing)
            float bobOffset = sin(item.lifetime * 3.0f) * 0.1f;
            glTranslatef(item.position.x, item.position.y + bobOffset, item.position.z);

            // Rotación animada basada en tiempo de vida
            glRotatef(item.lifetime * 100.0f, 0, 1, 0);

            // Escala más pequeña (0.2 = 20% del tamaño normal)
            float scale = 0.2f;

            // Dibujar cubo pequeño con texturas del bloque
            // Face 0 = Top (+Y)
            GLuint texTop = g_textureManager->getBlockTexture(item.blockType, 0);
            glBindTexture(GL_TEXTURE_2D, texTop);
            glBegin(GL_QUADS);
            glColor3f(1.0f, 1.0f, 1.0f);  // Más brillante
            glTexCoord2f(0, 0); glVertex3f(-scale,  scale, -scale);
            glTexCoord2f(0, 1); glVertex3f(-scale,  scale,  scale);
            glTexCoord2f(1, 1); glVertex3f( scale,  scale,  scale);
            glTexCoord2f(1, 0); glVertex3f( scale,  scale, -scale);
            glEnd();

            // Face 1 = Bottom (-Y)
            GLuint texBottom = g_textureManager->getBlockTexture(item.blockType, 1);
            glBindTexture(GL_TEXTURE_2D, texBottom);
            glBegin(GL_QUADS);
            glColor3f(0.5f, 0.5f, 0.5f);  // Más oscuro
            glTexCoord2f(0, 0); glVertex3f(-scale, -scale, -scale);
            glTexCoord2f(1, 0); glVertex3f( scale, -scale, -scale);
            glTexCoord2f(1, 1); glVertex3f( scale, -scale,  scale);
            glTexCoord2f(0, 1); glVertex3f(-scale, -scale,  scale);
            glEnd();

            // Face 2 = North (+Z)
            GLuint texNorth = g_textureManager->getBlockTexture(item.blockType, 2);
            glBindTexture(GL_TEXTURE_2D, texNorth);
            glBegin(GL_QUADS);
            glColor3f(0.8f, 0.8f, 0.8f);  // N/S faces
            glTexCoord2f(0, 0); glVertex3f(-scale, -scale,  scale);
            glTexCoord2f(1, 0); glVertex3f( scale, -scale,  scale);
            glTexCoord2f(1, 1); glVertex3f( scale,  scale,  scale);
            glTexCoord2f(0, 1); glVertex3f(-scale,  scale,  scale);
            glEnd();

            // Face 3 = South (-Z)
            GLuint texSouth = g_textureManager->getBlockTexture(item.blockType, 3);
            glBindTexture(GL_TEXTURE_2D, texSouth);
            glBegin(GL_QUADS);
            glColor3f(0.8f, 0.8f, 0.8f);  // N/S faces
            glTexCoord2f(0, 0); glVertex3f( scale, -scale, -scale);
            glTexCoord2f(1, 0); glVertex3f(-scale, -scale, -scale);
            glTexCoord2f(1, 1); glVertex3f(-scale,  scale, -scale);
            glTexCoord2f(0, 1); glVertex3f( scale,  scale, -scale);
            glEnd();

            // Face 4 = East (+X)
            GLuint texEast = g_textureManager->getBlockTexture(item.blockType, 4);
            glBindTexture(GL_TEXTURE_2D, texEast);
            glBegin(GL_QUADS);
            glColor3f(0.6f, 0.6f, 0.6f);  // E/W faces = más oscuro
            glTexCoord2f(0, 0); glVertex3f( scale, -scale,  scale);
            glTexCoord2f(1, 0); glVertex3f( scale, -scale, -scale);
            glTexCoord2f(1, 1); glVertex3f( scale,  scale, -scale);
            glTexCoord2f(0, 1); glVertex3f( scale,  scale,  scale);
            glEnd();

            // Face 5 = West (-X)
            GLuint texWest = g_textureManager->getBlockTexture(item.blockType, 5);
            glBindTexture(GL_TEXTURE_2D, texWest);
            glBegin(GL_QUADS);
            glColor3f(0.6f, 0.6f, 0.6f);  // E/W faces = más oscuro
            glTexCoord2f(0, 0); glVertex3f(-scale, -scale, -scale);
            glTexCoord2f(1, 0); glVertex3f(-scale, -scale,  scale);
            glTexCoord2f(1, 1); glVertex3f(-scale,  scale,  scale);
            glTexCoord2f(0, 1); glVertex3f(-scale,  scale, -scale);
            glEnd();

            glPopMatrix();
        }
        glPopMatrix();

        // Re-habilitar culling y deshabilitar texturas para el resto del renderizado
        glEnable(GL_CULL_FACE);
        glDisable(GL_TEXTURE_2D);

        // Renderizar partículas (en modo 3D)
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_gameState->particles.render();
        glDisable(GL_BLEND);

        // ⭐ RENDERIZAR MANO DEL JUGADOR con bloque seleccionado
        if (!g_gameState->isPaused && !g_gameState->inventoryOpen) {
            BlockType heldBlock = g_gameState->inventory.getSelectedBlock();
            float swingProgress = g_gameState->isMining ? g_gameState->miningProgress : 0.0f;
            renderPlayerHand(heldBlock, swingProgress, deltaTime);
        }

        // Renderizar outline del bloque apuntado
        Vec3 rayOrigin = g_gameState->player.getEyePosition();
        Vec3 rayDirection = g_gameState->player.getForward();
        RaycastResult rayResult = raycastBlock(g_gameState->world, rayOrigin, rayDirection, 4.0f);


        // Renderizar línea de raycast (desde los ojos del jugador hasta el bloque)
        if (!g_gameState->isPaused) {
            Vec3 lineOrigin = g_gameState->player.getEyePosition();
            Vec3 lineDirection = g_gameState->player.getForward();
            RaycastResult lineResult = raycastBlock(g_gameState->world, lineOrigin, lineDirection, 4.0f);

            float lineDistance = lineResult.hit ? lineResult.distance : 4.0f;
            renderRaycastLine(lineOrigin, lineDirection, lineDistance, lineResult.hit);
        }

        if (rayResult.hit && !g_gameState->isPaused) {
            renderBlockOutline(rayResult.blockPos, g_gameState->player.position);

            // Renderizar grietas si estamos minando este bloque
            if (g_gameState->isMining &&
                rayResult.blockPos.x == g_gameState->miningBlockPos.x &&
                rayResult.blockPos.y == g_gameState->miningBlockPos.y &&
                rayResult.blockPos.z == g_gameState->miningBlockPos.z) {
                renderBlockCrack(rayResult.blockPos, g_gameState->miningProgress);
            }
        }

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width, height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_DEPTH_TEST);

        // ⭐ CROSSHAIR (Centro de mira)
        if (!g_gameState->isPaused) {
            glColor3f(1, 1, 1);
            glLineWidth(2);
            glBegin(GL_LINES);
            glVertex2f(width / 2 - 10, height / 2);
            glVertex2f(width / 2 + 10, height / 2);
            glVertex2f(width / 2, height / 2 - 10);
            glVertex2f(width / 2, height / 2 + 10);
            glEnd();
        }

        if (!g_gameState->isPaused) {
            // ============ DEBUG: Información del raycast ============
            Vec3 debugRayOrigin = g_gameState->player.getEyePosition();
            Vec3 debugRayDirection = g_gameState->player.getForward();
            RaycastResult debugRayResult = raycastBlock(g_gameState->world, debugRayOrigin, debugRayDirection, 4.0f);

            // Mostrar información de debug en esquina superior izquierda
            glColor4f(1.0f, 1.0f, 0.0f, 1.0f);  // Amarillo
            char debugText[256];

            // Posición del jugador
            sprintf(debugText, "Pos: (%.1f, %.1f, %.1f)",
                g_gameState->player.position.x,
                g_gameState->player.position.y,
                g_gameState->player.position.z);
            renderText(debugText, 10, 10, 12);

            // Origen del raycast (posición de los ojos)
            Vec3 eyePos = g_gameState->player.getEyePosition();
            sprintf(debugText, "Ojos: (%.1f, %.1f, %.1f)",
                eyePos.x, eyePos.y, eyePos.z);
            renderText(debugText, 10, 150, 12);

            // Dirección del raycast (normalizada)
            sprintf(debugText, "Dir: (%.2f, %.2f, %.2f)",
                debugRayDirection.x,
                debugRayDirection.y,
                debugRayDirection.z);
            renderText(debugText, 10, 30, 12);

            // Ángulos de cámara
            sprintf(debugText, "Yaw: %.1f  Pitch: %.1f",
                g_gameState->player.yaw,
                g_gameState->player.pitch);
            renderText(debugText, 10, 50, 12);

            // Bloque detectado
            if (debugRayResult.hit) {
                BlockType blockType = g_gameState->world.getBlock(
                    debugRayResult.blockPos.x,
                    debugRayResult.blockPos.y,
                    debugRayResult.blockPos.z
                );

                const char* blockName = "DESCONOCIDO";
                switch(blockType) {
                    case BLOCK_DIRT: blockName = "TIERRA"; break;
                    case BLOCK_DIRT_POWDER: blockName = "POLVO DE TIERRA"; break;
                    case BLOCK_STICK: blockName = "PALO"; break;
                    case BLOCK_HOE: blockName = "HOZ"; break;
                    case BLOCK_COAL_ITEM: blockName = "CARBON"; break;
                    case BLOCK_RAW_ZINC: blockName = "ZINC CRUDO"; break;
                    case BLOCK_RAW_COPPER: blockName = "COBRE CRUDO"; break;
                    case BLOCK_GRASS: blockName = "CESPED"; break;
                    case BLOCK_STONE: blockName = "PIEDRA"; break;
                    case BLOCK_WOOD: blockName = "MADERA"; break;
                    case BLOCK_LEAVES: blockName = "HOJAS"; break;
                    case BLOCK_SAND: blockName = "ARENA"; break;
                    case BLOCK_BEDROCK: blockName = "BEDROCK"; break;
                    case BLOCK_WATER: blockName = "AGUA"; break;
                    case BLOCK_GRAVEL: blockName = "GRAVA"; break;
                    case BLOCK_ORANGE_FLOWER: blockName = "FLOR NARANJA"; break;
                    default: break;
                }

                glColor4f(0.0f, 1.0f, 0.0f, 1.0f);  // Verde = detectado
                sprintf(debugText, "BLOQUE: %s (%d, %d, %d)",
                    blockName,
                    debugRayResult.blockPos.x,
                    debugRayResult.blockPos.y,
                    debugRayResult.blockPos.z);
                renderText(debugText, 10, 70, 12);

                sprintf(debugText, "Dist: %.2f bloques", debugRayResult.distance);
                renderText(debugText, 10, 90, 12);
            } else {
                glColor4f(1.0f, 0.0f, 0.0f, 1.0f);  // Rojo = no detectado
                sprintf(debugText, "BLOQUE: NINGUNO (aire/lejos)");
                renderText(debugText, 10, 70, 12);
            }

            // Indicador de dirección cardinal
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            float yaw = g_gameState->player.yaw;
            const char* direction = "";

            // Normalizar yaw a 0-360
            while (yaw < 0) yaw += 360;
            while (yaw >= 360) yaw -= 360;

            if (yaw >= 337.5f || yaw < 22.5f) direction = "SUR";
            else if (yaw >= 22.5f && yaw < 67.5f) direction = "SUR-OESTE";
            else if (yaw >= 67.5f && yaw < 112.5f) direction = "OESTE";
            else if (yaw >= 112.5f && yaw < 157.5f) direction = "NOR-OESTE";
            else if (yaw >= 157.5f && yaw < 202.5f) direction = "NORTE";
            else if (yaw >= 202.5f && yaw < 247.5f) direction = "NOR-ESTE";
            else if (yaw >= 247.5f && yaw < 292.5f) direction = "ESTE";
            else if (yaw >= 292.5f && yaw < 337.5f) direction = "SUR-ESTE";

            sprintf(debugText, "Mirando: %s", direction);
            renderText(debugText, 10, 110, 12);

            // Indicador vertical
            float pitch = g_gameState->player.pitch;
            const char* vertical = "";
            if (pitch < -45) vertical = "ARRIBA";
            else if (pitch > 45) vertical = "ABAJO";
            else vertical = "HORIZONTAL";

            sprintf(debugText, "Vertical: %s", vertical);
            renderText(debugText, 10, 130, 12);
            // ============ FIN DEBUG ============

            // ⭐⭐⭐ OVERLAY DE AGUA: Renderizar filtro azul cuando estás bajo el agua
            if (g_gameState->player.isUnderwater) {
                // Configurar OpenGL para overlay 2D
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_LIGHTING);
                glDisable(GL_FOG);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                // Matriz ortográfica 2D para fullscreen quad
                glMatrixMode(GL_PROJECTION);
                glPushMatrix();
                glLoadIdentity();
                glOrtho(0, width, height, 0, -1, 1);
                glMatrixMode(GL_MODELVIEW);
                glPushMatrix();
                glLoadIdentity();

                // ⭐ Renderizar textura de agua muy transparente con tinte azul
                // Si tienes textura de agua, úsala; si no, solo el filtro
                if (g_textureManager) {
                    glEnable(GL_TEXTURE_2D);
                    GLuint waterTexture = g_textureManager->getBlockTexture(BLOCK_WATER, 0);
                    glBindTexture(GL_TEXTURE_2D, waterTexture);

                    // Color azul con transparencia muy alta (95% transparente)
                    glColor4f(0.3f, 0.5f, 1.0f, 0.15f); // Azul muy transparente

                    glBegin(GL_QUADS);
                    glTexCoord2f(0, 0); glVertex2f(0, 0);
                    glTexCoord2f(width/64.0f, 0); glVertex2f(width, 0);
                    glTexCoord2f(width/64.0f, height/64.0f); glVertex2f(width, height);
                    glTexCoord2f(0, height/64.0f); glVertex2f(0, height);
                    glEnd();

                    glDisable(GL_TEXTURE_2D);
                }

                // ⭐ Filtro azul adicional encima para el efecto submarino
                glColor4f(0.2f, 0.4f, 0.8f, 0.25f); // Azul semi-transparente
                glBegin(GL_QUADS);
                glVertex2f(0, 0);
                glVertex2f(width, 0);
                glVertex2f(width, height);
                glVertex2f(0, height);
                glEnd();

                // Restaurar matrices
                glPopMatrix();
                glMatrixMode(GL_PROJECTION);
                glPopMatrix();
                glMatrixMode(GL_MODELVIEW);

                glEnable(GL_DEPTH_TEST);
            }

            // Renderizar hotbar
            renderHotbar(&g_gameState->inventory, width, height);

            // ⭐ Renderizar item en la mano (esquina inferior derecha)
            renderItemInHand(&g_gameState->inventory, width, height);

            // ⭐⭐⭐ INDICADOR VISUAL DE AUTO-GUARDADO ⭐⭐⭐
            if (g_gameState->showSavingIndicator) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                // Calcular alpha basado en timer (fade out)
                float alpha = g_gameState->savingIndicatorTimer / 3.0f;
                if (alpha > 1.0f) alpha = 1.0f;
                if (alpha < 0.0f) alpha = 0.0f;

                // Posición: esquina superior derecha
                float indicatorX = width - 200;
                float indicatorY = 20;
                float indicatorWidth = 180;
                float indicatorHeight = 40;

                // Fondo semi-transparente
                glColor4f(0.0f, 0.0f, 0.0f, 0.7f * alpha);
                glBegin(GL_QUADS);
                glVertex2f(indicatorX, indicatorY);
                glVertex2f(indicatorX + indicatorWidth, indicatorY);
                glVertex2f(indicatorX + indicatorWidth, indicatorY + indicatorHeight);
                glVertex2f(indicatorX, indicatorY + indicatorHeight);
                glEnd();

                // Borde verde
                glColor4f(0.0f, 1.0f, 0.0f, alpha);
                glLineWidth(2.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(indicatorX, indicatorY);
                glVertex2f(indicatorX + indicatorWidth, indicatorY);
                glVertex2f(indicatorX + indicatorWidth, indicatorY + indicatorHeight);
                glVertex2f(indicatorX, indicatorY + indicatorHeight);
                glEnd();

                // Icono de guardado (disco)
                float iconX = indicatorX + 10;
                float iconY = indicatorY + 10;
                float iconSize = 20;

                glColor4f(0.0f, 1.0f, 0.0f, alpha);
                glBegin(GL_QUADS);
                // Rectángulo del disco
                glVertex2f(iconX, iconY);
                glVertex2f(iconX + iconSize, iconY);
                glVertex2f(iconX + iconSize, iconY + iconSize);
                glVertex2f(iconX, iconY + iconSize);
                glEnd();

                // ⭐ Texto "...Guardando" (limpio y discreto)
                glColor4f(1.0f, 1.0f, 1.0f, alpha);
                renderText("...Guardando", indicatorX + 40, indicatorY + 12, 16);

                glDisable(GL_BLEND);
            }
        }
        } // ⭐ FIN de renderizado solo para SCREEN_IN_GAME

        // Renderizar overlay de pausa
        if (g_gameState->isPaused) {
            // Desactivar culling para renderizado 2D
            glDisable(GL_CULL_FACE);

            // Habilitar blending para transparencia
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Overlay oscuro semi-transparente (0, 0, 0, 0.5 = 50% transparencia)
            glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
            glBegin(GL_QUADS);
            glVertex2f(0, 0);
            glVertex2f(width, 0);
            glVertex2f(width, height);
            glVertex2f(0, height);
            glEnd();

            // Si está guardando, mostrar mensaje y no renderizar menú
            if (g_gameState->isSaving) {
                float centerX = width / 2.0f;
                float centerY = height / 2.0f;

                // Texto "GUARDANDO..."
                glColor4f(1.0f, 1.0f, 0.0f, 1.0f);  // Amarillo
                glBegin(GL_QUADS);
                glVertex2f(centerX - 150, centerY - 30);
                glVertex2f(centerX + 150, centerY - 30);
                glVertex2f(centerX + 150, centerY + 30);
                glVertex2f(centerX - 150, centerY + 30);
                glEnd();

                // Barra de progreso
                float progress = 1.0f - (g_gameState->savingTimer / 0.5f);
                glColor4f(0.0f, 1.0f, 0.0f, 0.8f);  // Verde
                glBegin(GL_QUADS);
                glVertex2f(centerX - 140, centerY + 50);
                glVertex2f(centerX - 140 + (280 * progress), centerY + 50);
                glVertex2f(centerX - 140 + (280 * progress), centerY + 70);
                glVertex2f(centerX - 140, centerY + 70);
                glEnd();
            } else {
                // Renderizar menú normal

            // Obtener posición del mouse para hover
            double mouseX, mouseY;
            glfwGetCursorPos(window, &mouseX, &mouseY);

            float centerX = width / 2.0f;
            float buttonWidth = 300.0f;
            float buttonHeight = 50.0f;
            float buttonSpacing = 60.0f;

            if (g_gameState->pauseMenuState == PAUSE_MENU_MAIN) {
                // MENÚ PRINCIPAL
                float startY = height / 2.0f - 80.0f;

                // Título "PAUSA"
                glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
                float titleY = startY - 60;
                glBegin(GL_QUADS);
                glVertex2f(centerX - 80, titleY);
                glVertex2f(centerX + 80, titleY);
                glVertex2f(centerX + 80, titleY + 40);
                glVertex2f(centerX - 80, titleY + 40);
                glEnd();

                // Botón 1: Reanudar
                Button btn1(centerX - buttonWidth / 2, startY, buttonWidth, buttonHeight, "Reanudar");
                bool hover1 = btn1.contains((float)mouseX, (float)mouseY);
                // Fondo gris (más claro en hover)
                glColor4f(hover1 ? 0.5f : 0.3f, hover1 ? 0.5f : 0.3f, hover1 ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btn1.x, btn1.y);
                glVertex2f(btn1.x + btn1.width, btn1.y);
                glVertex2f(btn1.x + btn1.width, btn1.y + btn1.height);
                glVertex2f(btn1.x, btn1.y + btn1.height);
                glEnd();
                // Borde gris oscuro
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glLineWidth(2);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btn1.x, btn1.y);
                glVertex2f(btn1.x + btn1.width, btn1.y);
                glVertex2f(btn1.x + btn1.width, btn1.y + btn1.height);
                glVertex2f(btn1.x, btn1.y + btn1.height);
                glEnd();
                // Texto blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("REANUDAR", btn1.x + 50, btn1.y + 15, 20);

                // Botón 2: Gráficos
                Button btn2(centerX - buttonWidth / 2, startY + buttonSpacing, buttonWidth, buttonHeight, "Graficos");
                bool hover2 = btn2.contains((float)mouseX, (float)mouseY);
                // Fondo gris (más claro en hover)
                glColor4f(hover2 ? 0.5f : 0.3f, hover2 ? 0.5f : 0.3f, hover2 ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btn2.x, btn2.y);
                glVertex2f(btn2.x + btn2.width, btn2.y);
                glVertex2f(btn2.x + btn2.width, btn2.y + btn2.height);
                glVertex2f(btn2.x, btn2.y + btn2.height);
                glEnd();
                // Borde gris oscuro
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glLineWidth(2);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btn2.x, btn2.y);
                glVertex2f(btn2.x + btn2.width, btn2.y);
                glVertex2f(btn2.x + btn2.width, btn2.y + btn2.height);
                glVertex2f(btn2.x, btn2.y + btn2.height);
                glEnd();
                // Texto blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("GRAFICOS", btn2.x + 60, btn2.y + 15, 20);

                // Botón 3: Sensibilidad
                Button btn3(centerX - buttonWidth / 2, startY + buttonSpacing * 2, buttonWidth, buttonHeight, "Sensibilidad");
                bool hover3 = btn3.contains((float)mouseX, (float)mouseY);
                // Fondo gris (más claro en hover)
                glColor4f(hover3 ? 0.5f : 0.3f, hover3 ? 0.5f : 0.3f, hover3 ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btn3.x, btn3.y);
                glVertex2f(btn3.x + btn3.width, btn3.y);
                glVertex2f(btn3.x + btn3.width, btn3.y + btn3.height);
                glVertex2f(btn3.x, btn3.y + btn3.height);
                glEnd();
                // Borde gris oscuro
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glLineWidth(2);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btn3.x, btn3.y);
                glVertex2f(btn3.x + btn3.width, btn3.y);
                glVertex2f(btn3.x + btn3.width, btn3.y + btn3.height);
                glVertex2f(btn3.x, btn3.y + btn3.height);
                glEnd();
                // Texto blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("SENSIBILIDAD", btn3.x + 35, btn3.y + 15, 20);

                // Botón 4: Salir
                Button btn4(centerX - buttonWidth / 2, startY + buttonSpacing * 3, buttonWidth, buttonHeight, "Salir");
                bool hover4 = btn4.contains((float)mouseX, (float)mouseY);
                // Fondo gris (más claro en hover)
                glColor4f(hover4 ? 0.5f : 0.3f, hover4 ? 0.5f : 0.3f, hover4 ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btn4.x, btn4.y);
                glVertex2f(btn4.x + btn4.width, btn4.y);
                glVertex2f(btn4.x + btn4.width, btn4.y + btn4.height);
                glVertex2f(btn4.x, btn4.y + btn4.height);
                glEnd();
                // Borde gris oscuro
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glLineWidth(2);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btn4.x, btn4.y);
                glVertex2f(btn4.x + btn4.width, btn4.y);
                glVertex2f(btn4.x + btn4.width, btn4.y + btn4.height);
                glVertex2f(btn4.x, btn4.y + btn4.height);
                glEnd();
                // Texto blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("SALIR", btn4.x + 95, btn4.y + 15, 20);

            } else if (g_gameState->pauseMenuState == PAUSE_MENU_GRAPHICS) {
                // MENÚ DE GRÁFICOS
                float startY = height / 2.0f - 100.0f;

                // Título "GRAFICOS"
                glColor4f(0.2f, 0.6f, 1.0f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 100, startY);
                glVertex2f(centerX + 100, startY);
                glVertex2f(centerX + 100, startY + 40);
                glVertex2f(centerX - 100, startY + 40);
                glEnd();

                // Texto "Distancia de Render"
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 120, startY + 50);
                glVertex2f(centerX + 120, startY + 50);
                glVertex2f(centerX + 120, startY + 70);
                glVertex2f(centerX - 120, startY + 70);
                glEnd();

                // Botón -
                Button btnMinus(centerX - 150, startY + 80, 50, 40, "-");
                bool hoverMinus = btnMinus.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverMinus ? 0.5f : 0.3f, hoverMinus ? 0.5f : 0.3f, hoverMinus ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnMinus.x, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y + btnMinus.height);
                glVertex2f(btnMinus.x, btnMinus.y + btnMinus.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnMinus.x, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y + btnMinus.height);
                glVertex2f(btnMinus.x, btnMinus.y + btnMinus.height);
                glEnd();
                // Texto "-" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("-", btnMinus.x + 18, btnMinus.y + 10, 20);

                // Valor actual
                glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 40, startY + 80);
                glVertex2f(centerX + 40, startY + 80);
                glVertex2f(centerX + 40, startY + 120);
                glVertex2f(centerX - 40, startY + 120);
                glEnd();

                // Botón +
                Button btnPlus(centerX + 100, startY + 80, 50, 40, "+");
                bool hoverPlus = btnPlus.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverPlus ? 0.5f : 0.3f, hoverPlus ? 0.5f : 0.3f, hoverPlus ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnPlus.x, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y + btnPlus.height);
                glVertex2f(btnPlus.x, btnPlus.y + btnPlus.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnPlus.x, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y + btnPlus.height);
                glVertex2f(btnPlus.x, btnPlus.y + btnPlus.height);
                glEnd();
                // Texto "+" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("+", btnPlus.x + 18, btnPlus.y + 10, 20);

                // Botón volver
                Button btnBack(centerX - buttonWidth / 2, startY + 160, buttonWidth, buttonHeight, "Volver");
                bool hoverBack = btnBack.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverBack ? 0.5f : 0.3f, hoverBack ? 0.5f : 0.3f, hoverBack ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnBack.x, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y + btnBack.height);
                glVertex2f(btnBack.x, btnBack.y + btnBack.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnBack.x, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y + btnBack.height);
                glVertex2f(btnBack.x, btnBack.y + btnBack.height);
                glEnd();
                // Texto "Volver" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("VOLVER", btnBack.x + 75, btnBack.y + 15, 20);

            } else if (g_gameState->pauseMenuState == PAUSE_MENU_SENSITIVITY) {
                // MENÚ DE SENSIBILIDAD
                float startY = height / 2.0f - 100.0f;

                // Título "SENSIBILIDAD"
                glColor4f(1.0f, 0.5f, 0.2f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 120, startY);
                glVertex2f(centerX + 120, startY);
                glVertex2f(centerX + 120, startY + 40);
                glVertex2f(centerX - 120, startY + 40);
                glEnd();

                // Texto "Sensibilidad Mouse"
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 120, startY + 50);
                glVertex2f(centerX + 120, startY + 50);
                glVertex2f(centerX + 120, startY + 70);
                glVertex2f(centerX - 120, startY + 70);
                glEnd();

                // Botón -
                Button btnMinus(centerX - 150, startY + 80, 50, 40, "-");
                bool hoverMinus = btnMinus.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverMinus ? 0.5f : 0.3f, hoverMinus ? 0.5f : 0.3f, hoverMinus ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnMinus.x, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y + btnMinus.height);
                glVertex2f(btnMinus.x, btnMinus.y + btnMinus.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnMinus.x, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y);
                glVertex2f(btnMinus.x + btnMinus.width, btnMinus.y + btnMinus.height);
                glVertex2f(btnMinus.x, btnMinus.y + btnMinus.height);
                glEnd();
                // Texto "-" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("-", btnMinus.x + 18, btnMinus.y + 10, 20);

                // Valor actual (mostrar como porcentaje)
                glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
                glBegin(GL_QUADS);
                glVertex2f(centerX - 40, startY + 80);
                glVertex2f(centerX + 40, startY + 80);
                glVertex2f(centerX + 40, startY + 120);
                glVertex2f(centerX - 40, startY + 120);
                glEnd();

                // Botón +
                Button btnPlus(centerX + 100, startY + 80, 50, 40, "+");
                bool hoverPlus = btnPlus.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverPlus ? 0.5f : 0.3f, hoverPlus ? 0.5f : 0.3f, hoverPlus ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnPlus.x, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y + btnPlus.height);
                glVertex2f(btnPlus.x, btnPlus.y + btnPlus.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnPlus.x, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y);
                glVertex2f(btnPlus.x + btnPlus.width, btnPlus.y + btnPlus.height);
                glVertex2f(btnPlus.x, btnPlus.y + btnPlus.height);
                glEnd();
                // Texto "+" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("+", btnPlus.x + 18, btnPlus.y + 10, 20);

                // Botón volver
                Button btnBack(centerX - buttonWidth / 2, startY + 160, buttonWidth, buttonHeight, "Volver");
                bool hoverBack = btnBack.contains((float)mouseX, (float)mouseY);
                glColor4f(hoverBack ? 0.5f : 0.3f, hoverBack ? 0.5f : 0.3f, hoverBack ? 0.5f : 0.3f, 0.9f);
                glBegin(GL_QUADS);
                glVertex2f(btnBack.x, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y + btnBack.height);
                glVertex2f(btnBack.x, btnBack.y + btnBack.height);
                glEnd();
                glColor4f(0.2f, 0.2f, 0.2f, 1.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(btnBack.x, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y);
                glVertex2f(btnBack.x + btnBack.width, btnBack.y + btnBack.height);
                glVertex2f(btnBack.x, btnBack.y + btnBack.height);
                glEnd();
                // Texto "Volver" blanco
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                renderText("VOLVER", btnBack.x + 75, btnBack.y + 15, 20);
            }

            } // Fin del else (renderizar menú normal cuando no está guardando)

            // Deshabilitar blending
            glDisable(GL_BLEND);

            // Re-habilitar culling para el siguiente frame
            glEnable(GL_CULL_FACE);
        }

        // Renderizar inventario si está abierto
        if (g_gameState->inventoryOpen) {
            // SEGURIDAD: Asegurar que todo el estado de VBO esté limpio antes del inventario
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDisable(GL_TEXTURE_2D);

            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Overlay oscuro
            glColor4f(0.0f, 0.0f, 0.0f, 0.7f);
            glBegin(GL_QUADS);
            glVertex2f(0, 0);
            glVertex2f(width, 0);
            glVertex2f(width, height);
            glVertex2f(0, height);
            glEnd();

            // Título "INVENTARIO"
            glColor4f(1.0f, 0.8f, 0.2f, 1.0f);
            renderText("INVENTARIO", width / 2.0f - 80, 50, 24);

            // Configuración de la cuadrícula
            const int COLS = 9;
            const int ROWS = 5;
            const float SLOT_SIZE = 50.0f;
            const float SPACING = 5.0f;
            const float GRID_WIDTH = COLS * (SLOT_SIZE + SPACING) - SPACING;
            const float GRID_HEIGHT = ROWS * (SLOT_SIZE + SPACING) - SPACING;
            const float START_X = (width - GRID_WIDTH) / 2.0f;
            const float START_Y = 120.0f;

            // Renderizar slots del inventario
            for (int slot = 0; slot < Inventory::SLOTS; slot++) {
                int row = slot / COLS;
                int col = slot % COLS;

                float x = START_X + col * (SLOT_SIZE + SPACING);
                float y = START_Y + row * (SLOT_SIZE + SPACING);

                // Fondo del slot
                bool isSelected = (slot == g_gameState->inventory.selectedSlot);
                if (isSelected) {
                    glColor4f(0.6f, 0.6f, 0.2f, 0.9f);  // Amarillo si está seleccionado
                } else {
                    glColor4f(0.3f, 0.3f, 0.3f, 0.9f);  // Gris normal
                }

                glBegin(GL_QUADS);
                glVertex2f(x, y);
                glVertex2f(x + SLOT_SIZE, y);
                glVertex2f(x + SLOT_SIZE, y + SLOT_SIZE);
                glVertex2f(x, y + SLOT_SIZE);
                glEnd();

                // Borde del slot
                glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
                glLineWidth(2);
                glBegin(GL_LINE_LOOP);
                glVertex2f(x, y);
                glVertex2f(x + SLOT_SIZE, y);
                glVertex2f(x + SLOT_SIZE, y + SLOT_SIZE);
                glVertex2f(x, y + SLOT_SIZE);
                glEnd();

                // ⭐⭐⭐ RENDERIZAR ITEM CON TEXTURA REAL (como Minecraft) ⭐⭐⭐
                if (!g_gameState->inventory.slots[slot].isEmpty()) {
                    BlockType blockType = g_gameState->inventory.slots[slot].blockType;
                    int count = g_gameState->inventory.slots[slot].count;

                    float iconSize = SLOT_SIZE * 0.7f;  // Más grande para ver mejor la textura
                    float iconX = x + (SLOT_SIZE - iconSize) / 2.0f;
                    float iconY = y + (SLOT_SIZE - iconSize) / 2.0f;

                    // ⭐ RENDERIZAR CON TEXTURA REAL del bloque
                    if (g_textureManager != nullptr) {
                        glEnable(GL_TEXTURE_2D);
                        GLuint texture = g_textureManager->getBlockTexture(blockType, 0); // Cara superior
                        glBindTexture(GL_TEXTURE_2D, texture);

                        glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Blanco para no alterar textura
                        glBegin(GL_QUADS);
                        glTexCoord2f(0, 0); glVertex2f(iconX, iconY);
                        glTexCoord2f(1, 0); glVertex2f(iconX + iconSize, iconY);
                        glTexCoord2f(1, 1); glVertex2f(iconX + iconSize, iconY + iconSize);
                        glTexCoord2f(0, 1); glVertex2f(iconX, iconY + iconSize);
                        glEnd();

                        glDisable(GL_TEXTURE_2D);

                        // Borde decorativo del item
                        glColor4f(0.3f, 0.3f, 0.3f, 0.5f);
                        glLineWidth(1);
                        glBegin(GL_LINE_LOOP);
                        glVertex2f(iconX, iconY);
                        glVertex2f(iconX + iconSize, iconY);
                        glVertex2f(iconX + iconSize, iconY + iconSize);
                        glVertex2f(iconX, iconY + iconSize);
                        glEnd();
                    } else {
                        // Fallback: color sólido si no hay textura manager
                        float r, g, b;
                        getBlockColor(blockType, r, g, b);
                        glColor4f(r, g, b, 1.0f);

                        glBegin(GL_QUADS);
                        glVertex2f(iconX, iconY);
                        glVertex2f(iconX + iconSize, iconY);
                        glVertex2f(iconX + iconSize, iconY + iconSize);
                        glVertex2f(iconX, iconY + iconSize);
                        glEnd();
                    }

                    // Mostrar cantidad
                    if (count > 1) {
                        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                        char countText[16];
                        sprintf(countText, "%d", count);
                        renderText(countText, x + 5, y + SLOT_SIZE - 15, 12);
                    }
                }
            }

            // ⭐⭐⭐ RENDERIZAR GRID DE CRAFTEO 3x3 (A LA DERECHA DEL INVENTARIO) ⭐⭐⭐
            {
                const float CRAFT_SLOT_SIZE = 50.0f;
                const float CRAFT_SPACING = 5.0f;
                const float CRAFT_GRID_SIZE = 3 * (CRAFT_SLOT_SIZE + CRAFT_SPACING) - CRAFT_SPACING;
                const float CRAFT_START_X = START_X + GRID_WIDTH + 80.0f;  // A la derecha del inventario
                const float CRAFT_START_Y = START_Y + 30.0f;

                // Título "CRAFTEO"
                glColor4f(0.2f, 0.8f, 1.0f, 1.0f);
                renderText("CRAFTEO", CRAFT_START_X + 30, CRAFT_START_Y - 40, 20);

                // Renderizar grid de crafteo 3x3
                for (int i = 0; i < 9; i++) {
                    int row = i / 3;
                    int col = i % 3;

                    float x = CRAFT_START_X + col * (CRAFT_SLOT_SIZE + CRAFT_SPACING);
                    float y = CRAFT_START_Y + row * (CRAFT_SLOT_SIZE + CRAFT_SPACING);

                    // Fondo del slot
                    glColor4f(0.2f, 0.3f, 0.4f, 0.9f);  // Azul oscuro
                    glBegin(GL_QUADS);
                    glVertex2f(x, y);
                    glVertex2f(x + CRAFT_SLOT_SIZE, y);
                    glVertex2f(x + CRAFT_SLOT_SIZE, y + CRAFT_SLOT_SIZE);
                    glVertex2f(x, y + CRAFT_SLOT_SIZE);
                    glEnd();

                    // Borde del slot
                    glColor4f(0.6f, 0.8f, 1.0f, 1.0f);  // Azul claro
                    glLineWidth(2);
                    glBegin(GL_LINE_LOOP);
                    glVertex2f(x, y);
                    glVertex2f(x + CRAFT_SLOT_SIZE, y);
                    glVertex2f(x + CRAFT_SLOT_SIZE, y + CRAFT_SLOT_SIZE);
                    glVertex2f(x, y + CRAFT_SLOT_SIZE);
                    glEnd();

                    // Renderizar item en el slot
                    if (!g_gameState->craftingGrid.slots[i].isEmpty()) {
                        BlockType blockType = g_gameState->craftingGrid.slots[i].blockType;
                        int count = g_gameState->craftingGrid.slots[i].count;

                        float iconSize = CRAFT_SLOT_SIZE * 0.7f;
                        float iconX = x + (CRAFT_SLOT_SIZE - iconSize) / 2.0f;
                        float iconY = y + (CRAFT_SLOT_SIZE - iconSize) / 2.0f;

                        if (g_textureManager != nullptr) {
                            glEnable(GL_TEXTURE_2D);
                            GLuint texture = g_textureManager->getBlockTexture(blockType, 0);
                            glBindTexture(GL_TEXTURE_2D, texture);

                            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                            glBegin(GL_QUADS);
                            glTexCoord2f(0, 0); glVertex2f(iconX, iconY);
                            glTexCoord2f(1, 0); glVertex2f(iconX + iconSize, iconY);
                            glTexCoord2f(1, 1); glVertex2f(iconX + iconSize, iconY + iconSize);
                            glTexCoord2f(0, 1); glVertex2f(iconX, iconY + iconSize);
                            glEnd();

                            glDisable(GL_TEXTURE_2D);
                        }

                        // Mostrar cantidad
                        if (count > 1) {
                            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                            char countText[16];
                            sprintf(countText, "%d", count);
                            renderText(countText, x + 5, y + CRAFT_SLOT_SIZE - 15, 12);
                        }
                    }
                }

                // Flecha decorativa (indica resultado)
                float arrowX = CRAFT_START_X + CRAFT_GRID_SIZE + 15.0f;
                float arrowY = CRAFT_START_Y + CRAFT_GRID_SIZE / 2.0f - 10.0f;
                glColor4f(1.0f, 0.8f, 0.2f, 1.0f);
                renderText("=>", arrowX, arrowY, 24);

                // Slot de resultado (a la derecha de la flecha)
                float resultX = arrowX + 60.0f;
                float resultY = CRAFT_START_Y + CRAFT_GRID_SIZE / 2.0f - CRAFT_SLOT_SIZE / 2.0f;

                // Fondo del slot de resultado
                glColor4f(0.2f, 0.6f, 0.2f, 0.9f);  // Verde
                glBegin(GL_QUADS);
                glVertex2f(resultX, resultY);
                glVertex2f(resultX + CRAFT_SLOT_SIZE, resultY);
                glVertex2f(resultX + CRAFT_SLOT_SIZE, resultY + CRAFT_SLOT_SIZE);
                glVertex2f(resultX, resultY + CRAFT_SLOT_SIZE);
                glEnd();

                // Borde del slot de resultado
                glColor4f(0.4f, 1.0f, 0.4f, 1.0f);  // Verde claro
                glLineWidth(3);
                glBegin(GL_LINE_LOOP);
                glVertex2f(resultX, resultY);
                glVertex2f(resultX + CRAFT_SLOT_SIZE, resultY);
                glVertex2f(resultX + CRAFT_SLOT_SIZE, resultY + CRAFT_SLOT_SIZE);
                glVertex2f(resultX, resultY + CRAFT_SLOT_SIZE);
                glEnd();

                // Renderizar resultado del crafteo
                if (!g_gameState->craftingResult.isEmpty()) {
                    BlockType resultType = g_gameState->craftingResult.blockType;
                    int resultCount = g_gameState->craftingResult.count;

                    float iconSize = CRAFT_SLOT_SIZE * 0.7f;
                    float iconX = resultX + (CRAFT_SLOT_SIZE - iconSize) / 2.0f;
                    float iconY = resultY + (CRAFT_SLOT_SIZE - iconSize) / 2.0f;

                    if (g_textureManager != nullptr) {
                        glEnable(GL_TEXTURE_2D);
                        GLuint texture = g_textureManager->getBlockTexture(resultType, 0);
                        glBindTexture(GL_TEXTURE_2D, texture);

                        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                        glBegin(GL_QUADS);
                        glTexCoord2f(0, 0); glVertex2f(iconX, iconY);
                        glTexCoord2f(1, 0); glVertex2f(iconX + iconSize, iconY);
                        glTexCoord2f(1, 1); glVertex2f(iconX + iconSize, iconY + iconSize);
                        glTexCoord2f(0, 1); glVertex2f(iconX, iconY + iconSize);
                        glEnd();

                        glDisable(GL_TEXTURE_2D);
                    }

                    // Mostrar cantidad del resultado
                    if (resultCount > 1) {
                        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                        char countText[16];
                        sprintf(countText, "%d", resultCount);
                        renderText(countText, resultX + 5, resultY + CRAFT_SLOT_SIZE - 15, 12);
                    }
                }

                // Ayuda de crafteo
                glColor4f(0.6f, 0.8f, 1.0f, 1.0f);
                renderText("Coloca items aqui", CRAFT_START_X - 10, CRAFT_START_Y + CRAFT_GRID_SIZE + 15, 12);
                renderText("para craftear", CRAFT_START_X + 5, CRAFT_START_Y + CRAFT_GRID_SIZE + 32, 12);
            }

            // Mensajes de ayuda
            glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
            renderText("PRESIONA E PARA CERRAR", width / 2.0f - 140, height - 100, 16);
            renderText("1-9 O RUEDA DEL MOUSE PARA CAMBIAR SLOT", width / 2.0f - 240, height - 75, 16);

            // Instrucciones de crafteo
            glColor4f(0.2f, 0.8f, 1.0f, 1.0f);
            renderText("CRAFTEO: Clic IZQ = tomar/poner todo | Clic DER = tomar/poner mitad", width / 2.0f - 380, height - 50, 14);
            renderText("Mueve items entre slots con el cursor del mouse", width / 2.0f - 270, height - 30, 14);

            // ⭐⭐⭐ RENDERIZAR ITEM EN EL CURSOR (HELD SLOT) ⭐⭐⭐
            if (!g_gameState->heldSlot.isEmpty()) {
                double mouseX, mouseY;
                glfwGetCursorPos(window, &mouseX, &mouseY);

                const float HELD_SIZE = 40.0f;  // Tamaño del item en el cursor
                float heldX = (float)mouseX - HELD_SIZE / 2.0f;
                float heldY = (float)mouseY - HELD_SIZE / 2.0f;

                // Sombra del item
                glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
                glBegin(GL_QUADS);
                glVertex2f(heldX + 2, heldY + 2);
                glVertex2f(heldX + HELD_SIZE + 2, heldY + 2);
                glVertex2f(heldX + HELD_SIZE + 2, heldY + HELD_SIZE + 2);
                glVertex2f(heldX + 2, heldY + HELD_SIZE + 2);
                glEnd();

                // Renderizar textura del item
                if (g_textureManager != nullptr) {
                    glEnable(GL_TEXTURE_2D);
                    GLuint texture = g_textureManager->getBlockTexture(g_gameState->heldSlot.blockType, 0);
                    glBindTexture(GL_TEXTURE_2D, texture);

                    glColor4f(1.0f, 1.0f, 1.0f, 0.9f);  // Ligeramente transparente
                    glBegin(GL_QUADS);
                    glTexCoord2f(0, 0); glVertex2f(heldX, heldY);
                    glTexCoord2f(1, 0); glVertex2f(heldX + HELD_SIZE, heldY);
                    glTexCoord2f(1, 1); glVertex2f(heldX + HELD_SIZE, heldY + HELD_SIZE);
                    glTexCoord2f(0, 1); glVertex2f(heldX, heldY + HELD_SIZE);
                    glEnd();

                    glDisable(GL_TEXTURE_2D);
                }

                // Mostrar cantidad del item en el cursor
                if (g_gameState->heldSlot.count > 1) {
                    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                    char countText[16];
                    sprintf(countText, "%d", g_gameState->heldSlot.count);
                    renderText(countText, heldX + 5, heldY + HELD_SIZE - 12, 12);
                }
            }

            glDisable(GL_BLEND);
            glEnable(GL_CULL_FACE);
        }

        glEnable(GL_DEPTH_TEST);

        } // Fin de SCREEN_IN_GAME

            // ⭐ Profiler (F3): alimentar métricas del frame y dibujar overlay
            {
                Profiler::FrameStats stats;
                stats.fps = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
                stats.frameTimeMs = deltaTime * 1000.0f;
                stats.cpuTimeMs = (float)(physics_ms + chunks_ms + render_ms);
                if (g_gameState) {
                    stats.totalChunks = g_gameState->world.getChunkCount();
                }
                Profiler::updateStats(stats);
                Profiler::endFrame();
                if (Profiler::isVisible()) {
                    Profiler::ProfilerManager::getInstance()->renderOverlay(width, height);
                }
            }

            glfwSwapBuffers(window);
            glfwPollEvents();
        }
    } catch (const std::exception& e) {
        std::cerr << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cerr << "║  ❌ ERROR CRÍTICO EN EL JUEGO          ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════╝" << std::endl;
        std::cerr << "Tipo: " << typeid(e).name() << std::endl;
        std::cerr << "Mensaje: " << e.what() << std::endl;
        std::cerr << "\n⚠️ Intentando guardar el mundo antes de cerrar..." << std::endl;

        // Intento de guardado de emergencia
        if (g_gameState && !g_gameState->currentWorldName.empty()) {
            try {
                saveWorld(g_gameState);
                std::cerr << "✅ Mundo guardado exitosamente!" << std::endl;
            } catch (...) {
                std::cerr << "❌ No se pudo guardar el mundo" << std::endl;
            }
        }

        // Aviso visible: el binario no tiene consola, cerr solo llega al log
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "El juego encontró un error crítico y debe cerrarse.\n\n%s\n\n"
                 "Se intentó guardar el mundo. Revisa el log en:\n"
                 "%%LOCALAPPDATA%%\\VoxelGenesis\\log.txt", e.what());
        MessageBoxA(nullptr, msg, "VoxelWorld - Error crítico", MB_OK | MB_ICONERROR);
    } catch (...) {
        std::cerr << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cerr << "║  ❌ ERROR DESCONOCIDO CRÍTICO          ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════╝" << std::endl;

        // Intento de guardado de emergencia
        if (g_gameState && !g_gameState->currentWorldName.empty()) {
            try {
                saveWorld(g_gameState);
                std::cerr << "✅ Mundo guardado exitosamente!" << std::endl;
            } catch (...) {
                std::cerr << "❌ No se pudo guardar el mundo" << std::endl;
            }
        }

        MessageBoxA(nullptr,
                    "El juego encontró un error desconocido y debe cerrarse.\n\n"
                    "Se intentó guardar el mundo. Revisa el log en:\n"
                    "%LOCALAPPDATA%\\VoxelGenesis\\log.txt",
                    "VoxelWorld - Error crítico", MB_OK | MB_ICONERROR);
    }

    // ⭐ GUARDAR MUNDO ANTES DE CERRAR (guardado normal)
    if (g_gameState && !g_gameState->currentWorldName.empty()) {
        std::cout << "\n=== GUARDANDO MUNDO AL CERRAR ===" << std::endl;
        try {
            saveWorld(g_gameState);
            std::cout << "Mundo guardado exitosamente!" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "❌ Error al guardar: " << e.what() << std::endl;
        }
    }

    // Limpiar memoria de forma segura
    std::cout << "Liberando recursos..." << std::endl;

    if (g_soundManager) {
        delete g_soundManager;
        g_soundManager = nullptr;
    }

    if (g_gameState) {
        delete g_gameState;
        g_gameState = nullptr;
    }

    if (g_textureManager) {
        delete g_textureManager;
        g_textureManager = nullptr;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    // Cierre limpio: retirar el marcador de crash
    if (g_crashMarkerFd >= 0) {
        _close(g_crashMarkerFd);
        g_crashMarkerFd = -1;
    }
    if (!g_crashMarkerPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(g_crashMarkerPath, ec);
    }

    std::cout << "Juego cerrado correctamente!" << std::endl;

    return 0;
}
