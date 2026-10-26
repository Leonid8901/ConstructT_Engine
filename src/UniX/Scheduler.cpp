#include "UniX/Scheduler.h"
#include "Core.h"
#include "Platform/Hardware.h"
#include "Profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace ce::core {

namespace {

thread_local int t_slot = 0;  // 0 = главный поток

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr int kBucketOrder[2][4] = {
    {0, 1, 2, 3},  // Core: Sys -> Engine -> Actor -> Task
    {3, 2, 1, 0},  // General: Task -> Actor -> Engine -> Sys
};

struct PForData {
    void (*fn)(void*, uint32_t, uint32_t);
    void* ctx;
};

} // namespace

// ---------------- Arena ----------------

void Arena::Reset() {
    if (!m_overflow.empty() || m_requested > m_capacity) {
        // в прошлом вызове не хватило — растим один непрерывный блок
        size_t want = std::max<size_t>(m_requested * 2, 4096);
        m_overflow.clear();
        m_data.reset(new std::byte[want]);
        m_capacity = want;
    }
    m_offset = 0;
    m_requested = 0;
}

void* Arena::Alloc(size_t bytes, size_t align) {
    if (align < alignof(std::max_align_t))
        align = alignof(std::max_align_t);
    m_requested += bytes + align;
    if (m_data) {
        uintptr_t base = reinterpret_cast<uintptr_t>(m_data.get());
        uintptr_t p = (base + m_offset + align - 1) & ~(uintptr_t(align) - 1);
        if (p + bytes <= base + m_capacity) {
            m_offset = p + bytes - base;
            return reinterpret_cast<void*>(p);
        }
    }
    auto& block = m_overflow.emplace_back(new std::byte[bytes + align]);
    uintptr_t p = (reinterpret_cast<uintptr_t>(block.get()) + align - 1) & ~(uintptr_t(align) - 1);
    return reinterpret_cast<void*>(p);
}

TaskRecord::~TaskRecord() {
    if (state) {
        stateDtor(state);
        ::operator delete(state, std::align_val_t(stateAlign));
    }
}

// ---------------- Кольца work stealing ----------------

void Scheduler::Ring::Grow() {
    const uint32_t oldCap = static_cast<uint32_t>(buf.size());
    const uint32_t cap = oldCap == 0 ? 64u : oldCap * 2;
    std::vector<Job> grown(cap);
    for (uint32_t i = 0; i < size; ++i)
        grown[i] = buf[(head + i) & (oldCap - 1)];
    buf.swap(grown);
    head = 0;
}

void Scheduler::Ring::PushBack(const Job& job) {
    std::lock_guard lk(mutex);
    if (size == buf.size())
        Grow();
    buf[(head + size) & (buf.size() - 1)] = job;
    ++size;
    approxSize.store(size, std::memory_order_release);
}

void Scheduler::Ring::PushFront(const Job& job) {
    std::lock_guard lk(mutex);
    if (size == buf.size())
        Grow();
    head = (head + static_cast<uint32_t>(buf.size()) - 1) & (static_cast<uint32_t>(buf.size()) - 1);
    buf[head] = job;
    ++size;
    approxSize.store(size, std::memory_order_release);
}

bool Scheduler::Ring::PopFront(Job& out) {
    if (approxSize.load(std::memory_order_acquire) == 0)
        return false;  // пустое кольцо не блокируем
    std::lock_guard lk(mutex);
    if (size == 0)
        return false;
    out = buf[head];
    head = (head + 1) & (static_cast<uint32_t>(buf.size()) - 1);
    --size;
    approxSize.store(size, std::memory_order_release);
    return true;
}

// ---------------- Scheduler ----------------

uint32_t Scheduler::ResolveWorkerCount(uint32_t requested) {
    if (requested > 0)
        return requested;
    uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    return hw > 2 ? hw - 2 : 1;  // главный поток + фоновый поток
}

int Scheduler::ThreadSlot() { return t_slot; }

