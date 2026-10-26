#include "Platform/Settings.h"
#include "Core.h"
#include "Platform/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ce::core {

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Шаблон engine.json. Пишется рядом с exe при первом запуске, чтобы игроку было что править.
constexpr const char* kDefaultConfig = R"({
  "_readme": "auto = true: движок сам определяет CPU и GPU. auto = false: берёт vendor/type/threads ниже. Любое поле \"auto\" = решает движок.",
  "auto": true,

  "cpu": {
    "vendor": "auto",
    "threads": "auto"
  },
  "gpu": {
    "vendor": "auto",
    "type": "auto",
    "device": "auto"
  },
  "_gpu_help": "vendor: NVIDIA / AMD / Intel / Apple / ARM / Qualcomm / Software. type: discrete / integrated / software. device: номер GPU (0, 1, ...) или часть имени, работает всегда.",

  "settings": {
    "workerThreads": "auto",
    "vsync": "auto",
    "rayTracing": "auto",
    "msaa": "auto",
    "shadows": "auto",
    "shadowMapSize": "auto",
    "shadowDistance": "auto"
  },

  "window": {
    "platform": "auto"
  },
  "_window_help": "platform: auto / x11 / wayland (Linux) / win32",

  "audio": {
    "enabled": "auto",
    "master": "auto",
    "effects": "auto",
    "music": "auto",
    "voice": "auto",
    "ui": "auto"
  },
  "_audio_help": "громкость 0..1; enabled: false — без звука",

  "modules": {},
  "_modules_help": "настройки модулей игры: \"ИмяМодуля\": { \"enabled\": false, ...свои поля модуля }. enabled: false — модуль не загружается, например скримеры"
}
)";

std::string ReadString(const Json& v, const std::string& def = "auto") {
    if (v.IsString())
        return v.AsString();
    if (v.IsNumber()) {
        std::ostringstream ss;
        ss << v.AsNumber();
        return ss.str();
    }
    return def;
}

int ReadInt(const Json& v) {
    if (v.IsNumber())
        return std::max(0, static_cast<int>(v.AsNumber()));
    return -1;  // "auto", null, отсутствует
}

int ReadBool(const Json& v) {
    if (v.IsBool())
        return v.AsBool() ? 1 : 0;
    if (v.IsNumber())
        return v.AsNumber() != 0 ? 1 : 0;
    if (v.IsString()) {
        const std::string s = Lower(v.AsString());
        if (s == "true" || s == "on" || s == "yes") return 1;
        if (s == "false" || s == "off" || s == "no") return 0;
    }
    return -1;
}

} // namespace

const char* ToString(GpuClass c) {
    switch (c) {
    case GpuClass::Discrete: return "discrete";
    case GpuClass::Integrated: return "integrated";
    case GpuClass::Virtual: return "virtual";
    case GpuClass::Software: return "software";
    default: return "unknown";
    }
}

std::string GpuVendorName(uint32_t id) {
    switch (id) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x106B: return "Apple";
    case 0x13B5: return "ARM";        // Mali
    case 0x5143: return "Qualcomm";   // Adreno
    case 0x1010: return "ImgTec";     // PowerVR
    case 0x14E4: return "Broadcom";   // VideoCore (Raspberry Pi)
    case 0x144D: return "Samsung";
    case 0x1AE0: return "Google";     // SwiftShader
    case 0x10005: return "Mesa";      // llvmpipe / lavapipe
    case 0x15AD: return "VMware";
    case 0x1414: return "Microsoft";
    case 0x1D17: return "Zhaoxin";
    case 0x1E3E: return "Moore Threads";
    default: {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%04X", id);
        return buf;
    }
    }
}

