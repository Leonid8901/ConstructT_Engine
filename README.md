# CustomEngine

Экспериментальный 3D-движок на Vulkan 1.3 / C++20. Состоит из трёх подсистем:

| Подсистема | Роль | Документ |
|---|---|---|
| **UniX** | параллельный планировщик задач с адаптивной классификацией нагрузки | [docs/UniX.md](docs/UniX.md) |
| **Grave** | физика твёрдых тел (3D) | [docs/Grave.md](docs/Grave.md) |
| **Amper** | рендер и освещение: PBR-растеризация, тени через ray query или каскадные карты теней | [docs/Amper.md](docs/Amper.md) |

Игровой код подключает один заголовок `CustomEngine.h`. Vulkan, GLFW и glm в публичный API не попадают: у движка собственные `Vec2/3/4`, `Quat`, `Mat3`, `Mat4`.

Статус: рабочий прототип (alpha). Назначение — учебный референс архитектуры, а не готовый продакшн-движок. Ограничения перечислены в разделе [Ограничения](#ограничения).

## Возможности

- Любое пользовательское действие — задача UniX с заявленными `Cost`, `Priority`, `Significance`.
- Планировщик сравнивает заявку с измеренным CPU-временем и корректирует классификацию, приоритет и частоту выполнения.
- Очереди на поток с work stealing; мелкие задачи склеиваются в один джоб.
- Тела: сфера, бокс, капсула, плоскость. Острова, сон, триггеры, слои и маски, raycast (BVH), события контактов.
- Область мира: вылетевшие за неё сущности удаляются или возвращаются на точку возврата (флаг на сущность).
- Рендер: PBR (GGX), инстансинг, reverse-Z, ACES, до 256 точечных источников. Модели glTF 2.0 (меши, иерархия, PBR-материалы), текстуры (bindless, мипы, анизотропия), карты нормалей, alpha cutoff, двусторонние материалы. Скелетные анимации с плавными переходами (скиннинг на GPU, RT-тени от анимированных моделей). Отсечение по фрустуму камеры и по объёму каждого каскада теней. Сглаживание MSAA 1/2/4/8.
- Тени от солнца: ray query (флаг) либо 4 каскада по 2048² с PCF 3×3.
- Звук (miniaudio): WAV/MP3/FLAC, 3D-звук за сущностями (слушатель — камера), музыка с плавной сменой, группы громкости.
- Навигация (Recast/Detour): navmesh из статических тел, поиск пути, агенты, которые сами идут к цели ([docs/Nav.md](docs/Nav.md)).
- Модули: свои библиотеки поверх движка (жизненный цикл, зависимости, настройки в engine.json), примеры «Скример» и «Погоня» ([docs/Modules.md](docs/Modules.md)).
- Триггеры с событиями входа/выхода (работают и для спящих тел) и таймеры `Timer.After/Every` ([docs/Triggers.md](docs/Triggers.md)).
- Изменения мира из задач потокобезопасны без блокировок на стороне пользователя (отложенные команды).
- Windows и Linux (X11 и Wayland). Определение CPU и GPU, профили по классу/вендору GPU, `engine.json` для игрока, fallback-и форматов под разные драйверы ([docs/Platforms.md](docs/Platforms.md)).

## Быстрый старт

```cpp
#include <CustomEngine.h>

class MyGame : public Game {
public:
    void OnStart() override {
        Camera.SetPosition({0, 5, 14});
        Camera.LookAt({0, 1, 0});

        World.Spawn()
            .SetMesh(Mesh::Plane(80))
            .AddBody({BodyType::Static, Shape::Plane()});

        World.Spawn()
            .SetMesh(Mesh::Cube())
            .SetPosition({0, 4, 0})
            .SetColor({0.9f, 0.4f, 0.2f})
            .AddBody({BodyType::Dynamic, Shape::Box({0.5f, 0.5f, 0.5f}), 1.0f});

        UniX.CreateTask.NewTask("Quit", Cost::Low, Priority::High, [](Task&) {
            if (Input.Pressed(Key::Escape))
                Engine::Quit();
        });
    }
};

int main() {
    EngineConfig cfg;
    cfg.rayTracing = true;  // если GPU не поддерживает — остаётся растеризация
    return Engine::Run<MyGame>(cfg);
}
```

Полный пример — [`sandbox/main.cpp`](sandbox/main.cpp).

## Сборка

Требования:

- CMake ≥ 3.20, компилятор с C++20
- Windows: Vulkan SDK (заголовки, loader, `glslc`). Linux: пакеты из [docs/Platforms.md](docs/Platforms.md#5-linux) (на Ubuntu одной командой `apt install`)
- GPU и драйвер с Vulkan 1.3
- GLFW 3.4 и Vulkan Memory Allocator 3.3.0 подтягиваются через `FetchContent`

```
cmake -S . -B build
cmake --build build --config Release
```

Windows:
```
build\Release\Sandbox.exe
```
Linux:
```
./build/Sandbox
```

Шейдеры компилируются `glslc` при сборке. Движок ищет SPIR-V в `shaders/` рядом с исполняемым файлом, затем в текущей папке, затем в каталоге сборки (`SHADER_DIR`). `cmake --install build --prefix dist` собирает папку для раздачи (exe + `shaders/`). Validation layers включаются в Debug автоматически, если установлен SDK.

При первом запуске рядом с exe появляется `engine.json`: по умолчанию всё `"auto"`, игрок может выбрать GPU, MSAA, RT, тени, число потоков, X11/Wayland. Что выбрал движок — в логе, строки `[CE] CPU/GPU/Настройки`.

Ray tracing опционален: нужны расширения `VK_KHR_acceleration_structure` и `VK_KHR_ray_query`. При их отсутствии `Amper.RayTracingSupported()` возвращает `false`, `SetRayTracing(true)` ничего не включает.

### Управление в Sandbox

| Действие | Ввод |
|---|---|
| Обзор | ПКМ + мышь |
| Полёт | W A S D / Q E, Shift — быстрее |
| Выстрел шаром | ЛКМ или Space |
| Ray tracing вкл/выкл | R |
| Каскадные тени вкл/выкл (при выключенном RT) | T |
| Сглаживание: MSAA 1 → 2 → 4 → 8 (что умеет GPU) | M |
| Музыка вкл/выкл (если передан файл) | N |
| Громкость | + / − |
| Navmesh и «монстр», гонящийся за камерой (ещё раз — убрать) | G |
| Показать / скрыть navmesh | H |
| Выход | Esc |

### Профилирование и бенчмарки

| Опция CMake | Что даёт |
|---|---|
| `-DCE_PROFILE=ON` | зоны [Tracy](https://github.com/wolfpld/tracy) v0.11.1: задачи UniX по именам, куски `ParallelFor`, стадии, Grave, Amper. Данные собираются, только пока подключён Tracy Profiler той же версии. Без опции макросы пустые |
| `-DCE_BUILD_BENCH=ON` | `UniXBench` — планировщик без окна и GPU; `SceneBench` — сцена из N кубов (FPS, CPU-время кадра) |

```
UniXBench [workers] [tiny|pfor|mixed|all] [frames]
SceneBench [side] [seconds] [rt|nort] [shadows|noshadows]
StressTest [seconds] [workers]   # всё API вперемешку: для ASan/TSan и валидации синхронизации Vulkan
```

## Структура репозитория

```
include/                публичный API (единственное, что видит игра)
  CustomEngine.h        единый заголовок
  CE/Math.h             Vec2/3/4, Quat, Mat3, Mat4
  CE/UniX.h             Task, TaskHandle, UniX
  CE/World.h            Entity, Mesh, Material, World, Camera
  CE/Shapes.h           Shape, BodyDesc, BodyType
  CE/Grave.h            Grave
  CE/Amper.h            Amper, Light
  CE/Input.h            Input, Key, MouseButton
  CE/Audio.h            Sound, Voice, Audio
  CE/Nav.h              NavConfig, NavAgent, Nav
  CE/Timer.h            Timer, TimerHandle
  CE/Module.h           Module, ModuleSettings, Modules
  CE/Engine.h           EngineConfig, Game, Engine
src/
  Engine.cpp            главный цикл кадра
  Profile.h             макросы профилирования (Tracy)
  UniX/                 Scheduler (CPU), GpuLanes (очереди GPU)
  World/                хранилище сущностей, буферы отложенных команд
  Grave/                коллизии, решатель, острова
  Amper/                рендерер, реестр мешей, BLAS/TLAS, каскады теней
  Vulkan/               контекст, swapchain, обёртка VMA
  Audio/                звук поверх miniaudio
  Nav/                  navmesh (Recast), запросы и агенты (Detour)
  Timer/                таймеры
  Module/               реестр модулей
  Platform/             окно (GLFW), ввод, определение железа, профили и engine.json
shaders/                GLSL: mesh, mesh_rt, sky, shadow
sandbox/                демонстрационная игра
modules/                примеры модулей (Jumpscare.h, Chase.h)
bench/                  UniXBench, SceneBench
docs/                   документация по подсистемам
```

## Документация

- [docs/Architecture.md](docs/Architecture.md) — кадр, модель потоков, мир и отложенные команды, соглашения
- [docs/UniX.md](docs/UniX.md) — модель задачи, API, алгоритмы адаптации, константы
- [docs/Grave.md](docs/Grave.md) — формы, шаг симуляции, решатель, параметры
- [docs/Amper.md](docs/Amper.md) — конвейер, модель освещения, тени
- [docs/Nav.md](docs/Nav.md) — navmesh, поиск пути, агенты, пределы
- [docs/Triggers.md](docs/Triggers.md) — триггеры (вход/выход) и таймеры
- [docs/Modules.md](docs/Modules.md) — модули: свои библиотеки, примеры «Скример» и «Погоня»
- [docs/API.md](docs/API.md) — справочник по `World`, `Entity`, `Mesh`, `Camera`, `Input`, `Engine`, математике
- [docs/Platforms.md](docs/Platforms.md) — Windows/Linux, определение железа, профили, `engine.json`, сборка на Linux

## Проверка

Сборка и запуск выполнялись на Linux (GCC) с программной реализацией Vulkan и включёнными validation layers: ноль ошибок и предупреждений в сценариях «RT вкл», «RT выкл + каскады», изменение размера окна, ввод, стрельба, события триггера. Не проверено: Windows/MSVC, аппаратные GPU, производительность на реальной нагрузке. Сравнения планировщика с другими реализациями (Taskflow, TBB, enkiTS) не проводилось. Планировщик проверен ThreadSanitizer'ом на `UniXBench` (1, 3 и 6 воркеров): гонок не найдено.

## Ограничения

- **UniX.** Кольца work stealing защищены мьютексами (по одному на кольцо), не lock-free. Зависимости между задачами выражены стадиями с барьерами (Update → Simulate → Prepare), графа зависимостей нет. Распределение работы по GPU-очередям минимально: compute-очередь используется для сборки BLAS, transfer — для загрузки мешей.
- **Grave.** Нет CCD, шарниров, меш-коллайдеров. Высокие плотные стены (≈ 10×5 боксов) не всегда засыпают; для очень высоких стопок помогает `SetFixedRate(240)`.
- **Платформы.** Windows и Linux (x86-64). macOS и Android не поддерживаются; ARM64 Linux собирается, но не проверялся. Нужен Vulkan 1.3: старые GPU (примерно до GTX 900 / RX 400 / Intel Skylake на Windows) получат понятную ошибку со списком причин.
- **Nav.** Одна сетка под один размер агента, нет динамических препятствий и off-mesh связей, агенты не избегают друг друга, геометрия — только коллайдеры Grave (не меши моделей).
- **Amper.** Прозрачность только через alpha cutoff (без смешивания), текстуры несжатые RGBA8. Тени точечных источников — только с RT. Освещение без кластеризации: фрагмент перебирает все источники. Отсечение — на CPU по сферам, без отсечения по перекрытию.

## Лицензия

MIT-License

## ИИ

Создано при поддержке Claude AI
