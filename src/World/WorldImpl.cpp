#include "World/WorldImpl.h"
#include "Core.h"
#include "Amper/Renderer.h"
#include "Grave/Physics.h"
#include "UniX/Scheduler.h"

#include <algorithm>
#include <stdexcept>

namespace ce::core {

WorldImpl::WorldImpl(uint32_t threadSlots) {
    for (uint32_t i = 0; i < threadSlots; ++i)
        m_buffers.push_back(std::make_unique<CommandBuffer>());
}

WorldImpl::~WorldImpl() {
    for (auto& p : m_pages)
        delete[] p.load();
}

bool WorldImpl::Deferred() const {
    EngineCore* c = CorePtr();
    return Scheduler::ThreadSlot() != 0 || (c && c->scheduler && c->scheduler->InStage());
}

const EntityData* WorldImpl::TryGet(uint32_t index, uint32_t gen) const {
    if (index >= Capacity())
        return nullptr;
    const EntityData* page = m_pages[index >> kPageBits].load(std::memory_order_acquire);
    if (!page)
        return nullptr;
    const EntityData& d = page[index & (kPageSize - 1)];
    return (d.alive && d.gen == gen) ? &d : nullptr;
}

Entity WorldImpl::Spawn() {
    uint32_t index, gen;
    const bool deferred = Deferred();
    {
        std::lock_guard lk(m_allocMutex);
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = m_next++;
            uint32_t page = index >> kPageBits;
            if (page >= kMaxPages)
                throw std::runtime_error("World: entity limit reached");
            if (!m_pages[page].load())
                m_pages[page].store(new EntityData[kPageSize], std::memory_order_release);
        }
        EntityData& d = At(index);
        gen = d.gen;
        if (!deferred) {
            d = EntityData{};
            d.gen = gen;
            d.alive = true;
            m_alive.fetch_add(1, std::memory_order_relaxed);
        }
        if (index + 1 > m_published.load())
            m_published.store(index + 1, std::memory_order_release);
    }
    if (deferred) {
        Command c{};
        c.type = CmdType::Create;
        c.index = index;
        c.gen = gen;
        Submit(c);
    }
    return Access::MakeEntity(index, gen);
}

void WorldImpl::Submit(const Command& cmd) {
    if (!Deferred()) {
        Apply(cmd);
        return;
    }
    int slot = Scheduler::ThreadSlot();
    CommandBuffer& buf = *m_buffers[static_cast<size_t>(slot) % m_buffers.size()];
    std::lock_guard lk(buf.mutex);
    buf.cmds.push_back(cmd);
}

// Сначала все Create, потом остальное в порядке буферов. Иначе сущность, созданная в задаче на потоке 3
// и удалённая (или сдвинутая) задачей на потоке 1 в той же стадии, получала команды раньше, чем
// появлялась: Destroy терялся, и «удалённая» сущность оставалась жить.
void WorldImpl::Flush() {
    m_scratch.clear();
    for (auto& buf : m_buffers) {
        std::lock_guard lk(buf->mutex);
        m_scratch.insert(m_scratch.end(), buf->cmds.begin(), buf->cmds.end());
        buf->cmds.clear();
    }
    for (const Command& c : m_scratch)
        if (c.type == CmdType::Create)
            Apply(c);
    for (const Command& c : m_scratch)
        if (c.type != CmdType::Create)
            Apply(c);
    m_scratch.clear();
}

WorldBounds WorldImpl::Bounds() {
    std::lock_guard lk(m_boundsMutex);
    return m_bounds;
}

// Сущности за областью мира: удалить или вернуть в точку возврата.
// Проверяется центр сущности (позиция), а не её AABB: объект исчезает, когда «вылетел», а не когда коснулся края.
void WorldImpl::EnforceBounds() {
    const WorldBounds b = Bounds();
    if (!b.enabled)
        return;
    Physics& ph = *Core().grave;
    const uint32_t cap = Capacity();
    for (uint32_t i = 0; i < cap; ++i) {
        EntityData* page = m_pages[i >> kPageBits].load(std::memory_order_acquire);
        if (!page) {
            i |= kPageSize - 1;  // страница не выделена — к следующей
            continue;
        }
        EntityData& d = page[i & (kPageSize - 1)];
        if (!d.alive)
            continue;
        OutOfBounds action = d.oob != OutOfBounds::Default ? d.oob : (d.moves ? b.action : OutOfBounds::None);
        if (action == OutOfBounds::None || action == OutOfBounds::Default)
            continue;
        const Vec3 p = d.pos;
        if (p.x >= b.min.x && p.y >= b.min.y && p.z >= b.min.z && p.x <= b.max.x && p.y <= b.max.y && p.z <= b.max.z)
            continue;

        if (action == OutOfBounds::Respawn && d.hasHome) {
            d.pos = d.home;
            d.rot = d.homeRot;
            d.velocity = Vec3{};
            if (d.body != UINT32_MAX) {
                ph.SetPosition(d.body, d.home);
                ph.SetRotation(d.body, d.homeRot);
                ph.SetVelocity(d.body, Vec3{});
                ph.SetAngularVelocity(d.body, Vec3{});
            }
            continue;
        }
        // Destroy (или Respawn, когда возвращать некуда)
        Command c{};
        c.type = CmdType::Destroy;
        c.index = i;
        c.gen = d.gen;
        Apply(c);
    }
}

