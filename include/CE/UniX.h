#pragma once
// UniX — планировщик и помощник движка.
// Любое действие пользователя — задача (Task). У задачи есть:
//   Cost         — заявленная ресурсозатратность (Low/Medium/High/Extreme)
//   Priority     — приоритет (Low/Normal/High)
//   Significance — значимость (Sys/Engine/Actor/Task). Пользователю доступен только Task:
//                  уровни Sys/Engine/Actor создаются исключительно внутри движка.
// UniX сравнивает заявленные параметры с фактическим поведением задачи (коэффициент полезности)
// и по нему раскидывает работу по типам потоков и очередям.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <typeinfo>

namespace ce {

enum class Cost : uint8_t { Low, Medium, High, Extreme };
enum class Priority : uint8_t { Low, Normal, High };
enum class Significance : uint8_t { Sys, Engine, Actor, Task };

namespace core { struct TaskRecord; class Scheduler; struct Access; }

// Контекст выполнения задачи — то, что получает пользовательская лямбда.
class Task {
public:
    float DeltaTime() const;      // время с прошлого вызова ЭТОЙ задачи (учитывает пропуски при разгрузке)
    double Time() const;          // время с запуска движка, сек
    uint64_t Frame() const;
    const std::string& Name() const;

    Cost DeclaredCost() const;
    Cost EffectiveCost() const;   // как UniX классифицирует задачу по факту
    Priority GetPriority() const;
    Significance GetSignificance() const;
    float Utility() const;        // 1.0 — фактическая нагрузка совпадает с заявленной, ->0 — сильно расходится
    float LoadRatio() const;      // факт / заявка: >1 тяжелее заявленного, <1 легче
    bool IsAsync() const;         // выполняется в фоновом лейне (может длиться несколько кадров)
    bool Cancelled() const;       // задачу убили или движок завершается — длинным задачам стоит выйти
    void Stop();                  // завершить задачу после текущего вызова

    // Параллельный цикл по диапазону [0, count). fn(begin, end).
    // На сколько кусков резать, решает UniX по эффективной стоимости задачи.
    template <class Fn>
    void ParallelFor(uint32_t count, Fn&& fn) {
        ParallelForImpl(count, [](void* ctx, uint32_t b, uint32_t e) { (*static_cast<Fn*>(ctx))(b, e); },
                        const_cast<void*>(static_cast<const void*>(&fn)));
    }

    // Постоянное состояние задачи (живёт, пока жива задача). Один тип на задачу.
    template <class T>
    T& State() {
        return *static_cast<T*>(StateStorage(typeid(T).hash_code(), sizeof(T), alignof(T),
                                             [](void* p) { new (p) T(); },
                                             [](void* p) { static_cast<T*>(p)->~T(); }));
    }

    // Скретч-память текущего вызова (сбрасывается перед каждым вызовом). Только для тривиальных типов.
    void* Alloc(size_t bytes, size_t align = 16);
    template <class T>
    T* AllocArray(size_t n) { return static_cast<T*>(Alloc(sizeof(T) * n, alignof(T))); }

private:
    friend class core::Scheduler;
    explicit Task(core::TaskRecord* r) : m_rec(r) {}
    void ParallelForImpl(uint32_t count, void (*fn)(void*, uint32_t, uint32_t), void* ctx);
    void* StateStorage(size_t typeHash, size_t size, size_t align, void (*ctor)(void*), void (*dtor)(void*));
    core::TaskRecord* m_rec;
};

using TaskFn = std::function<void(Task&)>;

class TaskHandle {
public:
    struct Stats {
        float avgMs = 0;        // среднее CPU-время на вызов
        float utility = 1;
        float loadRatio = 1;
        Cost effective = Cost::Low;
        uint32_t throttle = 1;  // 1 — каждый кадр, N — раз в N кадров (разгрузка)
    };

    TaskHandle() = default;
    bool Alive() const;
    void Pause();
    void Resume();
    void Kill();
    Stats GetStats() const;

private:
    friend struct core::Access;
    std::shared_ptr<core::TaskRecord> m_rec;
};

struct CreateTaskApi {
    // Задача, выполняемая каждый кадр.
    TaskHandle NewTask(std::string name, Cost cost, Priority priority, TaskFn fn) const;
    // Одноразовая задача. Extreme уходит в фоновый лейн и может выполняться несколько кадров.
    TaskHandle Once(std::string name, Cost cost, Priority priority, TaskFn fn) const;
};

struct UniXApi {
    CreateTaskApi CreateTask;

    void SetTargetFps(float fps) const;  // «фактическая цель» для бюджета кадра
    float TargetFrameMs() const;
    float FrameWorkMs() const;           // CPU-работа последнего кадра без ожидания vsync/GPU
    uint32_t WorkerCount() const;

    struct LaneInfo { uint32_t core, general, background; };
    LaneInfo Lanes() const;              // текущее распределение потоков по типам
    const char* GpuLanes() const;        // как распределены GPU-очереди (graphics/compute/transfer)
};

inline constexpr UniXApi UniX{};

} // namespace ce
