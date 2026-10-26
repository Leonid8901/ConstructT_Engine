#pragma once
// Навигация внутри движка: берёт статические тела Grave, строит сетку и ведёт агентов.
// Update — в главном потоке вне стадий (после физики): скорости, записанные агентами, применяются сразу
// и попадают в следующий шаг физики.
#include "Nav/NavMesh.h"

#include <memory>
#include <mutex>
#include <vector>

namespace ce::core {

class Physics;
class WorldImpl;

class NavSystem {
public:
    NavSystem(Physics& grave, WorldImpl& world);

    bool Build(const NavConfig& config);
    void Clear();
    NavMesh& Mesh() { return m_mesh; }

    NavAgent Create(Entity entity, const NavAgentParams& params);
    bool Valid(uint32_t index, uint32_t gen);
    bool MoveTo(uint32_t index, uint32_t gen, Vec3 target);
    void Stop(uint32_t index, uint32_t gen);
    void Remove(uint32_t index, uint32_t gen);
    void SetSpeed(uint32_t index, uint32_t gen, float speed);
    // что именно спрашиваем
    enum class Query { Moving, Arrived, Partial };
    bool Flag(uint32_t index, uint32_t gen, Query q);
    Vec3 Target(uint32_t index, uint32_t gen);
    float Remaining(uint32_t index, uint32_t gen);
    uint32_t AgentCount();

    void Update(float dt);

private:
    struct Slot {
        std::unique_ptr<NavWalker> walker;
        NavAgentParams params;
        uint32_t entity = 0, entityGen = 0;
        uint32_t gen = 1;
        bool alive = false;
        bool seen = false;   // сущность уже была живой (созданная в задаче появится только после синхронизации кадра)
        uint8_t wait = 0;    // сколько кадров ждём её появления
        bool moving = false, arrived = false, partial = false, hasTarget = false;
        bool brake = false;  // остановили: один раз обнулить горизонтальную скорость
        Vec3 target;
        Vec3 vel;            // горизонтальная скорость для тел без физики
        float remaining = 0;
        Vec3 lastPos;        // для обнаружения застревания
        float stuckTime = 0;
    };

    Slot* Find(uint32_t index, uint32_t gen);
    void StopLocked(Slot& s);
    void Drive(Slot& s, Entity e, const NavWalker::Step& st, Vec3 feet, float footOffset, float dt);
    void Free(uint32_t index);

    Physics& m_grave;
    WorldImpl& m_world;
    NavMesh m_mesh;  // объявлен раньше агентов: умирает позже них
    std::mutex m_mutex;
    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_free;
};

} // namespace ce::core
