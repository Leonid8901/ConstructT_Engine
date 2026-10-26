#include "Platform/Hardware.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define CE_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ce::core {

namespace {

#ifdef CE_X86
void Cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#if defined(_MSC_VER)
    int v[4];
    __cpuidex(v, static_cast<int>(leaf), static_cast<int>(sub));
    for (int i = 0; i < 4; ++i)
        r[i] = static_cast<uint32_t>(v[i]);
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

uint64_t Xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (uint64_t(hi) << 32) | lo;
#endif
}

void DetectX86(CpuInfo& c) {
    c.arch = sizeof(void*) == 8 ? "x86-64" : "x86";
    uint32_t r[4];
    Cpuid(0, 0, r);
    const uint32_t maxLeaf = r[0];
    char vendor[13] = {};
    std::memcpy(vendor, &r[1], 4);
    std::memcpy(vendor + 4, &r[3], 4);
    std::memcpy(vendor + 8, &r[2], 4);
    const std::string v = vendor;
    c.vendor = v == "AuthenticAMD" ? "AMD"
               : v == "GenuineIntel" ? "Intel"
               : v == "HygonGenuine" ? "Hygon"
               : (v == "CentaurHauls" || v == "  Shanghai  ") ? "Zhaoxin"
                                                              : v;

    if (maxLeaf >= 1) {
        Cpuid(1, 0, r);
        uint32_t base = (r[0] >> 8) & 0xF, ext = (r[0] >> 20) & 0xFF;
        c.family = base == 0xF ? base + ext : base;
        c.sse42 = (r[2] >> 20) & 1;
        const bool osxsave = (r[2] >> 27) & 1;
        const bool avxHw = (r[2] >> 28) & 1;
        const uint64_t xcr0 = osxsave ? Xgetbv0() : 0;
        const bool avxOs = (xcr0 & 0x6) == 0x6;           // XMM + YMM сохраняются ОС
        const bool avx512Os = (xcr0 & 0xE6) == 0xE6;      // + opmask, ZMM
        c.avx = avxHw && avxOs;
        if (maxLeaf >= 7) {
            Cpuid(7, 0, r);
            c.avx2 = c.avx && ((r[1] >> 5) & 1);
            c.avx512 = avx512Os && ((r[1] >> 16) & 1);
            c.hybrid = (r[3] >> 15) & 1;  // Intel Alder Lake и новее; типы ядер узнаём у ОС
        }
    }
    Cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000004u) {
        char brand[49] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            Cpuid(0x80000002u + i, 0, r);
            std::memcpy(brand + i * 16, r, 16);
        }
        std::string b = brand;
        b.erase(0, b.find_first_not_of(' '));
        b.erase(b.find_last_not_of(' ') + 1);
        c.brand = b;
    }
    c.sharedFpu = c.vendor == "AMD" && c.family == 0x15;  // Bulldozer, Piledriver, Steamroller, Excavator
}
#endif

#ifndef _WIN32
std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s;
}

// "0-3,8,10-11" -> число процессоров
uint32_t CountCpuList(const std::string& list) {
    uint32_t n = 0;
    std::stringstream ss(list);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty())
            continue;
        const size_t dash = part.find('-');
        if (dash == std::string::npos) {
            ++n;
        } else {
            const int a = std::atoi(part.substr(0, dash).c_str()), b = std::atoi(part.substr(dash + 1).c_str());
            if (b >= a)
                n += static_cast<uint32_t>(b - a + 1);
        }
    }
    return n;
}