Scheduler::Scheduler(uint32_t workerThreads, float targetFps)
    : m_workerCount(ResolveWorkerCount(workerThreads)) {
    SetTargetFps(targetFps);
    m_queues = std::make_unique<SlotQueues[]>(ThreadSlots());
    m_workerLane = std::make_unique<std::atomic<LaneType>[]>(m_workerCount);
    m_coreWorkers = std::max(1u, m_workerCount / 3);
    for (uint32_t i = 0; i < m_workerCount; ++i)
        m_workerLane[i].store(i < m_coreWorkers ? LaneType::Core : LaneType::General);

    CE_THREAD_NAME("Main");
    for (uint32_t i = 0; i < m_workerCount; ++i)
        m_threads.emplace_back([this, i] { WorkerLoop(i + 1); });
    m_threads.emplace_back([this] { BackgroundLoop(BackgroundSlot()); });

    Log("UniX: workers=" + std::to_string(m_workerCount) + " (core=" + std::to_string(m_coreWorkers) +
        ", general=" + std::to_string(m_workerCount - m_coreWorkers) + ") + background=1 + main");
}

Scheduler::~Scheduler() {
    m_stop.store(true, std::memory_order_release);
    {
        std::lock_guard lk(m_sleepMutex);
    }
    m_sleepCv.notify_all();
    {
        std::lock_guard lk(m_bgMutex);
    }
    m_bgCv.notify_all();
    for (auto& t : m_threads)
        t.join();
}

std::shared_ptr<TaskRecord> Scheduler::Create(std::string name, Cost cost, Priority priority, Significance sig,
                                              Stage stage, TaskFn fn, bool once, bool pinMain) {
    auto rec = std::make_shared<TaskRecord>();
    rec->name = std::move(name);
    rec->declared = cost;
    rec->effective = cost;
    rec->candidate = cost;
    rec->priority = priority;
    rec->effectivePriority = priority;
    rec->sig = sig;
    rec->stage = stage;
    rec->fn = std::move(fn);
    rec->once = once;
    rec->pinMain = pinMain;
    rec->async = once && cost == Cost::Extreme;

    std::lock_guard lk(m_createMutex);
    if (rec->async) {
        // Фоновая задача владеет своей записью сама: её могут убить/забыть, пока она в очереди
        Job job;
        job.run = RunAsyncJob;
        job.data = new AsyncCtx{this, rec};
        job.rec = rec.get();
        {
            std::lock_guard blk(m_bgMutex);
            std::lock_guard qlk(m_background.mutex);
            m_background.jobs.push_back(job);
        }
        m_bgCv.notify_one();
    } else {
        m_pending.push_back(rec);
    }
    return rec;
}

void Scheduler::BeginFrame(double time, float dt, uint64_t frame) {
    m_time.store(time, std::memory_order_relaxed);
    m_dt = dt;
    m_frame.store(frame, std::memory_order_relaxed);
    m_stageNs = 0;
    m_waitNs.store(0);
    std::lock_guard lk(m_createMutex);
    for (auto& rec : m_pending)
        m_tasks[static_cast<size_t>(rec->stage)].push_back(std::move(rec));
    m_pending.clear();
}

namespace {
// Склейка мелких задач: задача считается мелкой, если по факту дешевле kSmallTaskMs
// (или ещё не измерена, но заявлена как Low). Джоб набирается до kBatchMs оценки или kMaxBatch задач.
constexpr float kSmallTaskMs = 0.05f;
constexpr float kBatchMs = 0.1f;
constexpr size_t kMaxBatch = 64;

bool IsSmall(const TaskRecord& r) {
    if (r.pinMain || r.effective != Cost::Low)
        return false;
    return r.emaMs < 0 ? r.declared == Cost::Low : r.emaMs < kSmallTaskMs;
}
float EstimateMs(const TaskRecord& r) { return r.emaMs < 0 ? 0.01f : r.emaMs; }
} // namespace

