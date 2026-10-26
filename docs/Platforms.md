# Платформы, железо и engine.json

Движок работает на **Windows** и **Linux** (x86-64; ARM64 Linux собирается, но не проверялся). Требование к GPU одно: драйвер с **Vulkan 1.3** (dynamic rendering + synchronization2). Остальное движок подстраивает под железо сам.

## 1. Что определяется при запуске

Всё попадает в лог (`[CE] CPU: …`, `[CE] GPU: …`, `[CE] Настройки: …`) и в `Engine::System()`.

**CPU** (`src/Platform/Hardware.cpp`)

| Что | Windows | Linux |
|---|---|---|
| Вендор, модель, семейство, AVX/AVX2/AVX-512 | CPUID (x86) | CPUID (x86); ARM — `CPU implementer` из `/proc/cpuinfo` |
| Ядра / потоки, SMT | `GetLogicalProcessorInformationEx` | `/sys/devices/system/cpu/cpu*/topology` |
| Гибрид (P/E, big.LITTLE) | `EfficiencyClass` ядер | `/sys/devices/cpu_core` + `cpu_atom` (Intel), `cpu_capacity` (ARM) |
| AMD Bulldozer…Excavator (семейство 15h) | модуль = 2 ядра с общим FPU | то же |

**GPU** (`src/Vulkan/VulkanContext.cpp`): вендор по `vendorID` (AMD, NVIDIA, Intel, Apple, ARM Mali, Qualcomm Adreno, ImgTec, Broadcom, Mesa llvmpipe, SwiftShader и др.), класс (дискретный / встроенный / виртуальный / программный), драйвер (`VkPhysicalDeviceDriverProperties`: различает RADV / AMDVLK / AMD, NVIDIA / NVK, ANV и т.д.), VRAM, окно BAR / Resizable BAR, ray query.

## 2. Выбор GPU

Из всех GPU с Vulkan 1.3: дискретный > встроенный > виртуальный > программный, при равенстве — больше VRAM. Ноутбук с iGPU + dGPU получит дискретную. Каждый GPU пишется в лог с номером и причиной, если не подходит. Если не подходит ни один — ошибка со списком причин (на Windows — ещё и окно с сообщением, игрок без консоли её увидит).

Выбрать вручную: `"gpu": { "device": "1" }` (номер из лога) или часть имени (`"device": "radeon"`).

## 3. Профили

Настройки собираются в три слоя, каждый следующий сильнее:

1. **`EngineConfig`** из кода игры — пожелания (`rayTracing = true` значит «включить, если железо тянет»).
2. **Профиль железа** — урезает то, что железо не потянет.
3. **`engine.json`** — явный выбор игрока.

| Класс GPU | RT | MSAA максимум | Карта теней | Дальность теней |
|---|---|---|---|---|
| дискретный, VRAM ≥ 3 ГБ | как просила игра | 8 | 2048 | 80 м |
| дискретный, VRAM < 3 ГБ | выкл | 4 | 1024 | 60 м |
| встроенный / виртуальный | выкл | 2 (мобильные тайловые: 4) | 1024 | 60 м |
| программный (llvmpipe, SwiftShader) | выкл | 1 | 1024 | 40 м |

Почему класс важнее вендора: RT на встроенной графике есть (RDNA2 APU, Intel Arc iGPU), но съедает кадр целиком; а дискретные NVIDIA/AMD/Intel одного класса ведут себя одинаково — разница уже учтена драйвером и Vulkan. Мобильным тайловым GPU (Mali, Adreno, PowerVR, Apple, VideoCore) MSAA почти бесплатен: resolve происходит в тайловой памяти.

**CPU**: воркеров UniX = логические потоки − 2 (главный и фоновый потоки уже есть), не меньше 1 и не больше 32. На гибридах и Bulldozer счёт тот же: разную скорость ядер выравнивает work stealing, а фоновый поток (одноразовые Extreme-задачи) идёт с пониженным приоритетом — ОС охотнее отдаёт ему E-ядра.

Помимо профиля, движок сам подстраивается под возможности GPU, независимо от настроек:

- **формат экрана**: `*_SRGB`, иначе 8/10-битный `UNORM` — тогда гамму кодирует шейдер (картинка совпадает с sRGB-вариантом);
- **compositeAlpha**: OPAQUE есть не везде (часть Wayland/мобильных) — берётся первое поддерживаемое;
- **формат глубины**: D32 → X8_D24 → D16 (спецификация гарантирует D16); для теней — с линейной фильтрацией, если она есть, иначе без неё;
- **MSAA**: только поддерживаемые устройством и форматами числа сэмплов (бывают пропуски, например 1× и 4×);
- **размер карты теней** не больше `maxImageDimension2D`;
- **очереди**: выделенные compute/transfer, если есть; иначе всё на графической;
- **память**: потоковые буферы в BAR / Resizable BAR, если есть, иначе в обычной host-visible (VMA).

