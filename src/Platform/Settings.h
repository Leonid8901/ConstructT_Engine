#pragma once
// Профили железа и engine.json.
//
// Итоговые настройки собираются в три слоя (каждый следующий сильнее):
//   1. EngineConfig из кода игры — пожелания разработчика (rayTracing = «включить, если железо тянет»).
//   2. Профиль железа — по классу и вендору GPU и топологии CPU: урезает то, что железо не потянет.
//   3. engine.json рядом с exe — явный выбор игрока; "auto" или отсутствующее поле = не трогать.
// При "auto": false в engine.json вендор/класс GPU и CPU берутся из файла, а не из определения железа.
#include "CE/Engine.h"
#include "Platform/Hardware.h"
#include "Platform/Json.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ce::core {

enum class GpuClass : uint8_t { Discrete, Integrated, Virtual, Software, Unknown };

struct GpuInfo {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    std::string vendor = "Unknown";
    std::string name;
    std::string driver;           // имя и версия драйвера (VkPhysicalDeviceDriverProperties)
    GpuClass type = GpuClass::Unknown;
    uint32_t apiVersion = 0;
    uint64_t vram = 0;            // самая большая device-local куча
    uint64_t hostVisibleVram = 0; // device-local + host-visible: окно BAR (256 МБ) или Resizable BAR
    bool rayQuery = false;
};

const char* ToString(GpuClass c);
std::string GpuVendorName(uint32_t vendorId);
std::string Describe(const GpuInfo& gpu);

// Содержимое engine.json. -1 / "auto" — поле не задано.
struct UserConfig {
    bool loaded = false;
    bool autoDetect = true;
    std::string cpuVendor = "auto";
    int cpuThreads = -1;
    std::string gpuVendor = "auto";
    std::string gpuType = "auto";    // discrete / integrated / software
    std::string gpuDevice = "auto";  // индекс GPU или часть имени: работает и при "auto": true
    int workerThreads = -1;
    int vsync = -1, rayTracing = -1, shadows = -1;
    int msaa = -1, shadowMapSize = -1;
    float shadowDistance = -1;
    std::string windowPlatform = "auto";  // auto / x11 / wayland / win32
    int audioEnabled = -1;
    float audioMaster = -1;
    float audioGroups[4] = {-1, -1, -1, -1};  // effects, music, voice, ui
    Json modules;                             // раздел "modules": настройки модулей игры (читает ModuleSystem)
};

UserConfig LoadUserConfig(const std::filesystem::path& path, bool createIfMissing);

// auto = false: подменить определённые характеристики значениями из файла
void ApplyManualHardware(const UserConfig& user, CpuInfo& cpu, GpuInfo& gpu);

struct RuntimeSettings {
    std::string profile;
    uint32_t workerThreads = 1;
    bool vsync = true;
    bool rayTracing = false;
    uint32_t msaa = 1;
    bool shadows = true;
    uint32_t shadowMapSize = 2048;
    float shadowDistance = 80;
};

RuntimeSettings ResolveSettings(const EngineConfig& config, const CpuInfo& cpu, const GpuInfo& gpu,
                                const UserConfig& user);
std::string Describe(const RuntimeSettings& s);

} // namespace ce::core
