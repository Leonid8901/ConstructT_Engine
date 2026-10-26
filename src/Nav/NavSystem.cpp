#include "Nav/NavSystem.h"
#include "Core.h"
#include "Grave/Physics.h"
#include "World/WorldImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace ce::core {

NavSystem::NavSystem(Physics& grave, WorldImpl& world) : m_grave(grave), m_world(world) {}

bool NavSystem::Build(const NavConfig& config) {
    const auto t0 = std::chrono::steady_clock::now();
    Aabb limit{config.boundsMin, config.boundsMax};
    StaticGeometry g;
    if (!m_grave.CollectStaticGeometry(config.layerMask, config.useBounds ? &limit : nullptr, g)) {
        Log("Nav: нет статической геометрии (боксы или плоскость со Static-телом); для одной плоскости задайте boundsMin/boundsMax");
        return false;
    }
    std::string error;
    if (!m_mesh.Build(g.verts, g.tris, config, g.min, g.max, error)) {
        Log("Nav: " + error);
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Nav: сетка построена за %.0f мс, %u полигонов, клетка %.2f м", ms, m_mesh.PolygonCount(),
                  m_mesh.CellSize());
    Log(buf);
    return true;
}

void NavSystem::Clear() { m_mesh.Clear(); }

NavSystem::Slot* NavSystem::Find(uint32_t index, uint32_t gen) {
    if (index >= m_slots.size())
        return nullptr;
    Slot& s = m_slots[index];
    return (s.alive && s.gen == gen) ? &s : nullptr;
}

void NavSystem::Free(uint32_t index) {
    Slot& s = m_slots[index];
    s.alive = false;
    s.walker.reset();
    ++s.gen;
    m_free.push_back(index);
}

NavAgent NavSystem::Create(Entity entity, const NavAgentParams& params) {
    if (Access::Index(entity) == UINT32_MAX)
        return {};
    std::lock_guard lk(m_mutex);
    uint32_t index;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    Slot& s = m_slots[index];
    const uint32_t gen = s.gen;
    s = Slot{};
    s.gen = gen;
    s.alive = true;
    s.walker = std::make_unique<NavWalker>(m_mesh);
    s.params = params;
    s.entity = Access::Index(entity);
    s.entityGen = Access::Gen(entity);
    s.seen = entity.Valid();
    return Access::MakeNavAgent(index, gen);
}

bool NavSystem::Valid(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    Slot* s = Find(index, gen);
    if (!s)
        return false;
    return Access::MakeEntity(s->entity, s->entityGen).Valid() || !s->seen;  // ещё не появилась — ждём
}

void NavSystem::StopLocked(Slot& s) {
    s.moving = false;
    s.brake = true;
    if (s.walker)
        s.walker->Clear();
}

bool NavSystem::MoveTo(uint32_t index, uint32_t gen, Vec3 target) {
    std::lock_guard lk(m_mutex);
    Slot* s = Find(index, gen);
    if (!s)
        return false;
    const Entity e = Access::MakeEntity(s->entity, s->entityGen);
    if (!e.Valid()) {
        if (s->seen) {  // сущность удалена
            Free(index);
            return false;
        }
        // Создана в этой же стадии и ещё не появилась: запоминаем цель, путь построит Update
        s->target = target;
        s->hasTarget = true;
        s->moving = true;
        s->arrived = false;
        s->partial = false;
        s->brake = false;
        return true;
    }
    s->seen = true;
    if (!m_mesh.Ready())
        return false;
    // Цель почти не сдвинулась: путь уже верный, перестраивать незачем (можно звать каждый кадр за игроком)
    if (s->moving && s->hasTarget && s->walker->HasPath() && LengthSq(target - s->target) < 0.25f * 0.25f)
        return true;
    const float foot = s->params.footOffset >= 0 ? s->params.footOffset : m_mesh.Config().agentHeight * 0.5f;
    bool partial = false;
    if (!s->walker->SetTarget(e.Position() - Vec3(0, foot, 0), target, partial)) {
        StopLocked(*s);
        s->hasTarget = false;
        s->arrived = false;
        s->partial = false;
        return false;
    }
    s->target = target;
    s->hasTarget = true;
    s->moving = true;
    s->arrived = false;
    s->partial = partial;
    s->brake = false;
    s->stuckTime = 0;
    s->lastPos = e.Position();
    NavWalker::Step st;  // длина пути сразу, а не после первого кадра
    if (s->walker->Advance(e.Position() - Vec3(0, foot, 0), 0.0f, st))
        s->remaining = st.remaining;
    return true;
}