void WorldImpl::Apply(const Command& c) {
    if (c.index >= Capacity())
        return;
    EntityData& d = At(c.index);
    if (d.gen != c.gen)
        return;

    if (c.type == CmdType::Create) {
        if (!d.alive) {
            d = EntityData{};
            d.gen = c.gen;
            d.alive = true;
            m_alive.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (!d.alive)
        return;

    Physics& ph = *Core().grave;
    auto removeBody = [&] {
        if (d.body == UINT32_MAX)
            return;
        uint32_t moved = ph.RemoveBody(d.body);
        if (moved != UINT32_MAX)
            At(moved).body = d.body;  // в слот удалённого тела переехало последнее
        d.body = UINT32_MAX;
    };

    switch (c.type) {
    case CmdType::Create:
        break;
    case CmdType::Destroy:
        removeBody();
        d.alive = false;
        ++d.gen;
        m_alive.fetch_sub(1, std::memory_order_relaxed);
        {
            std::lock_guard lk(m_allocMutex);
            m_free.push_back(c.index);
        }
        break;
    case CmdType::SetPos:
        d.pos = c.v;
        if (d.body != UINT32_MAX) ph.SetPosition(d.body, c.v);
        break;
    case CmdType::SetRot:
        d.rot = Normalize(c.q);
        if (d.body != UINT32_MAX) ph.SetRotation(d.body, d.rot);
        break;
    case CmdType::SetScale: d.scale = c.v; break;
    case CmdType::SetMesh:
        d.mesh = c.u;
        d.model = UINT32_MAX;
        break;
    case CmdType::SetModel:
        if (d.model != c.u) {
            d.model = c.u;
            d.animClip = UINT32_MAX;
            ++d.animSerial;
        }
        d.mesh = UINT32_MAX;
        break;
    case CmdType::PlayAnim: {
        const ModelData* md = d.model == UINT32_MAX ? nullptr : Core().amper->Models().Get(d.model);
        uint32_t clip = c.u;
        if (md && !c.text.empty()) {
            const int32_t found = md->FindClip(c.text);
            clip = found >= 0 ? static_cast<uint32_t>(found) : UINT32_MAX;
        }
        if (!md || clip >= md->clips.size()) {
            Log("PlayAnimation: " + std::string(md ? "нет анимации " : "у сущности нет модели, анимация ") +
                (c.text.empty() ? std::to_string(c.u) : "\"" + c.text + "\""));
            break;
        }
        d.animClip = clip;
        d.animLoop = c.v.y != 0;
        d.animFade = std::max(c.v.x, 0.0f);
        ++d.animSerial;
        break;
    }
    case CmdType::StopAnim:
        d.animClip = UINT32_MAX;
        d.animFade = std::max(c.v.x, 0.0f);
        ++d.animSerial;
        break;
    case CmdType::SetAnimSpeed: d.animSpeed = c.v.x; break;
    case CmdType::SetMaterial: d.material = c.material; break;
    case CmdType::SetColor: d.material.color = c.v; break;
    case CmdType::SetVisible: d.visible = c.u != 0; break;
    case CmdType::AddBody:
        removeBody();
        d.body = ph.AddBody(c.body, d.pos, d.rot, c.index, d.gen);
        d.moves = c.body.type != BodyType::Static && c.body.shape.type != Shape::Type::Plane;
        if (!d.homeExplicit) {
            d.home = d.pos;
            d.homeRot = d.rot;
            d.hasHome = true;
        }
        break;
    case CmdType::RemoveBody:
        removeBody();
        d.moves = false;
        break;
    case CmdType::SetOutOfBounds:
        d.oob = static_cast<OutOfBounds>(c.u);
        if (d.oob == OutOfBounds::Respawn && !d.hasHome) {
            d.home = d.pos;
            d.homeRot = d.rot;
            d.hasHome = true;
        }
        break;
    case CmdType::SetHome:
        d.home = c.v;
        d.homeRot = Normalize(c.q);
        d.hasHome = true;
        d.homeExplicit = true;
        break;
    case CmdType::SetVel:
        d.velocity = c.v;
        if (d.body != UINT32_MAX) ph.SetVelocity(d.body, c.v);
        break;
    case CmdType::SetAngVel:
        if (d.body != UINT32_MAX) ph.SetAngularVelocity(d.body, c.v);
        break;
    case CmdType::AddForce:
        if (d.body != UINT32_MAX) ph.AddForce(d.body, c.v);
        break;
    case CmdType::AddImpulse:
        if (d.body != UINT32_MAX) ph.AddImpulse(d.body, c.v);
        break;
    }
}

} // namespace ce::core