std::string Describe(const GpuInfo& g) {
    std::string s = g.vendor + " | " + g.name + " | " + ToString(g.type) + " | VRAM " + std::to_string(g.vram >> 20) +
                    " МБ";
    if (g.type == GpuClass::Integrated || g.type == GpuClass::Software)
        s += " (общая с CPU)";
    else if (g.hostVisibleVram > (256ull << 20))
        s += " | Resizable BAR " + std::to_string(g.hostVisibleVram >> 20) + " МБ";
    else if (g.hostVisibleVram > 0)
        s += " | BAR " + std::to_string(g.hostVisibleVram >> 20) + " МБ";
    if (!g.driver.empty())
        s += " | " + g.driver;
    return s;
}

UserConfig LoadUserConfig(const std::filesystem::path& path, bool createIfMissing) {
    UserConfig u;
    if (path.empty())
        return u;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (createIfMissing) {
            std::ofstream out(path, std::ios::binary);
            if (out)
                out << kDefaultConfig;  // не получилось (папка только для чтения) — не страшно
        }
        return u;
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF)  // BOM от Блокнота
        text.erase(0, 3);

    Json root;
    std::string error;
    if (!Json::Parse(text, root, error) || !root.IsObject()) {
        Log("engine.json: " + (error.empty() ? std::string("корень должен быть объектом") : error) +
            " — файл проигнорирован, работают настройки по умолчанию");
        return u;
    }
    u.loaded = true;
    if (root["auto"].IsBool())
        u.autoDetect = root["auto"].AsBool();
    const Json& cpu = root["cpu"];
    u.cpuVendor = ReadString(cpu["vendor"]);
    u.cpuThreads = ReadInt(cpu["threads"]);
    const Json& gpu = root["gpu"];
    u.gpuVendor = ReadString(gpu["vendor"]);
    u.gpuType = Lower(ReadString(gpu["type"]));
    u.gpuDevice = ReadString(gpu["device"]);
    if (root["modules"].IsObject())
        u.modules = root["modules"];
    const Json& st = root["settings"];
    u.workerThreads = ReadInt(st["workerThreads"]);
    u.vsync = ReadBool(st["vsync"]);
    u.rayTracing = ReadBool(st["rayTracing"]);
    u.shadows = ReadBool(st["shadows"]);
    u.msaa = ReadInt(st["msaa"]);
    u.shadowMapSize = ReadInt(st["shadowMapSize"]);
    u.shadowDistance = st["shadowDistance"].IsNumber() ? static_cast<float>(st["shadowDistance"].AsNumber()) : -1.0f;
    u.windowPlatform = Lower(ReadString(root["window"]["platform"]));
    const Json& au = root["audio"];
    u.audioEnabled = ReadBool(au["enabled"]);
    auto vol = [](const Json& v) { return v.IsNumber() ? std::max(0.0f, static_cast<float>(v.AsNumber())) : -1.0f; };
    u.audioMaster = vol(au["master"]);
    const char* names[4] = {"effects", "music", "voice", "ui"};
    for (int g = 0; g < 4; ++g)
        u.audioGroups[g] = vol(au[names[g]]);
    return u;
}

void ApplyManualHardware(const UserConfig& u, CpuInfo& cpu, GpuInfo& gpu) {
    if (u.autoDetect)
        return;
    if (Lower(u.cpuVendor) != "auto")
        cpu.vendor = u.cpuVendor;
    if (u.cpuThreads > 0) {
        cpu.logical = static_cast<uint32_t>(u.cpuThreads);
        cpu.physical = std::min(cpu.physical, cpu.logical);
    }
    if (Lower(u.gpuVendor) != "auto")
        gpu.vendor = u.gpuVendor;
    if (u.gpuType == "discrete") gpu.type = GpuClass::Discrete;
    else if (u.gpuType == "integrated") gpu.type = GpuClass::Integrated;
    else if (u.gpuType == "software") gpu.type = GpuClass::Software;
}

