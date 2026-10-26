// Публичный фасад UniX
#include "Core.h"
#include "UniX/GpuLanes.h"
#include "UniX/Scheduler.h"

#include <stdexcept>

namespace ce {

using core::Core;

// ---------------- Task ----------------

float Task::DeltaTime() const { return m_rec->dt; }
double Task::Time() const { return Core().scheduler->Time(); }
uint64_t Task::Frame() const { return Core().scheduler->Frame(); }
const std::string& Task::Name() const { return m_rec->name; }
Cost Task::DeclaredCost() const { return m_rec->declared; }
Cost Task::EffectiveCost() const { return m_rec->effective; }
Priority Task::GetPriority() const { return m_rec->effectivePriority; }
Significance Task::GetSignificance() const { return m_rec->sig; }
float Task::Utility() const { return m_rec->utility; }
float Task::LoadRatio() const { return m_rec->loadRatio; }
bool Task::IsAsync() const { return m_rec->async; }
bool Task::Cancelled() const { return !m_rec->alive.load() || Core().scheduler->ShuttingDown(); }
void Task::Stop() { m_rec->alive.store(false); }

void Task::ParallelForImpl(uint32_t count, void (*fn)(void*, uint32_t, uint32_t), void* ctx) {
    Core().scheduler->ParallelFor(m_rec, count, fn, ctx);
}

void* Task::StateStorage(size_t typeHash, size_t size, size_t align, void (*ctor)(void*), void (*dtor)(void*)) {
    if (m_rec->state) {
        if (m_rec->stateHash != typeHash)
            throw std::logic_error("Task::State<T>: задача уже хранит состояние другого типа");
        return m_rec->state;
    }
    void* p = ::operator new(size, std::align_val_t(align));
    ctor(p);
    m_rec->state = p;
    m_rec->stateHash = typeHash;
    m_rec->stateAlign = align;
    m_rec->stateDtor = dtor;
    return p;
}

void* Task::Alloc(size_t bytes, size_t align) { return m_rec->arena.Alloc(bytes, align); }

// ---------------- TaskHandle ----------------

bool TaskHandle::Alive() const { return m_rec && m_rec->alive.load(); }
void TaskHandle::Pause() { if (m_rec) m_rec->paused.store(true); }
void TaskHandle::Resume() { if (m_rec) m_rec->paused.store(false); }
void TaskHandle::Kill() { if (m_rec) m_rec->alive.store(false); }

TaskHandle::Stats TaskHandle::GetStats() const {
    Stats s;
    if (!m_rec)
        return s;
    s.avgMs = std::max(m_rec->emaMs, 0.0f);
    s.utility = m_rec->utility;
    s.loadRatio = m_rec->loadRatio;
    s.effective = m_rec->effective;
    s.throttle = m_rec->throttle;
    return s;
}

// ---------------- CreateTask ----------------
// Пользовательский код может создать только задачи значимости Task: Sys/Engine/Actor
// создаются через core::Scheduler::Create, который недоступен из публичных заголовков.

TaskHandle CreateTaskApi::NewTask(std::string name, Cost cost, Priority priority, TaskFn fn) const {
    if (!fn)
        throw std::invalid_argument("UniX.CreateTask.NewTask: пустая функция задачи");
    return core::Access::MakeHandle(Core().scheduler->Create(std::move(name), cost, priority, Significance::Task,
                                                        core::Stage::Update, std::move(fn), false));
}

TaskHandle CreateTaskApi::Once(std::string name, Cost cost, Priority priority, TaskFn fn) const {
    if (!fn)
        throw std::invalid_argument("UniX.CreateTask.Once: пустая функция задачи");
    return core::Access::MakeHandle(Core().scheduler->Create(std::move(name), cost, priority, Significance::Task,
                                                        core::Stage::Update, std::move(fn), true));
}

// ---------------- UniX ----------------

void UniXApi::SetTargetFps(float fps) const { Core().scheduler->SetTargetFps(fps); }
float UniXApi::TargetFrameMs() const { return Core().scheduler->TargetFrameMs(); }
float UniXApi::FrameWorkMs() const { return Core().scheduler->FrameWorkMs(); }
uint32_t UniXApi::WorkerCount() const { return Core().scheduler->WorkerCount(); }
UniXApi::LaneInfo UniXApi::Lanes() const { return Core().scheduler->Lanes(); }
const char* UniXApi::GpuLanes() const { return Core().lanes->Description().c_str(); }

} // namespace ce
