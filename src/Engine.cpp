// Точка входа движка и главный цикл кадра.
//
//  Sys     (главный поток)   события окна, снимок ввода
//  Update  (UniX, параллельно) пользовательские задачи                 -> Flush мира
//  Simulate(UniX)             Grave.Step (Actor), внутри ParallelFor    -> события контактов -> Flush
//  Prepare (UniX)             Amper.Prepare (Actor), внутри ParallelFor
//  Render  (главный поток)   запись и отправка кадра в GPU (Engine)
//  EndFrame                  UniX: статистика, коэффициенты полезности, разгрузка, ребаланс лейнов
#include "Amper/Renderer.h"
#include "CE/Engine.h"
#include "CE/Input.h"
#include "Core.h"
#include "Audio/AudioSystem.h"
#include "Grave/Physics.h"
#include "Module/ModuleSystem.h"
#include "Nav/NavSystem.h"
#include "Timer/TimerSystem.h"
#include "Platform/Hardware.h"
#include "Platform/Input.h"
#include "Platform/Settings.h"
#include "Platform/Window.h"
#include "Profile.h"
#include "UniX/GpuLanes.h"
#include "UniX/Scheduler.h"
#include "Vulkan/Gpu.h"
#include "Vulkan/VulkanContext.h"
#include "World/WorldImpl.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // только для UTF-8 в консоли
#endif

namespace ce {

namespace core {

namespace {
std::atomic<EngineCore*> g_core{nullptr};
std::mutex g_logMutex;
} // namespace

EngineCore* CorePtr() { return g_core.load(std::memory_order_acquire); }
void SetCore(EngineCore* c) { g_core.store(c, std::memory_order_release); }

void Log(const std::string& message) {
    std::lock_guard lk(g_logMutex);
    std::cout << "[CE] " << message << std::endl;
}

} // namespace core

int Engine::Run(Game& game, const EngineConfig& requested) {
    using namespace core;
    if (CorePtr()) {
        Log("Engine::Run: движок уже запущен");
        return 1;
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        EngineCore ec;

        // engine.json рядом с exe (путь в UTF-8 -> path: на Windows ANSI сломал бы кириллицу)
        UserConfig user;
        if (!requested.configFile.empty()) {
            std::filesystem::path path(reinterpret_cast<const char8_t*>(requested.configFile.c_str()));
            if (path.is_relative()) {
                const std::filesystem::path dir = ExecutableDir();
                if (!dir.empty())
                    path = dir / path;
            }
            user = LoadUserConfig(path, true);
        }
        CpuInfo cpu = DetectCpu();

        InputState input;
        Window window(requested.width, requested.height, requested.title, &input, user.windowPlatform);
        VulkanContext vk(window, user.gpuDevice);
        GpuInfo gpuInfo = vk.Info();
        ApplyManualHardware(user, cpu, gpuInfo);
        const RuntimeSettings settings = ResolveSettings(requested, cpu, gpuInfo, user);

        EngineConfig cfg = requested;
        cfg.workerThreads = settings.workerThreads;
        cfg.vsync = settings.vsync;
        cfg.rayTracing = settings.rayTracing;
        cfg.msaa = settings.msaa;
        cfg.shadowMapSize = settings.shadowMapSize;
        ec.config = cfg;

        Log("CPU: " + Describe(cpu));
        Log("GPU: " + Describe(gpuInfo));
        Log("Окно: " + window.PlatformName() + (user.loaded ? " | engine.json прочитан" : "") +
            (user.loaded && !user.autoDetect ? " (auto = false: железо из файла)" : ""));
        Log("Настройки: " + Describe(settings));

        SystemInfo& si = ec.system;
#ifdef _WIN32
        si.os = "Windows";
#elif defined(__linux__)
        si.os = "Linux";
#else
        si.os = "unknown";
#endif
        si.windowPlatform = window.PlatformName();
        si.cpuVendor = cpu.vendor;
        si.cpuName = cpu.brand;
        si.cpuCores = cpu.physical;
        si.cpuThreads = cpu.logical;
        si.cpuHybrid = cpu.hybrid;
        si.gpuVendor = gpuInfo.vendor;
        si.gpuName = gpuInfo.name;
        si.gpuType = ToString(gpuInfo.type);
        si.gpuDriver = gpuInfo.driver;
        si.gpuMemoryMB = gpuInfo.vram >> 20;
        si.profile = settings.profile;
        si.configLoaded = user.loaded;

        Gpu gpu(vk);
        GpuLanes lanes(vk);
        const uint32_t workers = Scheduler::ResolveWorkerCount(cfg.workerThreads);
        WorldImpl world(workers + 2);
        Physics grave;
        NavSystem nav(grave, world);
        CameraState camera;
        Renderer amper(vk, gpu, lanes, window, cfg);
        amper.SetShadows(settings.shadows);
        amper.SetShadowDistance(settings.shadowDistance);

        AudioSettings audioSettings;
        audioSettings.enabled = user.audioEnabled >= 0 ? user.audioEnabled != 0 : cfg.audio;
        if (user.audioMaster >= 0)
            audioSettings.master = user.audioMaster;
        for (size_t g = 0; g < audioSettings.groups.size(); ++g)
            if (user.audioGroups[g] >= 0)
                audioSettings.groups[g] = user.audioGroups[g];
        AudioSystem audio(audioSettings);
        Log("Звук: " + audio.Description());

        ec.input = &input;
        ec.window = &window;
        ec.vk = &vk;
        ec.gpu = &gpu;
        ec.lanes = &lanes;
        ec.world = &world;
        ec.grave = &grave;
        ec.camera = &camera;
        ec.amper = &amper;
        ec.audio = &audio;
        ec.nav = &nav;
        TimerSystem timers;
        ec.timers = &timers;
        SetCore(&ec);
        struct CoreReset {
            ~CoreReset() { SetCore(nullptr); }
        } coreReset;

        // Планировщик создаётся последним и уничтожается первым: потоки останавливаются
        // до того, как исчезнут мир, физика и рендер, с которыми работают задачи.
        Scheduler scheduler(workers, cfg.targetFps);
        ec.scheduler = &scheduler;
        Log(std::string("UniX GPU lanes: ") + lanes.Description());
        Log(std::string("Amper ray tracing: ") + (amper.RtSupported() ? (amper.RayTracing() ? "on" : "off (flag)") : "not supported"));

        // Внутренние задачи уровня Actor — пользователь такие создать не может
        scheduler.Create("Grave.Step", Cost::High, Priority::High, Significance::Actor, Stage::Simulate,
                    [&grave](Task& t) { grave.Step(t, t.DeltaTime()); }, false);
        scheduler.Create("Amper.Prepare", Cost::Medium, Priority::High, Significance::Actor, Stage::Prepare,
                    [&amper](Task& t) { amper.Prepare(t); }, false);

        // Модули игры: после планировщика, значит выгрузятся раньше него (их задачи и таймеры ещё живы)
        ModuleSystem modules(user.modules);
        ec.modules = &modules;

        game.OnStart();
        world.Flush();
        grave.RefreshQueries();

        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        auto prev = start, titleTimer = start;
        uint32_t frames = 0;

        while (!window.ShouldClose() && !ec.quit.load()) {
            window.PollEvents();
            input.NewFrame(window);

            int w = 0, h = 0;
            window.FramebufferSize(w, h);
            if (w == 0 || h == 0) {  // свёрнуто
                window.WaitEvents();
                prev = clock::now();
                continue;
            }

            const auto now = clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - prev).count(), 0.25f);
            prev = now;
            ec.time.store(std::chrono::duration<double>(now - start).count());
            ec.dt.store(dt);
            ec.frame.fetch_add(1);

            scheduler.BeginFrame(ec.time.load(), dt, ec.frame.load());
            scheduler.RunStage(Stage::Update);
            {
                CE_ZONE("World.Flush");
                world.Flush();
            }
            scheduler.RunStage(Stage::Simulate);
            {
                CE_ZONE("Grave.Events + World.Flush");
                grave.DispatchEvents();
                timers.Update(dt);
                world.Flush();
                world.EnforceBounds();  // KillZ / возврат вылетевших
                grave.RefreshQueries(); // дерево для Raycast на следующий кадр
                nav.Update(dt);         // агенты навигации: скорости уйдут в следующий шаг физики
                Vec3 camPos;
                float yaw, pitch;
                {
                    std::lock_guard lk(camera.mutex);
                    camPos = camera.pos;
                    yaw = camera.yaw;
                    pitch = camera.pitch;
                }
                const Vec3 fwd{std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
                const Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
                audio.Update(camPos, fwd, Cross(right, fwd), world);  // слушатель = камера
            }
            scheduler.RunStage(Stage::Prepare);

            const auto r0 = clock::now();
            amper.Render();
            scheduler.AddMainWork(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - r0).count());
            scheduler.EndFrame();
            CE_FRAME_MARK();

            ++frames;
            const float elapsed = std::chrono::duration<float>(now - titleTimer).count();
            if (elapsed >= 0.5f) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s | %.0f FPS | CPU %.2f ms | RT %s | bodies %u (awake %u) | draws %u",
                              cfg.title.c_str(), frames / elapsed, scheduler.FrameWorkMs(),
                              amper.RayTracing() ? "on" : "off", grave.BodyCount(), grave.AwakeCount(),
                              amper.DrawCalls());
                window.SetTitle(buf);
                frames = 0;
                titleTimer = now;
            }
        }

        vkDeviceWaitIdle(vk.Device());
        game.OnShutdown();
        modules.UnloadAll();
        return 0;
    } catch (const std::exception& e) {
        Log(std::string("Fatal: ") + e.what());
        ShowFatalError("CustomEngine", e.what());  // на Windows игрок без консоли увидит окно с причиной
        return 1;
    }
}

