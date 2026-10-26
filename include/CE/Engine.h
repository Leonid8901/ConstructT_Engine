#pragma once
#include <cstdint>
#include <string>

namespace ce {

struct EngineConfig {
    std::string title = "CustomEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool rayTracing = false;   // стартовое значение флага Amper RT
    float targetFps = 60.0f;   // бюджет кадра для UniX
    uint32_t workerThreads = 0; // 0 = авто (ядра - 2)
    uint32_t msaa = 4;          // сглаживание краёв: 1 (выкл), 2, 4, 8; больше, чем умеет GPU, — урежется
    uint32_t shadowMapSize = 0; // сторона каскадной карты теней; 0 = по профилю железа (2048 / 1024)
    bool audio = true;          // звук; без устройства вывода движок всё равно работает (молча)
    std::string configFile = "engine.json";  // рядом с exe; создаётся при первом запуске; "" — без файла
};

// Что движок узнал о железе и какие настройки выбрал (для меню настроек, логов, баг-репортов)
struct SystemInfo {
    std::string os;              // Windows / Linux
    std::string windowPlatform;  // Win32 / X11 / Wayland
    std::string cpuVendor, cpuName;
    uint32_t cpuCores = 0, cpuThreads = 0;
    bool cpuHybrid = false;
    std::string gpuVendor, gpuName, gpuType, gpuDriver;  // gpuType: discrete / integrated / software / virtual
    uint64_t gpuMemoryMB = 0;
    std::string profile;         // выбранный профиль, например "AMD discrete"
    bool configLoaded = false;   // engine.json прочитан
};

class Game {
public:
    virtual ~Game() = default;
    virtual void OnStart() {}     // создать сцену и задачи
    virtual void OnShutdown() {}
};

struct Engine {
    static int Run(Game& game, const EngineConfig& config = {});
    template <class G>
    static int Run(const EngineConfig& config = {}) {
        G game;
        return Run(game, config);
    }
    static void Quit();
    static double Time();
    static float DeltaTime();
    static uint64_t Frame();
    static const SystemInfo& System();
};

} // namespace ce
