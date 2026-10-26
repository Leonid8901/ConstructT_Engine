#pragma once
// CPU-сторона UniX.
//
// Кадр = последовательность стадий (Update -> Simulate -> Prepare). Внутри стадии все задачи
// выполняются параллельно, между стадиями — барьер и точка синхронизации мира.
//
// Типы потоков (лейны):
//   Main       — главный поток: Sys-задачи (окно, ввод), отправка кадра в GPU; помогает воркерам.
//   Core       — воркеры, которые сначала берут Sys/Engine/Actor работу, потом пользовательскую.
//   General    — воркеры, которые сначала берут пользовательские задачи (Task), потом остальное.
//   Background — фоновый поток для одноразовых Extreme-задач, растянутых на несколько кадров.
// Соотношение Core/General UniX пересчитывает по фактической нагрузке.
//
// Очереди: у каждого потока (main, воркеры, фон) своё кольцо работы на каждый уровень значимости.
// Поток сначала берёт работу из своего кольца, затем крадёт из чужих (work stealing). Порядок
// обхода уровней задаёт тип потока (Core/General), так что предпочтения лейнов сохраняются.
// Мелкие задачи одной значимости склеиваются в один джоб, чтобы не платить накладные расходы
// на каждую.
//
// Задача «перекидывается» только между вызовами — это безопасная точка: её состояние живёт
// в TaskRecord, а не в потоке, поэтому следующий тик может уйти в другой лейн.
#include "CE/UniX.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ce::core {

enum class Stage : uint8_t { Update, Simulate, Prepare, Count };
enum class LaneType : uint8_t { Core, General, Background };

// Линейный аллокатор памяти задачи на один вызов
class Arena {
public:
    void Reset();
    void* Alloc(size_t bytes, size_t align);

private:
    std::unique_ptr<std::byte[]> m_data;
    size_t m_capacity = 0;
    size_t m_offset = 0;
    size_t m_requested = 0;
    std::vector<std::unique_ptr<std::byte[]>> m_overflow;
};

struct TaskRecord {
    std::string name;
    Cost declared = Cost::Low;
    Priority priority = Priority::Normal;
    Significance sig = Significance::Task;
    Stage stage = Stage::Update;
    TaskFn fn;
    bool once = false;
    bool async = false;
    bool pinMain = false;

    std::atomic<bool> alive{true};
    std::atomic<bool> paused{false};

    // Разгрузка
    float dt = 0;
    float pendingDt = 0;
    uint32_t throttle = 1;
    uint32_t frameCounter = 0;
    uint64_t lastRunFrame = UINT64_MAX;

    // Измерения и коэффициент полезности
    std::atomic<int64_t> chunkNs{0};
    int64_t waitNs = 0;
    int64_t lastCpuNs = 0;
    float emaMs = -1;
    float loadRatio = 1;
    float utility = 1;
    Cost effective = Cost::Low;
    Cost candidate = Cost::Low;
    uint32_t candidateFrames = 0;
    Priority effectivePriority = Priority::Normal;

    Arena arena;

    void* state = nullptr;
    size_t stateHash = 0;
    size_t stateAlign = 0;
    void (*stateDtor)(void*) = nullptr;

    ~TaskRecord();
};

class Scheduler {
public:
    Scheduler(uint32_t workerThreads, float targetFps);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    static uint32_t ResolveWorkerCount(uint32_t requested);
    uint32_t ThreadSlots() const { return m_workerCount + 2; }  // main + воркеры + фон
    static int ThreadSlot();

    std::shared_ptr<TaskRecord> Create(std::string name, Cost cost, Priority priority, Significance sig, Stage stage,
                                       TaskFn fn, bool once, bool pinMain = false);

    void BeginFrame(double time, float dt, uint64_t frame);
    void RunStage(Stage stage);
    void EndFrame();
    void NoteWait(int64_t ns) { m_waitNs.fetch_add(ns, std::memory_order_relaxed); }
    void AddMainWork(int64_t ns) { m_stageNs += ns; }  // работа главного потока вне стадий (Render)

    void ParallelFor(TaskRecord* rec, uint32_t count, void (*fn)(void*, uint32_t, uint32_t), void* ctx);