void Scheduler::RunStage(Stage stage) {
    CE_ZONE("UniX.RunStage");
    auto& list = m_tasks[static_cast<size_t>(stage)];
    m_due.clear();
    for (auto& sp : list) {
        TaskRecord* r = sp.get();
        if (!r->alive.load(std::memory_order_relaxed) || r->paused.load(std::memory_order_relaxed))
            continue;
        ++r->frameCounter;
        if (r->throttle > 1 && (r->frameCounter % r->throttle) != 0) {
            r->pendingDt += m_dt;  // разгрузка: пропуск тика, время копится
            continue;
        }
        r->dt = r->pendingDt + m_dt;
        r->pendingDt = 0;
        r->lastRunFrame = Frame();
        m_due.push_back(r);
    }
    if (m_due.empty())
        return;

    // Значимость -> приоритет -> самые тяжёлые первыми (LPT: минимизирует время стадии)
    std::sort(m_due.begin(), m_due.end(), [](const TaskRecord* a, const TaskRecord* b) {
        if (a->sig != b->sig) return a->sig < b->sig;
        if (a->effectivePriority != b->effectivePriority) return a->effectivePriority > b->effectivePriority;
        return a->effective > b->effective;
    });

    int64_t t0 = NowNs();
    m_inStage.store(true, std::memory_order_release);

    // Джобы раздаются по кольцам main + воркеров по кругу: самые тяжёлые попадают к разным потокам,
    // свободные потоки докрадывают остальное.
    std::atomic<int32_t> counter{0};
    const uint32_t execSlots = m_workerCount + 1;
    uint32_t next = 1 % execSlots;
    uint32_t pushed = 0;
    auto submit = [&](const Job& job, int bucket) {
        counter.fetch_add(1, std::memory_order_relaxed);
        PushTo(next, bucket, job, false);
        next = (next + 1) % execSlots;
        ++pushed;
    };

    size_t i = 0;
    while (i < m_due.size()) {
        TaskRecord* r = m_due[i];
        if (r->pinMain) {
            ++i;
            continue;
        }
        Job job;
        job.data = this;
        job.counter = &counter;
        const int bucket = static_cast<int>(r->sig);
        if (IsSmall(*r)) {
            size_t j = i;
            float est = 0;
            while (j < m_due.size() && j - i < kMaxBatch) {
                const TaskRecord* q = m_due[j];
                if (q->sig != r->sig || !IsSmall(*q))
                    break;
                est += EstimateMs(*q);
                ++j;
                if (est >= kBatchMs)
                    break;
            }
            if (j - i > 1) {
                job.run = RunBatchJob;
                job.begin = static_cast<uint32_t>(i);
                job.end = static_cast<uint32_t>(j);
                submit(job, bucket);
                i = j;
                continue;
            }
        }
        job.run = RunTaskJob;
        job.rec = r;
        submit(job, bucket);
        ++i;
    }
    Notify(pushed);

    for (TaskRecord* r : m_due)
        if (r->pinMain)
            ExecuteTask(*r);

    WaitAndHelp(counter, 3);

    m_inStage.store(false, std::memory_order_release);
    m_stageNs += NowNs() - t0;
}

void Scheduler::ExecuteTask(TaskRecord& rec) {
    CE_ZONE_NAMED(rec.name.c_str(), rec.name.size());
    rec.arena.Reset();
    rec.waitNs = 0;
    rec.chunkNs.store(0, std::memory_order_relaxed);

    int64_t t0 = NowNs();
    Task task(&rec);
    try {
        rec.fn(task);
    } catch (const std::exception& e) {
        Log("UniX: task '" + rec.name + "' threw: " + e.what() + " — task stopped");
        rec.alive.store(false);
    } catch (...) {
        Log("UniX: task '" + rec.name + "' threw unknown exception — task stopped");
        rec.alive.store(false);
    }
    int64_t wall = NowNs() - t0;
    rec.lastCpuNs = std::max<int64_t>(0, wall - rec.waitNs) + rec.chunkNs.load(std::memory_order_relaxed);
    if (rec.once)
        rec.alive.store(false);
}

void Scheduler::RunTaskJob(Job& job) { static_cast<Scheduler*>(job.data)->ExecuteTask(*job.rec); }

// Склеенные мелкие задачи: m_due не меняется до конца стадии, поэтому индексы стабильны
void Scheduler::RunBatchJob(Job& job) {
    auto* self = static_cast<Scheduler*>(job.data);
    for (uint32_t k = job.begin; k < job.end; ++k)
        self->ExecuteTask(*self->m_due[k]);
}

void Scheduler::RunAsyncJob(Job& job) {
    std::unique_ptr<AsyncCtx> ctx(static_cast<AsyncCtx*>(job.data));
    if (ctx->rec->alive.load() && !ctx->self->ShuttingDown())
        ctx->self->ExecuteTask(*ctx->rec);
    ctx->rec->alive.store(false);
}