## 4. engine.json

Лежит рядом с exe. Если файла нет, движок создаёт его при первом запуске (если папка доступна на запись). Путь меняется `EngineConfig::configFile`, `""` отключает файл. Ошибка в файле не ломает запуск: в лог пишется строка с ошибкой, работают настройки по умолчанию.

```json
{
  "auto": true,
  "cpu": { "vendor": "auto", "threads": "auto" },
  "gpu": { "vendor": "auto", "type": "auto", "device": "auto" },
  "settings": {
    "workerThreads": "auto",
    "vsync": "auto",
    "rayTracing": "auto",
    "msaa": "auto",
    "shadows": "auto",
    "shadowMapSize": "auto",
    "shadowDistance": "auto"
  },
  "window": { "platform": "auto" },
  "audio": { "enabled": "auto", "master": "auto", "effects": "auto", "music": "auto", "voice": "auto", "ui": "auto" }
}
```

| Поле | Значения | Что делает |
|---|---|---|
| `auto` | `true` / `false` | `false`: `cpu.vendor`, `cpu.threads`, `gpu.vendor`, `gpu.type` берутся из файла, а не из определения железа; от них выбирается профиль |
| `cpu.vendor` | `AMD`, `Intel`, … | при `auto: false` |
| `cpu.threads` | число | при `auto: false`: сколько потоков считать |
| `gpu.vendor` | `NVIDIA`, `AMD`, `Intel`, `Apple`, `ARM`, `Qualcomm`, … | при `auto: false` |
| `gpu.type` | `discrete`, `integrated`, `software` | при `auto: false` |
| `gpu.device` | номер или часть имени | выбор GPU; работает и при `auto: true` |
| `settings.workerThreads` | число | воркеры UniX |
| `settings.vsync`, `rayTracing`, `shadows` | `true` / `false` | `rayTracing: true` включает RT даже на встроенной графике (если GPU умеет) |
| `settings.msaa` | 1, 2, 4, 8 | округляется вниз до поддерживаемого |
| `settings.shadowMapSize` | 512…8192 | округляется вверх до степени двойки |
| `settings.shadowDistance` | метры | дальность каскадных теней |
| `window.platform` | `auto`, `x11`, `wayland`, `win32` | Linux: `auto` — Wayland в Wayland-сессии, иначе X11. Не получилось — откат на `auto` |
| `audio.enabled` | `true` / `false` | выключить звук целиком |
| `audio.master`, `effects`, `music`, `voice`, `ui` | 0…1 | громкости: общая и по группам |
| `modules.<Имя>` | объект | настройки модуля игры; `"enabled": false` — модуль не загружается, остальные поля читает сам модуль ([Modules.md](Modules.md)) |

Любое поле можно удалить или оставить `"auto"` — тогда решает движок.

## 5. Linux

Пакеты (Ubuntu/Debian; в других дистрибутивах — те же библиотеки):

```
sudo apt install build-essential cmake git libvulkan-dev glslc vulkan-validationlayers \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols
```

Драйвер: AMD и Intel — Mesa (`mesa-vulkan-drivers`, RADV / ANV), NVIDIA — проприетарный драйвер (или NVK в свежей Mesa). Проверка: `vulkaninfo --summary` (пакет `vulkan-tools`) должен показать GPU с API 1.3.

Fedora: `sudo dnf install gcc-c++ cmake vulkan-loader-devel glslc vulkan-validation-layers libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel wayland-protocols-devel`.
Arch: `sudo pacman -S base-devel cmake vulkan-icd-loader vulkan-headers shaderc vulkan-validation-layers libx11 libxrandr libxinerama libxcursor libxi wayland libxkbcommon wayland-protocols`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Sandbox
```

GLFW собирается с X11 и с Wayland, если пакеты Wayland есть; без них — только X11 (CMake напишет об этом). Выбор при запуске — по сессии или `window.platform`.

Раздача: `cmake --install build --prefix dist` кладёт `Sandbox` и `shaders/` в одну папку — её можно копировать на другую машину и запускать из любого каталога (шейдеры ищутся рядом с exe).

## 6. Что проверено

| Конфигурация | Как |
|---|---|
| Windows 11, AMD RX 7800 xt, AMD Rayzen 8 900x3D| у автора (Debug, RT on); детект CPU/GPU и engine.json на Windows ещё не запускались |
| Linux, X11 (Xvfb), Mesa llvmpipe | сборка Release/Debug, валидация без ошибок, все варианты engine.json |
| Linux, Wayland (Weston), Mesa llvmpipe | запуск, валидация без ошибок, скриншот |
| swapchain UNORM (ручная гамма) | скриншот совпадает с sRGB (RMSE 0,1 %) |

Не проверено на реальном железе: NVIDIA, Intel, гибридные CPU, ARM. Если что-то не так — пришлите строки `[CE]` из лога.