void NavSystem::Stop(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    if (Slot* s = Find(index, gen)) {
        StopLocked(*s);
        s->hasTarget = false;
        s->arrived = false;
    }
}

void NavSystem::Remove(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    Slot* s = Find(index, gen);
    if (!s)
        return;
    StopLocked(*s);  // тело не должно ехать по инерции: затормозим, пока сущность жива
    Entity e = Access::MakeEntity(s->entity, s->entityGen);
    if (e.Valid()) {
        const EntityData* d = m_world.TryGet(s->entity, s->entityGen);
        if (d && d->body != UINT32_MAX && d->moves) {
            const Vec3 v = d->velocity;
            e.SetVelocity(m_grave.TypeOf(d->body) == BodyType::Dynamic ? Vec3(0, v.y, 0) : Vec3{});
        }
    }
    Free(index);
}

void NavSystem::SetSpeed(uint32_t index, uint32_t gen, float speed) {
    std::lock_guard lk(m_mutex);
    if (Slot* s = Find(index, gen))
        s->params.speed = std::max(0.0f, speed);
}

bool NavSystem::Flag(uint32_t index, uint32_t gen, Query q) {
    std::lock_guard lk(m_mutex);
    const Slot* s = Find(index, gen);
    if (!s)
        return false;
    switch (q) {
    case Query::Moving: return s->moving;
    case Query::Arrived: return s->arrived;
    case Query::Partial: return s->partial;
    }
    return false;
}

Vec3 NavSystem::Target(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    const Slot* s = Find(index, gen);
    return s ? s->target : Vec3{};
}

float NavSystem::Remaining(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    const Slot* s = Find(index, gen);
    return s ? s->remaining : 0.0f;
}

uint32_t NavSystem::AgentCount() {
    std::lock_guard lk(m_mutex);
    uint32_t n = 0;
    for (const Slot& s : m_slots)
        n += s.alive ? 1u : 0u;
    return n;
}

// Применить желаемое движение к сущности. Как именно — зависит от тела:
//  Dynamic    — горизонтальная скорость, вертикаль остаётся гравитации и контактам;
//  Kinematic  — скорость, по вертикали подтягиваем к высоте сетки;
//  нет тела / Static — прямое перемещение.
void NavSystem::Drive(Slot& s, Entity e, const NavWalker::Step& st, Vec3 feet, float foot, float dt) {
    Vec3 to = st.steer - feet;
    to.y = 0;
    const float len = Length(to);
    const Vec3 dir = len > 1e-4f ? to / len : Vec3{};

    // Скорость, с которой ещё можно остановиться у цели с заданным торможением
    const float brakeSpeed = std::sqrt(std::max(0.0f, s.params.acceleration) * std::max(0.0f, st.remaining - s.params.stopDistance));
    const float speed = std::min(s.params.speed, std::max(0.4f, brakeSpeed));
    const Vec3 want = dir * speed;

    const EntityData* d = m_world.TryGet(Access::Index(e), Access::Gen(e));
    const bool bodyDriven = d && d->body != UINT32_MAX && d->moves;
    const BodyType type = bodyDriven ? m_grave.TypeOf(d->body) : BodyType::Static;
    const Vec3 curV = bodyDriven ? d->velocity : s.vel;
    const Vec3 curH{curV.x, 0, curV.z};

    Vec3 newH = want;
    if (s.params.acceleration > 0) {
        const Vec3 diff = want - curH;
        const float dl = Length(diff);
        const float maxStep = s.params.acceleration * dt;
        if (dl > maxStep)
            newH = curH + diff * (maxStep / dl);
    }

    const Vec3 pos = e.Position();
    if (bodyDriven && type == BodyType::Dynamic) {
        e.SetVelocity({newH.x, curV.y, newH.z});
    } else if (bodyDriven) {
        const float vy = Clamp((st.pos.y + foot - pos.y) * 10.0f, -10.0f, 10.0f);
        e.SetVelocity({newH.x, vy, newH.z});
    } else {
        s.vel = newH;
        Vec3 np = pos + newH * dt;
        np.y = Lerp(pos.y, st.pos.y + foot, 1.0f - std::exp(-15.0f * dt));
        e.SetPosition(np);
    }

    // Лицом по ходу движения (у Entity::Forward() взгляд в -Z, yaw = 0)
    if (s.params.turnSpeed > 0 && Length(newH) > 0.2f) {
        const Vec3 fwd = e.Rotation().Rotate({0, 0, -1});
        const float cur = std::atan2(-fwd.x, -fwd.z);
        float delta = std::atan2(-newH.x, -newH.z) - cur;
        while (delta > kPi) delta -= 2 * kPi;
        while (delta < -kPi) delta += 2 * kPi;
        const float step = Clamp(delta, -s.params.turnSpeed * dt, s.params.turnSpeed * dt);
        e.SetRotation(Quat::Euler(0, cur + step, 0));
    }
}