void Scheduler::RunChunkJob(Job& job) {
    CE_ZONE("UniX.ParallelFor.Chunk");
    auto* d = static_cast<PForData*>(job.data);
    int64_t t0 = NowNs();
    d->fn(d->ctx, job.begin, job.end);
    if (job.rec)
        job.rec->chunkNs.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

// Счётчик уровня увеличивается ДО вставки: он может быть больше фактического числа джобов,
// но никогда меньше — спящий воркер не пропустит работу.
void Scheduler::PushTo(uint32_t slot, int bucket, const Job& job, bool front) {
    m_bucketPending[bucket].fetch_add(1, std::memory_order_seq_cst);
    Ring& ring = m_queues[slot].rings[bucket];
    if (front)
        ring.PushFront(job);
    else
        ring.PushBack(job);
}

bool Scheduler::PopFrom(uint32_t slot, int bucket, Job& out) {
    if (!m_queues[slot].rings[bucket].PopFront(out))
        return false;
    m_bucketPending[bucket].fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool Scheduler::AnyPending() const {
    for (const auto& p : m_bucketPending)
        if (p.load(std::memory_order_seq_cst) > 0)
            return true;
    return false;
}

// Будим ровно столько воркеров, сколько появилось работы; если никто не спит — мьютекс не трогаем.
void Scheduler::Notify(uint32_t jobs) {
    if (jobs == 0 || m_sleepers.load(std::memory_order_seq_cst) == 0)
        return;
    {
        std::lock_guard lk(m_sleepMutex);
    }
    if (jobs >= m_workerCount)
        m_sleepCv.notify_all();
    else
        for (uint32_t i = 0; i < jobs; ++i)
            m_sleepCv.notify_one();
}

void Scheduler::RunJob(Job& job, int bucket) {
    int64_t t0 = NowNs();
    job.run(job);
    m_bucketNs[bucket].fetch_add(NowNs() - t0, std::memory_order_relaxed);
    if (job.counter)
        job.counter->fetch_sub(1, std::memory_order_acq_rel);
}

// Уровни обходятся в порядке типа потока. На каждом уровне: своё кольцо -> чужие кольца ->
// кольцо фонового потока (его куски не обгоняют кадровую работу).
bool Scheduler::TryRunOne(int slotIndex, LaneType lane, int maxBucket) {
    const int* order = kBucketOrder[lane == LaneType::Core ? 0 : 1];
    const uint32_t slot = static_cast<uint32_t>(slotIndex);
    const uint32_t slots = ThreadSlots();
    const uint32_t bg = BackgroundSlot();
    if (slot == bg) {
        // Фоновый поток (пониженный приоритет) помогает только своим кускам ParallelFor: возьми он
        // кадровую работу, барьер стадии ждал бы медленный поток (инверсия приоритетов)
        for (int b = 0; b <= maxBucket && b < 4; ++b) {
            Job job;
            if (m_bucketPending[b].load(std::memory_order_relaxed) > 0 && PopFrom(bg, b, job)) {
                RunJob(job, b);
                return true;
            }
        }
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        const int b = order[i];
        if (b > maxBucket || m_bucketPending[b].load(std::memory_order_relaxed) <= 0)
            continue;
        Job job;
        if (PopFrom(slot, b, job)) {
            RunJob(job, b);
            return true;
        }
        for (uint32_t k = 1; k < slots; ++k) {
            const uint32_t victim = (slot + k) % slots;
            if (victim == bg)
                continue;
            if (PopFrom(victim, b, job)) {
                RunJob(job, b);
                return true;
            }
        }
        if (slot != bg && PopFrom(bg, b, job)) {
            RunJob(job, b);
            return true;
        }
    }
    return false;
}

LaneType Scheduler::CurrentLane() const {
    if (t_slot == 0)
        return LaneType::Core;
    if (static_cast<uint32_t>(t_slot) <= m_workerCount)
        return m_workerLane[t_slot - 1].load(std::memory_order_relaxed);
    return LaneType::General;
}

// Пока ждём — помогаем. maxBucket не даёт взять работу менее значимого уровня: например,
// ожидающий шаг Grave (держит свой лок) не подхватит пользовательский кусок, который зовёт Raycast.
void Scheduler::WaitAndHelp(std::atomic<int32_t>& counter, int maxBucket) {
    const LaneType lane = CurrentLane();
    while (counter.load(std::memory_order_acquire) > 0)
        if (!TryRunOne(t_slot, lane, maxBucket))
            std::this_thread::yield();
}

void Scheduler::WorkerLoop(uint32_t slot) {
    t_slot = static_cast<int>(slot);
    CE_THREAD_NAME(("UniX worker " + std::to_string(slot)).c_str());
    while (!m_stop.load(std::memory_order_acquire)) {
        LaneType lane = m_workerLane[slot - 1].load(std::memory_order_relaxed);
        if (TryRunOne(t_slot, lane))
            continue;
        bool got = false;
        for (int i = 0; i < 64 && !got; ++i) {
            std::this_thread::yield();
            got = TryRunOne(t_slot, lane);
        }
        if (got)
            continue;
        std::unique_lock lk(m_sleepMutex);
        m_sleepers.fetch_add(1, std::memory_order_seq_cst);
        m_sleepCv.wait(lk, [&] { return m_stop.load() || AnyPending(); });
        m_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }
}

void Scheduler::BackgroundLoop(uint32_t slot) {
    t_slot = static_cast<int>(slot);
    CE_THREAD_NAME("UniX background");
    LowerCurrentThreadPriority();  // на гибридных CPU ОС уведёт его на E-ядра, кадру не мешает
    while (true) {
        Job job;
        {
            std::unique_lock lk(m_bgMutex);
            m_bgCv.wait(lk, [&] {
                std::lock_guard qlk(m_background.mutex);
                return m_stop.load() || !m_background.jobs.empty();
            });
            std::lock_guard qlk(m_background.mutex);
            if (m_stop.load()) {
                for (Job& j : m_background.jobs)
                    delete static_cast<AsyncCtx*>(j.data);
                m_background.jobs.clear();
                return;
            }
            job = m_background.jobs.front();
            m_background.jobs.pop_front();
        }
        job.run(job);
    }
}

void Scheduler::ParallelFor(TaskRecord* rec, uint32_t count, void (*fn)(void*, uint32_t, uint32_t), void* ctx) {
    if (count == 0)
        return;
    const uint32_t lanes = m_workerCount + 1;
    const Cost c = rec ? rec->effective : Cost::High;
    uint32_t chunks = 1;
    switch (c) {
    case Cost::Low: chunks = 1; break;
    case Cost::Medium: chunks = std::min(2u, lanes); break;
    case Cost::High: chunks = lanes; break;
    case Cost::Extreme: chunks = lanes * 4; break;  // мелкая нарезка — лучше балансировка
    }
    const uint32_t grain = c >= Cost::High ? 1u : 16u;
    chunks = std::min(chunks, std::max(1u, count / grain));
    if (chunks <= 1) {
        fn(ctx, 0, count);
        return;
    }

    PForData data{fn, ctx};
    std::atomic<int32_t> counter{static_cast<int32_t>(chunks - 1)};
    const int bucket = rec ? static_cast<int>(rec->sig) : 3;
    const bool async = rec && rec->async;
    const uint32_t self = static_cast<uint32_t>(t_slot);
    for (uint32_t i = chunks - 1; i >= 1; --i) {
        Job job;
        job.run = RunChunkJob;
        job.data = &data;
        job.begin = static_cast<uint32_t>(uint64_t(count) * i / chunks);
        job.end = static_cast<uint32_t>(uint64_t(count) * (i + 1) / chunks);
        job.counter = &counter;
        job.rec = rec;
        if (async)
            PushTo(BackgroundSlot(), bucket, job, false);  // фоновые куски не обгоняют кадровую работу
        else
            PushTo((self + i) % lanes, bucket, job, true);  // куски — вперёд очереди, по разным потокам
    }
    Notify(chunks - 1);

    fn(ctx, 0, static_cast<uint32_t>(uint64_t(count) / chunks));

    int64_t w0 = NowNs();
    WaitAndHelp(counter, bucket);
    if (rec)
        rec->waitNs += NowNs() - w0;
}

float Scheduler::Budget(Cost c) const {
    static constexpr float kShare[] = {0.02f, 0.08f, 0.25f, 0.6f};
    return m_targetMs.load() * kShare[static_cast<int>(c)];
}

// Коэффициент полезности: насколько фактическая нагрузка соответствует заявленным параметрам.
void Scheduler::UpdateStats(TaskRecord& rec) {
    float ms = static_cast<float>(rec.lastCpuNs) * 1e-6f;
    rec.emaMs = rec.emaMs < 0 ? ms : rec.emaMs * 0.9f + ms * 0.1f;

    float budget = Budget(rec.declared);
    rec.loadRatio = rec.emaMs / budget;
    if (rec.loadRatio >= 1.0f)
        rec.utility = 1.0f / rec.loadRatio;  // недозаявила — мешает остальным
    else
        rec.utility = rec.declared == Cost::Low ? 1.0f : std::sqrt(rec.loadRatio);  // перезаявила тяжесть
    rec.utility = std::max(rec.utility, 0.01f);

    float f = m_targetMs.load();
    Cost actual = rec.emaMs < 0.04f * f ? Cost::Low
                  : rec.emaMs < 0.15f * f ? Cost::Medium
                  : rec.emaMs < 0.4f * f  ? Cost::High
                                          : Cost::Extreme;
    if (actual != rec.effective) {
        if (actual == rec.candidate) {
            if (++rec.candidateFrames >= 15) {  // гистерезис, чтобы не дёргать
                rec.effective = actual;
                rec.candidateFrames = 0;
            }
        } else {
            rec.candidate = actual;
            rec.candidateFrames = 1;
        }
    } else {
        rec.candidateFrames = 0;
    }

    // Пользовательская задача, сильно превысившая заявку, теряет приоритет
    if (rec.sig == Significance::Task) {
        int p = static_cast<int>(rec.priority);
        if (rec.loadRatio > 4.0f) p -= 2;
        else if (rec.loadRatio > 2.0f) p -= 1;
        rec.effectivePriority = static_cast<Priority>(std::max(p, 0));
    }
}

// Разгрузка: при перегрузе низкоприоритетные пользовательские задачи выполняются реже.
void Scheduler::AdjustThrottling() {
    float f = m_targetMs.load();
    if (m_workEma > f) {
        m_underFrames = 0;
        if (++m_overFrames < 10)
            return;
        m_overFrames = 0;
        for (auto& r : m_tasks[static_cast<size_t>(Stage::Update)])
            if (r->sig == Significance::Task && r->effectivePriority == Priority::Low && !r->once)
                r->throttle = std::min(r->throttle * 2, 8u);
    } else if (m_workEma < f * 0.7f) {
        m_overFrames = 0;
        if (++m_underFrames < 30)
            return;
        m_underFrames = 0;
        for (auto& r : m_tasks[static_cast<size_t>(Stage::Update)])
            r->throttle = std::max(r->throttle / 2, 1u);
    }
}

// Пересчёт соотношения Core/General воркеров по фактической нагрузке значимостей.
void Scheduler::Rebalance() {
    if (++m_rebalanceCounter < 60 || m_workerCount < 2)
        return;
    m_rebalanceCounter = 0;

    int64_t core = m_bucketNs[0].exchange(0) + m_bucketNs[1].exchange(0) + m_bucketNs[2].exchange(0);
    int64_t user = m_bucketNs[3].exchange(0);
    if (core + user <= 0)
        return;

    double share = static_cast<double>(core) / static_cast<double>(core + user);
    uint32_t desired = static_cast<uint32_t>(std::lround(share * m_workerCount));
    desired = std::clamp(desired, 1u, m_workerCount - 1);
    if (desired > m_coreWorkers) ++m_coreWorkers;
    else if (desired < m_coreWorkers) --m_coreWorkers;
    for (uint32_t i = 0; i < m_workerCount; ++i)
        m_workerLane[i].store(i < m_coreWorkers ? LaneType::Core : LaneType::General, std::memory_order_relaxed);
}

void Scheduler::EndFrame() {
    int64_t work = std::max<int64_t>(0, m_stageNs - m_waitNs.load());
    float workMs = static_cast<float>(work) * 1e-6f;
    m_lastWorkMs.store(workMs);
    m_workEma = m_workEma <= 0 ? workMs : m_workEma * 0.9f + workMs * 0.1f;

    for (auto& list : m_tasks) {
        for (auto& r : list)
            if (r->lastRunFrame == Frame() && r->alive.load())
                UpdateStats(*r);
        std::erase_if(list, [](const std::shared_ptr<TaskRecord>& r) { return !r->alive.load(); });
    }

    AdjustThrottling();
    Rebalance();
}

UniXApi::LaneInfo Scheduler::Lanes() const {
    UniXApi::LaneInfo info{0, 0, 1};
    for (uint32_t i = 0; i < m_workerCount; ++i)
        (m_workerLane[i].load() == LaneType::Core ? info.core : info.general)++;
    return info;
}

} // namespace ce::core
