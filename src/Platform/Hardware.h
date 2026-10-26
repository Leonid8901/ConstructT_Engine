#pragma once
// Определение процессора и мелкие платформенные функции (Windows / Linux, x86-64 / ARM64).
#include <cstdint>
#include <filesystem>
#include <string>

namespace ce::core {

struct CpuInfo {
    std::string vendor = "Unknown";  // AMD, Intel, ARM, Apple, Qualcomm, … (по CPUID / MIDR)
    std::string brand;               // строка модели, если ОС/CPUID её отдаёт
    std::string arch;                // x86-64, arm64, …
    uint32_t logical = 1;            // логических процессоров
    uint32_t physical = 1;           // физических ядер (модуль Bulldozer считается двумя ядрами)
    uint32_t perfCores = 0;          // гибрид: производительные ядра (Intel P / ARM big)
    uint32_t effCores = 0;           // гибрид: энергоэффективные ядра (Intel E / ARM LITTLE)
    bool smt = false;                // Hyper-Threading / SMT
    bool hybrid = false;
    bool sharedFpu = false;          // AMD Bulldozer…Excavator (семейство 15h): 2 ядра модуля делят FPU
    uint32_t family = 0;             // x86: семейство CPUID (с расширенным)
    bool sse42 = false, avx = false, avx2 = false, avx512 = false, neon = false;
    bool detected = false;           // false — значения по умолчанию (не удалось определить)
};

CpuInfo DetectCpu();
std::string Describe(const CpuInfo& cpu);

// Каталог запущенного exe (пустой путь, если ОС не сказала). std::filesystem::path — чтобы на Windows
// работали пути с кириллицей (имя пользователя), которые через std::string/ANSI ломаются.
std::filesystem::path ExecutableDir();
void LowerCurrentThreadPriority();  // фоновый поток: ОС охотнее отдаёт ему E-ядра / LITTLE и не мешает кадру
void ShowFatalError(const std::string& title, const std::string& message);  // окно с ошибкой на Windows (лог пишет вызывающий)

} // namespace ce::core