void NavSystem::Update(float dt) {
    std::lock_guard lk(m_mutex);
    if (m_slots.empty())
        return;
    const bool ready = m_mesh.Ready();
    const float agentHeight = ready ? m_mesh.Config().agentHeight : 1.8f;

    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        Slot& s = m_slots[i];
        if (!s.alive)
            continue;
        Entity e = Access::MakeEntity(s.entity, s.entityGen);
        if (!e.Valid()) {
            if (!s.seen && s.wait < 3) {  // создана в задаче: появится после синхронизации кадра
                ++s.wait;
                continue;
            }
            Free(i);  // сущность удалили (или так и не появилась): агент уходит вместе с ней
            continue;
        }
        if (!s.seen) {
            s.seen = true;
            s.lastPos = e.Position();
        }
        const EntityData* d = m_world.TryGet(s.entity, s.entityGen);
        const bool bodyDriven = d && d->body != UINT32_MAX && d->moves;

        if (s.brake) {  // остановились: убрать оставшийся ход, иначе тело проскользит по инерции
            s.brake = false;
            if (bodyDriven) {
                const Vec3 v = d->velocity;
                e.SetVelocity(m_grave.TypeOf(d->body) == BodyType::Dynamic ? Vec3(0, v.y, 0) : Vec3{});
            }
            s.vel = {};
        }
        if (!s.moving)
            continue;
        if (!ready) {
            StopLocked(s);
            continue;
        }

        const float foot = s.params.footOffset >= 0 ? s.params.footOffset : agentHeight * 0.5f;
        const Vec3 feet = e.Position() - Vec3(0, foot, 0);
        NavWalker::Step st;
        bool ok = s.walker->Advance(feet, dt, st);
        if (ok) {
            // Отбросило или телепортировали далеко от пути; либо застряли на месте (другое тело, неровность)
            Vec3 off = st.pos - feet;
            off.y = 0;
            Vec3 moved = e.Position() - s.lastPos;
            moved.y = 0;
            s.lastPos = e.Position();
            s.stuckTime = Length(moved) < s.params.speed * dt * 0.15f ? s.stuckTime + dt : 0.0f;
            if (LengthSq(off) > 2.0f * 2.0f || s.stuckTime > 1.0f) {
                ok = false;
                s.stuckTime = 0;
            }
        }
        if (!ok) {  // путь потерян, сетку пересобрали или агент сбился: строим заново к той же цели
            bool partial = false;
            if (!s.hasTarget || !s.walker->SetTarget(feet, s.target, partial) || !s.walker->Advance(feet, dt, st)) {
                StopLocked(s);
                s.arrived = false;
                continue;
            }
            s.partial = partial;
        }
        s.remaining = st.remaining;
        if (st.atEnd || st.remaining <= s.params.stopDistance) {
            s.moving = false;
            s.arrived = true;
            s.brake = true;
            s.walker->Clear();
            continue;
        }
        Drive(s, e, st, feet, foot, dt);
    }
}

} // namespace ce::core