void DetectLinuxTopology(CpuInfo& c) {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    c.logical = online > 0 ? static_cast<uint32_t>(online) : std::max(1u, std::thread::hardware_concurrency());

    std::set<std::string> cores;
    std::vector<long> capacity;
    for (uint32_t i = 0; i < 8192; ++i) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i);
        std::error_code ec;
        if (!std::filesystem::exists(base, ec))
            break;
        std::string siblings = ReadFile(base + "/topology/core_cpus_list");
        if (siblings.empty())
            siblings = ReadFile(base + "/topology/thread_siblings_list");
        if (siblings.empty())
            continue;  // процессор выключен
        cores.insert(siblings);
        if (CountCpuList(siblings) > 1)
            c.smt = true;
        const std::string cap = ReadFile(base + "/cpu_capacity");  // ARM big.LITTLE
        if (!cap.empty())
            capacity.push_back(std::atol(cap.c_str()));
    }
    if (!cores.empty())
        c.physical = static_cast<uint32_t>(cores.size());

    // Intel гибрид: ядра P и E — отдельные PMU
    const std::string atom = ReadFile("/sys/devices/cpu_atom/cpus");
    const std::string core = ReadFile("/sys/devices/cpu_core/cpus");
    if (!atom.empty() && !core.empty()) {
        c.hybrid = true;
        c.effCores = CountCpuList(atom);
        c.perfCores = c.physical > c.effCores ? c.physical - c.effCores : 0;
    } else if (capacity.size() > 1) {
        const long maxCap = *std::max_element(capacity.begin(), capacity.end());
        const auto big = static_cast<uint32_t>(std::count(capacity.begin(), capacity.end(), maxCap));
        if (big < capacity.size()) {
            c.hybrid = true;
            c.perfCores = big;
            c.effCores = static_cast<uint32_t>(capacity.size()) - big;
        }
    }

    // ARM: вендор и модель из /proc/cpuinfo
    if (c.vendor == "Unknown" || c.brand.empty()) {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            auto value = [&] {
                const size_t p = line.find(':');
                std::string v = p == std::string::npos ? "" : line.substr(p + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                return v;
            };
            if (c.brand.empty() && (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0 ||
                                    line.rfind("Hardware", 0) == 0))
                c.brand = value();
            if (c.vendor == "Unknown" && line.rfind("CPU implementer", 0) == 0) {
                const long impl = std::strtol(value().c_str(), nullptr, 16);
                c.vendor = impl == 0x41 ? "ARM" : impl == 0x51 ? "Qualcomm" : impl == 0x61 ? "Apple"
                         : impl == 0x48 ? "HiSilicon" : impl == 0x4E ? "NVIDIA" : impl == 0xC0 ? "Ampere"
                         : impl == 0x46 ? "Fujitsu" : impl == 0x6D ? "Microsoft" : "ARM-compatible";
            }
        }
    }
}
#endif

#ifdef _WIN32
void DetectWindowsTopology(CpuInfo& c) {
    c.logical = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return;
    std::vector<uint8_t> buf(len);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len))
        return;
    uint32_t cores = 0;
    std::vector<BYTE> classes;
    for (DWORD off = 0; off < len;) {
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Relationship == RelationProcessorCore) {
            ++cores;
            if (info->Processor.Flags & LTP_PC_SMT)
                c.smt = true;
            classes.push_back(info->Processor.EfficiencyClass);  // 0 у негибридных; больше — производительнее
        }
        off += info->Size;
    }
    if (cores)
        c.physical = cores;
    if (!classes.empty()) {
        const BYTE maxClass = *std::max_element(classes.begin(), classes.end());
        const auto perf = static_cast<uint32_t>(std::count(classes.begin(), classes.end(), maxClass));
        if (perf < classes.size()) {
            c.hybrid = true;
            c.perfCores = perf;
            c.effCores = static_cast<uint32_t>(classes.size()) - perf;
        }
    }
}
#endif

} // namespace

CpuInfo DetectCpu() {
    CpuInfo c;
    c.logical = std::max(1u, std::thread::hardware_concurrency());
    c.physical = c.logical;
#ifdef CE_X86
    DetectX86(c);
#elif defined(__aarch64__) || defined(_M_ARM64)
    c.arch = "arm64";
    c.neon = true;
#else
    c.arch = "unknown";
#endif
#ifdef _WIN32
    DetectWindowsTopology(c);
#else
    DetectLinuxTopology(c);
#endif
    if (c.sharedFpu) {
        // Windows показывает модуль Bulldozer как ядро с SMT; по сути это 2 целочисленных ядра с общим FPU
        c.physical = c.logical;
        c.smt = false;
    }
    if (c.hybrid && c.perfCores == 0 && c.effCores == 0)
        c.hybrid = false;  // CPUID сказал «гибрид», но ОС типы ядер не отдала
    c.detected = true;
    return c;
}

std::string Describe(const CpuInfo& c) {
    std::string s = c.vendor + (c.brand.empty() ? "" : " | " + c.brand) + " | " + c.arch + " | " +
                    std::to_string(c.physical) + " ядер / " + std::to_string(c.logical) + " потоков";
    if (c.smt) s += ", SMT";
    if (c.hybrid) s += ", гибрид " + std::to_string(c.perfCores) + "P+" + std::to_string(c.effCores) + "E";
    if (c.sharedFpu) s += ", общий FPU в модуле";
    std::string isa;
    if (c.avx512) isa = "AVX-512";
    else if (c.avx2) isa = "AVX2";
    else if (c.avx) isa = "AVX";
    else if (c.sse42) isa = "SSE4.2";
    else if (c.neon) isa = "NEON";
    if (!isa.empty()) s += " | " + isa;
    return s;
}

std::filesystem::path ExecutableDir() {
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p.parent_path();
#endif
}

void LowerCurrentThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    // В Linux nice действует на отдельный поток (tid); поднимать nice можно без прав root
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
}

void ShowFatalError(const std::string& title, const std::string& message) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    MessageBoxW(nullptr, wide(message).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR);
#else
    (void)title;  // в Linux сообщение уже в логе (stdout/stderr), окна не показываем
    (void)message;
#endif
}

} // namespace ce::core