    bool InStage() const { return m_inStage.load(std::memory_order_acquire); }
    bool ShuttingDown() const { return m_stop.load(std::memory_order_acquire); }
    // atomic: фоновые (async) задачи читают время/кадр, пока главный поток начинает новый кадр
    double Time() const { return m_time.load(std::memory_order_relaxed); }
    uint64_t Frame() const { return m_frame.load(std::memory_order_relaxed); }
    float TargetFrameMs() const { return m_targetMs.load(); }
    void SetTargetFps(float fps) { m_targetMs.store(1000.0f / (fps > 1 ? fps : 1)); }
    float FrameWorkMs() const { return m_lastWorkMs.load(); }
    uint32_t WorkerCount() const { return m_workerCount; }
    UniXApi::LaneInfo Lanes() const;

private:
    struct Job {
        void (*run)(Job&) = nullptr;
        void* data = nullptr;
        uint32_t begin = 0, end = 0;
        std::atomic<int32_t>* counter = nullptr;
        TaskRecord* rec = nullptr;
    };
    struct JobQueue {
        std::mutex mutex;
        std::deque<Job> jobs;
    };

    // Кольцевой буфер джобов одного потока для одного уровня значимости.
    // approxSize читается без блокировки, чтобы при краже не трогать мьютекс пустых колец.
    // alignas(64): соседние кольца не делят строку кэша (MSVC предупреждает о выравнивании — это и нужно).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Ring {
        std::mutex mutex;
        std::vector<Job> buf;  // ёмкость — степень двойки
        uint32_t head = 0;
        uint32_t size = 0;
        std::atomic<uint32_t> approxSize{0};

        void PushBack(const Job& job);
        void PushFront(const Job& job);
        bool PopFront(Job& out);

    private:
        void Grow();
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    struct SlotQueues {
        std::array<Ring, 4> rings;  // Sys, Engine, Actor, Task
    };

    struct AsyncCtx {
        Scheduler* self;
        std::shared_ptr<TaskRecord> rec;
    };
    static void RunTaskJob(Job& job);
    static void RunBatchJob(Job& job);
    static void RunAsyncJob(Job& job);
    static void RunChunkJob(Job& job);

    void ExecuteTask(TaskRecord& rec);
    void WorkerLoop(uint32_t slot);
    void BackgroundLoop(uint32_t slot);
    bool TryRunOne(int slot, LaneType lane, int maxBucket = 3);
    bool PopFrom(uint32_t slot, int bucket, Job& out);
    void RunJob(Job& job, int bucket);
    void PushTo(uint32_t slot, int bucket, const Job& job, bool front);
    void Notify(uint32_t jobs);
    bool AnyPending() const;
    void WaitAndHelp(std::atomic<int32_t>& counter, int maxBucket);
    LaneType CurrentLane() const;
    uint32_t BackgroundSlot() const { return m_workerCount + 1; }

    void UpdateStats(TaskRecord& rec);
    void Rebalance();
    void AdjustThrottling();
    float Budget(Cost c) const;

    uint32_t m_workerCount;
    std::vector<std::thread> m_threads;
    std::unique_ptr<std::atomic<LaneType>[]> m_workerLane;
    uint32_t m_coreWorkers = 1;

    std::unique_ptr<SlotQueues[]> m_queues;  // по одному набору колец на поток (ThreadSlots())
    std::array<std::atomic<int32_t>, 4> m_bucketPending{};  // джобов в кольцах по уровням (с запасом вверх)
    JobQueue m_background;  // фоновые Once-задачи целиком
    std::atomic<uint32_t> m_sleepers{0};
    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
    std::mutex m_bgMutex;
    std::condition_variable m_bgCv;
    std::atomic<bool> m_stop{false};

    std::mutex m_createMutex;
    std::vector<std::shared_ptr<TaskRecord>> m_pending;
    std::array<std::vector<std::shared_ptr<TaskRecord>>, static_cast<size_t>(Stage::Count)> m_tasks;
    std::vector<TaskRecord*> m_due;

    std::atomic<bool> m_inStage{false};
    std::atomic<double> m_time{0};
    float m_dt = 0;
    std::atomic<uint64_t> m_frame{0};

    std::atomic<float> m_targetMs{16.6667f};
    std::atomic<int64_t> m_waitNs{0};
    int64_t m_stageNs = 0;
    std::atomic<float> m_lastWorkMs{0};
    float m_workEma = 0;
    uint32_t m_overFrames = 0, m_underFrames = 0;

    std::array<std::atomic<int64_t>, 4> m_bucketNs{};
    uint32_t m_rebalanceCounter = 0;
};

} // namespace ce::core