void Engine::Quit() {
    if (auto* c = core::CorePtr())
        c->quit.store(true);
}
double Engine::Time() { return core::Core().time.load(std::memory_order_relaxed); }
float Engine::DeltaTime() { return core::Core().dt.load(std::memory_order_relaxed); }
uint64_t Engine::Frame() { return core::Core().frame.load(std::memory_order_relaxed); }
const SystemInfo& Engine::System() { return core::Core().system; }

// ---------------- Input ----------------

namespace {
core::InputState& In() { return *core::Core().input; }
} // namespace

bool InputApi::Down(Key k) const { return In().Down(static_cast<int>(k)); }
bool InputApi::Pressed(Key k) const { return In().Pressed(static_cast<int>(k)); }
bool InputApi::Released(Key k) const { return In().Released(static_cast<int>(k)); }
bool InputApi::MouseDown(MouseButton b) const { return In().MouseDown(static_cast<int>(b)); }
bool InputApi::MousePressed(MouseButton b) const { return In().MousePressed(static_cast<int>(b)); }
Vec2 InputApi::MousePosition() const { return In().MousePosition(); }
Vec2 InputApi::MouseDelta() const { return In().MouseDelta(); }
float InputApi::Scroll() const { return In().Scroll(); }
void InputApi::LockCursor(bool locked) const { In().RequestCursorLock(locked); }

} // namespace ce