RuntimeSettings ResolveSettings(const EngineConfig& cfg, const CpuInfo& cpu, const GpuInfo& gpu, const UserConfig& u) {
    RuntimeSettings s;

    // ---- профиль GPU: класс решает больше, чем вендор ----
    bool allowRt = true;
    uint32_t msaaCap = 8;
    const std::string vendor = Lower(gpu.vendor);
    const bool tiler = vendor == "arm" || vendor == "qualcomm" || vendor == "imgtec" || vendor == "apple" ||
                       vendor == "broadcom" || vendor == "samsung";  // мобильные тайловые GPU
    std::string cls;
    switch (gpu.type) {
    case GpuClass::Software:
        cls = "software";
        allowRt = false;
        msaaCap = 1;
        s.shadowMapSize = 1024;
        s.shadowDistance = 40;
        break;
    case GpuClass::Integrated:
    case GpuClass::Virtual:
    case GpuClass::Unknown:
        cls = gpu.type == GpuClass::Integrated ? "integrated" : ToString(gpu.type);
        allowRt = false;  // RT на встроенной графике есть (RDNA2 APU, Arc iGPU), но кадр он съедает целиком
        msaaCap = tiler ? 4 : 2;  // тайловым MSAA почти бесплатен: resolve прямо в тайловой памяти
        s.shadowMapSize = 1024;
        s.shadowDistance = 60;
        break;
    case GpuClass::Discrete:
        cls = "discrete";
        if (gpu.vram > 0 && gpu.vram < (3ull << 30)) {  // 2 ГБ и меньше: экономим память и заливку
            cls = "discrete-low";
            allowRt = false;
            msaaCap = 4;
            s.shadowMapSize = 1024;
            s.shadowDistance = 60;
        }
        break;
    }
    s.profile = gpu.vendor + " " + cls;

    // ---- профиль CPU ----
    // main + фоновый поток уже есть; воркеров = логические − 2. На гибридах и Bulldozer счёт тот же:
    // перекосы скорости ядер выравнивает work stealing, а фоновый поток идёт с пониженным приоритетом.
    uint32_t workers = cpu.logical > 2 ? cpu.logical - 2 : 1;
    workers = std::min(workers, 32u);  // дальше выигрыша нет: стадии короткие, кражи дороже

    // ---- слой 1+2: пожелания игры, урезанные профилем ----
    s.workerThreads = cfg.workerThreads ? cfg.workerThreads : workers;
    s.vsync = cfg.vsync;
    s.rayTracing = cfg.rayTracing && allowRt && gpu.rayQuery;
    s.msaa = std::max(1u, std::min(cfg.msaa, msaaCap));
    if (cfg.shadowMapSize)
        s.shadowMapSize = cfg.shadowMapSize;

    // ---- слой 3: engine.json ----
    if (u.workerThreads > 0) s.workerThreads = static_cast<uint32_t>(u.workerThreads);
    if (u.vsync >= 0) s.vsync = u.vsync != 0;
    if (u.rayTracing >= 0) s.rayTracing = u.rayTracing != 0 && gpu.rayQuery;
    if (u.shadows >= 0) s.shadows = u.shadows != 0;
    if (u.msaa > 0) s.msaa = static_cast<uint32_t>(u.msaa);
    if (u.shadowMapSize > 0) s.shadowMapSize = static_cast<uint32_t>(u.shadowMapSize);
    if (u.shadowDistance > 0) s.shadowDistance = u.shadowDistance;

    // Карта теней: степень двойки 512…8192
    uint32_t sm = 512;
    while (sm < s.shadowMapSize && sm < 8192)
        sm *= 2;
    s.shadowMapSize = sm;
    s.workerThreads = std::clamp(s.workerThreads, 1u, 64u);
    return s;
}

std::string Describe(const RuntimeSettings& s) {
    return "профиль " + s.profile + " | воркеров " + std::to_string(s.workerThreads) + " | RT " +
           (s.rayTracing ? "on" : "off") + " | MSAA " + std::to_string(s.msaa) + "x | тени " +
           (s.shadows ? std::to_string(s.shadowMapSize) + "² до " + std::to_string(static_cast<int>(s.shadowDistance)) + " м"
                      : "off") +
           " | vsync " + (s.vsync ? "on" : "off");
}

} // namespace ce::core
