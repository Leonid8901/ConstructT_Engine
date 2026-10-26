#include "Grave/Physics.h"
#include "Core.h"
#include "Grave/Collision.h"
#include "Profile.h"
#include "World/WorldImpl.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ce::core {

namespace {

constexpr float kSleepLinear = 0.02f;
constexpr float kSleepAngular = 0.04f;
constexpr float kSleepTime = 0.5f;
constexpr float kBaumgarte = 0.2f;
constexpr float kSlop = 0.01f;
constexpr float kMaxBiasVelocity = 4.0f;
constexpr float kBounceThreshold = 1.0f;
constexpr int kMaxSteps = 8;

uint64_t PairKey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

void TangentBasis(Vec3 n, Vec3& t1, Vec3& t2) {
    t1 = std::fabs(n.x) >= 0.57735f ? Normalize(Vec3{n.y, -n.x, 0}) : Normalize(Vec3{0, n.z, -n.y});
    t2 = Cross(n, t1);
}

void UpdateInertiaWorld(Body& b) {
    if (!b.Dynamic() || b.lockRot) {
        b.invInertiaWorld = Mat3::Diagonal(Vec3(0.0f));
        return;
    }
    Mat3 R = Mat3::FromQuat(b.rot);
    b.invInertiaWorld = R * Mat3::Diagonal(b.invInertiaLocal) * R.Transposed();
}

inline Vec3 VelocityAt(const Body& b, Vec3 r) { return b.v + Cross(b.w, r); }

inline void ApplyImpulse(Body& b, Vec3 j, Vec3 r) {
    if (!b.Dynamic())
        return;  // статику и кинематику не трогаем: их делят острова, писать в них нельзя
    b.v += j * b.invMass;
    b.w += b.invInertiaWorld * Cross(r, j);
}

inline Vec3 PseudoVelocityAt(const Body& b, Vec3 r) { return b.pv + Cross(b.pw, r); }

inline void ApplyPseudo(Body& b, Vec3 j, Vec3 r) {
    if (!b.Dynamic())
        return;
    b.pv += j * b.invMass;
    b.pw += b.invInertiaWorld * Cross(r, j);
}

inline void ApplyAngular(Body& b, Vec3 L) {
    if (b.Dynamic())
        b.w += b.invInertiaWorld * L;
}

inline float EffectiveMass(const Body& A, const Body& B, Vec3 rA, Vec3 rB, Vec3 dir) {
    float k = A.invMass + B.invMass + Dot(Cross(A.invInertiaWorld * Cross(rA, dir), rA) +
                                          Cross(B.invInertiaWorld * Cross(rB, dir), rB), dir);
    return k > 1e-9f ? 1.0f / k : 0.0f;
}

} // namespace

// ---------------- тела ----------------

void Physics::UpdateMass(Body& b, const BodyDesc& d) {
    if (b.type != BodyType::Dynamic) {
        b.invMass = 0;
        b.invInertiaLocal = Vec3(0.0f);
        UpdateInertiaWorld(b);
        return;
    }
    float m = d.mass > 0 ? d.mass : 1.0f;
    b.invMass = 1.0f / m;
    Vec3 I;
    Vec3 e = d.shape.size;
    switch (d.shape.type) {
    case Shape::Type::Sphere: I = Vec3(0.4f * m * e.x * e.x); break;
    case Shape::Type::Box: I = {m / 3 * (e.y * e.y + e.z * e.z), m / 3 * (e.x * e.x + e.z * e.z), m / 3 * (e.x * e.x + e.y * e.y)}; break;
    case Shape::Type::Capsule: {
        Vec3 h{e.x, e.y + e.x, e.x};  // аппроксимация боксом
        I = {m / 3 * (h.y * h.y + h.z * h.z), m / 3 * (h.x * h.x + h.z * h.z), m / 3 * (h.x * h.x + h.y * h.y)};
        break;
    }
    case Shape::Type::Plane: I = Vec3(1.0f); break;
    }
    b.invInertiaLocal = d.lockRotation ? Vec3(0.0f) : Vec3{1 / I.x, 1 / I.y, 1 / I.z};
    UpdateInertiaWorld(b);
}

uint32_t Physics::AddBody(const BodyDesc& d, Vec3 pos, Quat rot, uint32_t entity, uint32_t gen) {
    std::unique_lock lk(m_lock);
    Body b;
    b.pos = pos;
    b.rot = Normalize(rot);
    b.shape = d.shape;
    b.type = d.shape.type == Shape::Type::Plane ? BodyType::Static : d.type;
    b.friction = d.friction;
    b.restitution = d.restitution;
    b.linDamp = d.linearDamping;
    b.angDamp = d.angularDamping;
    b.trigger = d.trigger;
    b.lockRot = d.lockRotation;
    b.layer = d.layer;
    b.mask = d.mask;
    b.entity = entity;
    b.entityGen = gen;
    UpdateMass(b, d);
    b.aabb = ComputeAabb(b);
    m_bodies.push_back(b);
    m_topologyDirty = true;
    m_rayValid = false;
    return static_cast<uint32_t>(m_bodies.size() - 1);
}

uint32_t Physics::RemoveBody(uint32_t index) {
    std::unique_lock lk(m_lock);
    if (index >= m_bodies.size())
        return UINT32_MAX;
    uint32_t last = static_cast<uint32_t>(m_bodies.size() - 1);
    uint32_t moved = UINT32_MAX;
    if (index != last) {
        m_bodies[index] = m_bodies[last];
        moved = m_bodies[index].entity;
    }
    m_bodies.pop_back();
    m_topologyDirty = true;
    m_rayValid = false;
    m_cache.clear();  // индексы тел сдвинулись — старый кэш недействителен
    // пары триггеров хранятся по сущностям и от перестановки индексов не зависят: не трогаем
    return moved;
}

void Physics::Wake(Body& b) {
    b.sleeping = false;
    b.sleepTimer = 0;
}

#define CE_BODY_OP(Method, Type, Body_)              \
    void Physics::Method(uint32_t i, Type val) {      \
        std::unique_lock lk(m_lock);                  \
        if (i >= m_bodies.size()) return;             \
        Body& b = m_bodies[i];                        \
        Body_;                                        \
        Wake(b);                                      \
        m_rayValid = false;                           \
    }

CE_BODY_OP(SetPosition, Vec3, { b.pos = val; b.aabb = ComputeAabb(b); })
CE_BODY_OP(SetRotation, Quat, { b.rot = Normalize(val); UpdateInertiaWorld(b); b.aabb = ComputeAabb(b); })
CE_BODY_OP(SetVelocity, Vec3, { b.v = val; })
CE_BODY_OP(SetAngularVelocity, Vec3, { if (!b.lockRot) b.w = val; })
CE_BODY_OP(AddForce, Vec3, { b.force += val; })
CE_BODY_OP(AddImpulse, Vec3, { b.v += val * b.invMass; })
#undef CE_BODY_OP

void Physics::SetGravity(Vec3 g) {
    std::unique_lock lk(m_lock);
    m_gravity = g;
    for (Body& b : m_bodies)
        Wake(b);
}
Vec3 Physics::Gravity() const {
    std::shared_lock lk(m_lock);
    return m_gravity;
}
void Physics::SetIterations(uint32_t n) {
    std::unique_lock lk(m_lock);
    m_iterations = std::clamp(n, 1u, 64u);
}
void Physics::SetFixedRate(float hz) {
    std::unique_lock lk(m_lock);
    m_fixedDt = 1.0f / std::clamp(hz, 15.0f, 1000.0f);
}
uint32_t Physics::BodyCount() const {
    std::shared_lock lk(m_lock);
    return static_cast<uint32_t>(m_bodies.size());
}

void Physics::AddCallback(std::function<void(const ContactEvent&)> cb) {
    std::lock_guard lk(m_cbMutex);
    m_callbacks.push_back(std::move(cb));
}

// ---------------- шаг ----------------

void Physics::Step(Task& task, float frameDt) {
    std::unique_lock lk(m_lock);
    m_events.clear();
    m_trigEvents.clear();
    m_accum += std::min(frameDt, 0.1f);
    int steps = 0;
    while (m_accum >= m_fixedDt && steps < kMaxSteps) {
        FixedStep(task, m_fixedDt);
        m_accum -= m_fixedDt;
        ++steps;
    }
    if (steps == kMaxSteps)
        m_accum = 0;  // защита от «спирали смерти»
    if (steps == 0)
        return;
    m_rayValid = false;

    // Запись результата в мир (в стадии Simulate никто другой мир не пишет)
    WorldImpl& world = *Core().world;
    std::atomic<uint32_t> awake{0};
    task.ParallelFor(static_cast<uint32_t>(m_bodies.size()), [&](uint32_t begin, uint32_t end) {
        uint32_t local = 0;
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            b.force = Vec3{};
            b.torque = Vec3{};
            if (b.type == BodyType::Static || b.sleeping)
                continue;
            ++local;
            EntityData& d = world.At(b.entity);
            if (d.alive && d.gen == b.entityGen) {
                d.pos = b.pos;
                d.rot = b.rot;
                d.velocity = b.v;
            }
        }
        awake.fetch_add(local, std::memory_order_relaxed);
    });
    m_awake = awake.load();
}

bool Physics::Active(const Body& b) const {
    if (b.type == BodyType::Dynamic)
        return !b.sleeping;
    if (b.type == BodyType::Kinematic)
        return LengthSq(b.v) > 0 || LengthSq(b.w) > 0;
    return false;
}

void Physics::Broadphase() {
    CE_ZONE("Grave.Broadphase");
    if (m_topologyDirty) {
        m_sapOrder.clear();
        m_planes.clear();
        for (uint32_t i = 0; i < m_bodies.size(); ++i)
            (m_bodies[i].Plane() ? m_planes : m_sapOrder).push_back(i);
        std::sort(m_sapOrder.begin(), m_sapOrder.end(),
                  [&](uint32_t a, uint32_t b) { return m_bodies[a].aabb.min.x < m_bodies[b].aabb.min.x; });
        m_topologyDirty = false;
    } else {
        // Insertion sort: порядок между шагами почти не меняется -> почти O(n)
        for (size_t i = 1; i < m_sapOrder.size(); ++i) {
            uint32_t key = m_sapOrder[i];
            float kx = m_bodies[key].aabb.min.x;
            size_t j = i;
            while (j > 0 && m_bodies[m_sapOrder[j - 1]].aabb.min.x > kx) {
                m_sapOrder[j] = m_sapOrder[j - 1];
                --j;
            }
            m_sapOrder[j] = key;
        }
    }

    auto filter = [&](const Body& A, const Body& B) {
        if (!((A.layer & B.mask) && (B.layer & A.mask)))
            return false;
        if (A.trigger || B.trigger) {
            // Зона проверяется и когда тело спит или стоит: иначе заснувшее тело «выходило» бы из неё.
            // Два статических тела друг с другом не взаимодействуют.
            return A.type != BodyType::Static || B.type != BodyType::Static;
        }
        if (!(Active(A) || Active(B)))
            return false;
        return A.Dynamic() || B.Dynamic();
    };

    m_pairs.clear();
    const size_t n = m_sapOrder.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t ia = m_sapOrder[i];
        const Body& A = m_bodies[ia];
        for (size_t j = i + 1; j < n; ++j) {
            const uint32_t ib = m_sapOrder[j];
            const Body& B = m_bodies[ib];
            if (B.aabb.min.x > A.aabb.max.x)
                break;
            if (A.aabb.max.y < B.aabb.min.y || B.aabb.max.y < A.aabb.min.y || A.aabb.max.z < B.aabb.min.z ||
                B.aabb.max.z < A.aabb.min.z)
                continue;
            if (filter(A, B))
                m_pairs.emplace_back(std::min(ia, ib), std::max(ia, ib));
        }
    }

    for (uint32_t ip : m_planes) {
        const Body& P = m_bodies[ip];
        Vec3 pn = P.rot.Rotate({0, 1, 0});
        float pd = Dot(pn, P.pos);
        for (uint32_t ib : m_sapOrder) {
            const Body& B = m_bodies[ib];
            if (!filter(P, B))
                continue;
            Vec3 c = (B.aabb.min + B.aabb.max) * 0.5f, h = (B.aabb.max - B.aabb.min) * 0.5f;
            if (Dot(pn, c) - pd - Dot(Abs(pn), h) <= 0)
                m_pairs.emplace_back(std::min(ip, ib), std::max(ip, ib));
        }
    }
}

uint32_t Physics::Find(uint32_t i) {
    while (m_parent[i] != i) {
        m_parent[i] = m_parent[m_parent[i]];
        i = m_parent[i];
    }
    return i;
}

void Physics::BuildIslands() {
    CE_ZONE("Grave.Islands");
    const uint32_t n = static_cast<uint32_t>(m_bodies.size());
    m_parent.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_parent[i] = i;
    for (const Manifold& m : m_contacts)
        if (m_bodies[m.a].Dynamic() && m_bodies[m.b].Dynamic()) {
            uint32_t ra = Find(m.a), rb = Find(m.b);
            if (ra != rb) m_parent[ra] = rb;
        }

    // пробуждение: остров, где есть хоть одно бодрствующее тело, просыпается целиком
    m_islandAwake.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic() && !m_bodies[i].sleeping)
            m_islandAwake[Find(i)] = 1;
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic() && m_bodies[i].sleeping && m_islandAwake[Find(i)])
            Wake(m_bodies[i]);

    // группировка контактов по островам (counting sort)
    m_islandOf.assign(n, UINT32_MAX);
    std::vector<uint32_t> counts;
    std::vector<uint32_t> contactIsland(m_contacts.size());
    for (size_t c = 0; c < m_contacts.size(); ++c) {
        const Manifold& m = m_contacts[c];
        uint32_t root = Find(m_bodies[m.a].Dynamic() ? m.a : m.b);
        if (m_islandOf[root] == UINT32_MAX) {
            m_islandOf[root] = static_cast<uint32_t>(counts.size());
            counts.push_back(0);
        }
        contactIsland[c] = m_islandOf[root];
        ++counts[m_islandOf[root]];
    }
    m_islandStart.assign(counts.size() + 1, 0);
    for (size_t i = 0; i < counts.size(); ++i)
        m_islandStart[i + 1] = m_islandStart[i] + counts[i];
    m_islandContacts.resize(m_contacts.size());
    std::vector<uint32_t> cursor(m_islandStart.begin(), m_islandStart.end() - 1);
    for (size_t c = 0; c < m_contacts.size(); ++c)
        m_islandContacts[cursor[contactIsland[c]]++] = static_cast<uint32_t>(c);
}

void Physics::SolveIsland(uint32_t island, float h) {
    const uint32_t begin = m_islandStart[island], end = m_islandStart[island + 1];

    // Подготовка: эффективные массы, bias, warm start
    for (uint32_t k = begin; k < end; ++k) {
        Manifold& m = m_contacts[m_islandContacts[k]];
        Body& A = m_bodies[m.a];
        Body& B = m_bodies[m.b];
        Vec3 center{};
        for (uint32_t i = 0; i < m.count; ++i) {
            ContactPoint& cp = m.pts[i];
            center += cp.p;
            cp.rA = cp.p - A.pos;
            cp.rB = cp.p - B.pos;
            cp.massN = EffectiveMass(A, B, cp.rA, cp.rB, m.n);
            float vn = Dot(VelocityAt(B, cp.rB) - VelocityAt(A, cp.rA), m.n);
            if (cp.depth < 0) {
                cp.bias = cp.depth / h;  // спекулятивный контакт: можно сблизиться на зазор, не больше
                cp.posBias = 0;
            } else {
                cp.bias = vn < -kBounceThreshold ? -m.restitution * vn : 0.0f;
                cp.posBias = std::min(kBaumgarte / h * std::max(cp.depth - kSlop, 0.0f), kMaxBiasVelocity);
            }
            cp.jp = 0;
        }
        center = center / static_cast<float>(m.count);
        m.twistRadius = 0;
        for (uint32_t i = 0; i < m.count; ++i)
            m.twistRadius += Length(m.pts[i].p - center);
        m.twistRadius /= static_cast<float>(m.count);
        m.rA = center - A.pos;
        m.rB = center - B.pos;
        m.massT1 = EffectiveMass(A, B, m.rA, m.rB, m.t1);
        m.massT2 = EffectiveMass(A, B, m.rA, m.rB, m.t2);
        float kt = Dot(m.n, A.invInertiaWorld * m.n) + Dot(m.n, B.invInertiaWorld * m.n);
        m.massTwist = kt > 1e-9f ? 1.0f / kt : 0.0f;

        // warm start
        for (uint32_t i = 0; i < m.count; ++i) {
            ContactPoint& cp = m.pts[i];
            ApplyImpulse(A, -m.n * cp.jn, cp.rA);
            ApplyImpulse(B, m.n * cp.jn, cp.rB);
        }
        Vec3 jt = m.t1 * m.jt1 + m.t2 * m.jt2;
        ApplyImpulse(A, -jt, m.rA);
        ApplyImpulse(B, jt, m.rB);
        ApplyAngular(A, -m.n * m.jtw);
        ApplyAngular(B, m.n * m.jtw);
    }

    for (uint32_t it = 0; it < m_iterations; ++it) {
        for (uint32_t k = begin; k < end; ++k) {
            Manifold& m = m_contacts[m_islandContacts[k]];
            Body& A = m_bodies[m.a];
            Body& B = m_bodies[m.b];

            // Трение в центре пятна: лимит — по суммарному нормальному импульсу
            float sumJn = 0;
            for (uint32_t i = 0; i < m.count; ++i)
                sumJn += m.pts[i].jn;
            float maxF = m.friction * sumJn;

            Vec3 dv = VelocityAt(B, m.rB) - VelocityAt(A, m.rA);
            float l1 = -Dot(dv, m.t1) * m.massT1;
            float n1 = Clamp(m.jt1 + l1, -maxF, maxF);
            l1 = n1 - m.jt1;
            m.jt1 = n1;
            ApplyImpulse(A, -m.t1 * l1, m.rA);
            ApplyImpulse(B, m.t1 * l1, m.rB);

            dv = VelocityAt(B, m.rB) - VelocityAt(A, m.rA);
            float l2 = -Dot(dv, m.t2) * m.massT2;
            float n2 = Clamp(m.jt2 + l2, -maxF, maxF);
            l2 = n2 - m.jt2;
            m.jt2 = n2;
            ApplyImpulse(A, -m.t2 * l2, m.rA);
            ApplyImpulse(B, m.t2 * l2, m.rB);

            // кручение вокруг нормали
            float maxTw = maxF * m.twistRadius;
            float lt = -Dot(B.w - A.w, m.n) * m.massTwist;
            float nt = Clamp(m.jtw + lt, -maxTw, maxTw);
            lt = nt - m.jtw;
            m.jtw = nt;
            ApplyAngular(A, -m.n * lt);
            ApplyAngular(B, m.n * lt);

            // нормаль в каждой точке
            for (uint32_t i = 0; i < m.count; ++i) {
                ContactPoint& cp = m.pts[i];
                dv = VelocityAt(B, cp.rB) - VelocityAt(A, cp.rA);
                float ln = (-Dot(dv, m.n) + cp.bias) * cp.massN;
                float nn = std::max(cp.jn + ln, 0.0f);
                ln = nn - cp.jn;
                cp.jn = nn;
                ApplyImpulse(A, -m.n * ln, cp.rA);
                ApplyImpulse(B, m.n * ln, cp.rB);
            }
        }
    }

    // Split impulse: выталкивание из проникновения на псевдо-скоростях
    const uint32_t splitIters = std::max(4u, m_iterations / 2);
    for (uint32_t it = 0; it < splitIters; ++it) {
        for (uint32_t k = begin; k < end; ++k) {
            Manifold& m = m_contacts[m_islandContacts[k]];
            Body& A = m_bodies[m.a];
            Body& B = m_bodies[m.b];
            for (uint32_t i = 0; i < m.count; ++i) {
                ContactPoint& cp = m.pts[i];
                if (cp.posBias <= 0)
                    continue;
                float dvp = Dot(PseudoVelocityAt(B, cp.rB) - PseudoVelocityAt(A, cp.rA), m.n);
                float lp = (cp.posBias - dvp) * cp.massN;
                float np = std::max(cp.jp + lp, 0.0f);
                lp = np - cp.jp;
                cp.jp = np;
                ApplyPseudo(A, -m.n * lp, cp.rA);
                ApplyPseudo(B, m.n * lp, cp.rB);
            }
        }
    }
}

void Physics::FixedStep(Task& task, float h) {
    CE_ZONE("Grave.FixedStep");
    const uint32_t n = static_cast<uint32_t>(m_bodies.size());
    if (n == 0)
        return;
    const Vec3 g = m_gravity;

    // 1. Скорости + AABB
    task.ParallelFor(n, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            if (b.Dynamic() && !b.sleeping) {
                b.v += (g + b.force * b.invMass) * h;
                b.w += b.invInertiaWorld * b.torque * h;
                b.v *= 1.0f / (1.0f + h * b.linDamp);
                b.w *= 1.0f / (1.0f + h * b.angDamp);
                if (b.lockRot) b.w = Vec3{};
            }
            if (!b.Plane())
                b.aabb = ComputeAabb(b);
        }
    });

    // 2. Broadphase
    Broadphase();

    // 3. Narrowphase (параллельно, у каждой пары свой слот)
    m_narrow.resize(m_pairs.size());
    task.ParallelFor(static_cast<uint32_t>(m_pairs.size()), [&](uint32_t begin, uint32_t end) {
        for (uint32_t p = begin; p < end; ++p) {
            Manifold& m = m_narrow[p];
            m.count = 0;
            auto [ia, ib] = m_pairs[p];
            const Body& A = m_bodies[ia];
            const Body& B = m_bodies[ib];
            ManifoldResult r;
            if (!Collide(A, B, r))
                continue;
            m.a = ia;
            m.b = ib;
            m.n = r.normal;
            TangentBasis(m.n, m.t1, m.t2);
            m.friction = std::sqrt(A.friction * B.friction);
            m.restitution = std::max(A.restitution, B.restitution);
            m.trigger = A.trigger || B.trigger;
            m.count = r.count;
            m.jt1 = m.jt2 = m.jtw = 0;
            auto cached = m_cache.find(PairKey(ia, ib));
            const bool warm = cached != m_cache.end() && Dot(cached->second.n, m.n) > 0.95f;
            if (warm) {
                m.jt1 = cached->second.jt1 * 0.95f;
                m.jt2 = cached->second.jt2 * 0.95f;
                m.jtw = cached->second.jtw * 0.95f;
            }
            for (uint32_t i = 0; i < r.count; ++i) {
                ContactPoint& cp = m.pts[i];
                cp = ContactPoint{};
                cp.p = r.points[i];
                cp.depth = r.depths[i];
                if (warm) {  // warm starting по ближайшей точке прошлого шага
                    for (uint32_t c = 0; c < cached->second.count; ++c) {
                        const CachedPoint& old = cached->second.pts[c];
                        if (LengthSq(old.p - cp.p) < 0.0016f) {
                            cp.jn = old.jn * 0.95f;
                            break;
                        }
                    }
                }
            }
        }
    });

    // 4. Компактизация, события
    m_contacts.clear();
    m_trigNow.clear();
    for (const Manifold& m : m_narrow) {
        if (m.count == 0)
            continue;
        const Body& A = m_bodies[m.a];
        const Body& B = m_bodies[m.b];
        if (m.trigger) {
            TrigPair t{A.entity, A.entityGen, B.entity, B.entityGen, A.trigger, B.trigger, m.pts[0].p, m.n};
            if (B.entity < A.entity || (B.entity == A.entity && B.entityGen < A.entityGen)) {
                std::swap(t.ea, t.eb);
                std::swap(t.ga, t.gb);
                std::swap(t.zoneA, t.zoneB);
                t.normal = -t.normal;
            }
            m_trigNow.push_back(t);
        } else {
            m_contacts.push_back(m);
            if (!m_cache.count(PairKey(m.a, m.b)))
                m_events.push_back({A.entity, A.entityGen, B.entity, B.entityGen, m.pts[0].p, m.n, false});
        }
    }
    std::sort(m_trigNow.begin(), m_trigNow.end());
    {
        // вход = есть сейчас и не было, выход = было и нет (двухуказательный проход по отсортированным спискам)
        auto emit = [&](const TrigPair& t, bool enter) {
            if (t.zoneA) m_trigEvents.push_back({t.ea, t.ga, t.eb, t.gb, enter});
            if (t.zoneB) m_trigEvents.push_back({t.eb, t.gb, t.ea, t.ga, enter});
        };
        size_t i = 0, j = 0;
        while (i < m_trigNow.size() || j < m_trigPrev.size()) {
            if (j == m_trigPrev.size() || (i < m_trigNow.size() && m_trigNow[i] < m_trigPrev[j])) {
                const TrigPair& t = m_trigNow[i++];
                emit(t, true);
                m_events.push_back({t.ea, t.ga, t.eb, t.gb, t.point, t.normal, true});
            } else if (i == m_trigNow.size() || m_trigPrev[j] < m_trigNow[i]) {
                emit(m_trigPrev[j++], false);
            } else {
                ++i;
                ++j;
            }
        }
    }
    m_trigPrev.swap(m_trigNow);

    // 5. Острова и решатель (острова независимы -> параллельно)
    BuildIslands();
    const uint32_t islands = static_cast<uint32_t>(m_islandStart.size()) - 1;
    task.ParallelFor(islands, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i)
            SolveIsland(i, h);
    });

    // 6. Позиции, таймеры сна
    task.ParallelFor(n, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            if (b.type == BodyType::Static || b.sleeping)
                continue;
            b.pos += (b.v + b.pv) * h;
            Vec3 wt = b.w + b.pw;
            b.pv = Vec3{};
            b.pw = Vec3{};
            if (LengthSq(wt) > 0) {
                Quat dq{wt.x, wt.y, wt.z, 0};
                Quat q = dq * b.rot;
                b.rot = Normalize(Quat{b.rot.x + q.x * 0.5f * h, b.rot.y + q.y * 0.5f * h,
                                       b.rot.z + q.z * 0.5f * h, b.rot.w + q.w * 0.5f * h});
                UpdateInertiaWorld(b);
            }
            if (b.Dynamic()) {
                bool still = LengthSq(b.v) < kSleepLinear * kSleepLinear * 25 && LengthSq(b.w) < kSleepAngular * kSleepAngular * 25;
                b.sleepTimer = still ? b.sleepTimer + h : 0.0f;
            }
        }
    });

    // 7. Сон островов: засыпают, только если все тела острова давно неподвижны
    m_islandMinSleep.assign(n, 1e9f);
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic()) {
            uint32_t r = Find(i);
            m_islandMinSleep[r] = std::min(m_islandMinSleep[r], m_bodies[i].sleepTimer);
        }
    for (uint32_t i = 0; i < n; ++i) {
        Body& b = m_bodies[i];
        if (b.Dynamic() && !b.sleeping && m_islandMinSleep[Find(i)] > kSleepTime) {
            b.sleeping = true;
            b.v = Vec3{};
            b.w = Vec3{};
        }
    }

    // 8. Кэш импульсов для warm starting следующего шага
    m_newCache.clear();
    for (const Manifold& m : m_contacts) {
        CachedManifold cm{};
        cm.count = m.count;
        cm.n = m.n;
        cm.jt1 = m.jt1;
        cm.jt2 = m.jt2;
        cm.jtw = m.jtw;
        for (uint32_t i = 0; i < m.count; ++i)
            cm.pts[i] = {m.pts[i].p, m.pts[i].jn};
        m_newCache[PairKey(m.a, m.b)] = cm;
    }
    m_cache.swap(m_newCache);
}

void Physics::AddTriggerCallback(std::function<void(const TriggerEvent&)> cb) {
    std::lock_guard lk(m_cbMutex);
    m_trigCallbacks.push_back(std::move(cb));
}

void Physics::AddTriggerHandler(uint32_t entity, uint32_t gen, bool enter, std::function<void(Entity)> fn) {
    if (!fn)
        return;
    std::lock_guard lk(m_cbMutex);
    Handlers& h = m_handlers[(uint64_t(entity) << 32) | gen];
    if (h.enter.empty() && h.exit.empty()) {
        h.entity = entity;
        h.gen = gen;
        h.since = m_dispatchCount;
    }
    auto p = std::make_shared<std::function<void(Entity)>>(std::move(fn));
    (enter ? h.enter : h.exit).push_back(std::move(p));
}

void Physics::Overlapping(uint32_t entity, uint32_t gen, std::vector<Entity>& out) const {
    std::vector<std::pair<uint32_t, uint32_t>> ids;
    {
        std::shared_lock lk(m_lock);
        for (const TrigPair& t : m_trigPrev) {
            if (t.zoneA && t.ea == entity && t.ga == gen)
                ids.emplace_back(t.eb, t.gb);
            if (t.zoneB && t.eb == entity && t.gb == gen)
                ids.emplace_back(t.ea, t.ga);
        }
    }
    for (auto [e, g] : ids) {
        Entity en = Access::MakeEntity(e, g);
        if (en.Valid())
            out.push_back(en);
    }
}

bool Physics::IsInside(uint32_t zone, uint32_t zoneGen, uint32_t other, uint32_t otherGen) const {
    std::shared_lock lk(m_lock);
    for (const TrigPair& t : m_trigPrev) {
        if (t.zoneA && t.ea == zone && t.ga == zoneGen && t.eb == other && t.gb == otherGen)
            return true;
        if (t.zoneB && t.eb == zone && t.gb == zoneGen && t.ea == other && t.ga == otherGen)
            return true;
    }
    return false;
}

void Physics::DispatchEvents() {
    std::vector<std::function<void(const ContactEvent&)>> cbs;
    std::vector<std::function<void(const TriggerEvent&)>> tcbs;
    {
        std::lock_guard lk(m_cbMutex);
        cbs = m_callbacks;
        tcbs = m_trigCallbacks;
    }
    for (const PendingEvent& e : m_events) {
        ContactEvent ev;
        ev.a = Access::MakeEntity(e.ea, e.ga);
        ev.b = Access::MakeEntity(e.eb, e.gb);
        ev.point = e.point;
        ev.normal = e.normal;
        ev.trigger = e.trigger;
        for (auto& cb : cbs)
            cb(ev);
    }

    for (const TrigEvent& e : m_trigEvents) {
        TriggerEvent ev;
        ev.zone = Access::MakeEntity(e.zone, e.zoneGen);
        ev.other = Access::MakeEntity(e.other, e.otherGen);
        ev.enter = e.enter;
        if (!ev.zone.Valid())
            continue;
        if (e.enter && !ev.other.Valid())
            continue;  // вошёл и тут же удалён: входа никто не увидит
        for (auto& cb : tcbs)
            cb(ev);
        // копия списка: обработчик может добавить новый или удалить зону
        std::vector<std::shared_ptr<std::function<void(Entity)>>> list;
        {
            std::lock_guard lk(m_cbMutex);
            auto it = m_handlers.find((uint64_t(e.zone) << 32) | e.zoneGen);
            if (it != m_handlers.end())
                list = e.enter ? it->second.enter : it->second.exit;
        }
        for (auto& fn : list)
            (*fn)(ev.other);
    }

    // Раз в ~256 кадров убираем обработчики умерших зон (только давно зарегистрированные: сущность, созданная
    // в этом кадре в задаче, станет валидной после синхронизации).
    if ((++m_dispatchCount & 255) == 0) {
        std::lock_guard lk(m_cbMutex);
        for (auto it = m_handlers.begin(); it != m_handlers.end();) {
            if (it->second.since + 4 < m_dispatchCount && !Access::MakeEntity(it->second.entity, it->second.gen).Valid())
                it = m_handlers.erase(it);
            else
                ++it;
        }
    }
}

// ---------------- лучи ----------------

namespace {

// Slab test: пересекает ли луч AABB на отрезке [0, tMax]; tEnter — точка входа
inline bool RayBox(const Aabb& box, Vec3 o, Vec3 d, Vec3 inv, float tMax, float& tEnter) {
    float tmin = 0, tmax = tMax;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-8f) {
            if (o[a] < box.min[a] || o[a] > box.max[a])
                return false;
            continue;
        }
        float t1 = (box.min[a] - o[a]) * inv[a], t2 = (box.max[a] - o[a]) * inv[a];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax)
            return false;
    }
    tEnter = tmin;
    return true;
}

inline Vec3 Center(const Aabb& b) { return (b.min + b.max) * 0.5f; }

} // namespace

void Physics::RefreshQueries() {
    std::unique_lock lk(m_lock);
    if (m_rayValid)
        return;
    CE_ZONE("Grave.RayTree");
    BuildRayTree();
}

void Physics::BuildRayTree() {
    m_rayItems.clear();
    m_rayPlanes.clear();
    m_rayNodes.clear();
    for (uint32_t i = 0; i < m_bodies.size(); ++i) {
        Body& b = m_bodies[i];
        if (b.Plane()) {
            m_rayPlanes.push_back(i);
            continue;
        }
        b.aabb = ComputeAabb(b);  // после интеграции позиций AABB отстаёт на подшаг — обновляем
        m_rayItems.push_back(i);
    }
    if (!m_rayItems.empty()) {
        m_rayNodes.reserve(m_rayItems.size() * 2);
        m_rayNodes.emplace_back();
        BuildRayNode(0, 0, static_cast<uint32_t>(m_rayItems.size()));
    }
    m_rayValid = true;
}

// Медианное разбиение по самой длинной оси центров; листья до 4 тел. Дети узла лежат парой подряд.
void Physics::BuildRayNode(uint32_t node, uint32_t begin, uint32_t end) {
    Aabb box = m_bodies[m_rayItems[begin]].aabb;
    Vec3 cmin = Center(box), cmax = cmin;
    for (uint32_t i = begin + 1; i < end; ++i) {
        const Aabb& b = m_bodies[m_rayItems[i]].aabb;
        box.min = Min(box.min, b.min);
        box.max = Max(box.max, b.max);
        const Vec3 c = Center(b);
        cmin = Min(cmin, c);
        cmax = Max(cmax, c);
    }
    m_rayNodes[node].box = box;
    if (end - begin <= 4) {
        m_rayNodes[node].first = begin;
        m_rayNodes[node].count = end - begin;
        return;
    }
    const Vec3 ext = cmax - cmin;
    const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(m_rayItems.begin() + begin, m_rayItems.begin() + mid, m_rayItems.begin() + end,
                     [&](uint32_t a, uint32_t b) {
                         return Center(m_bodies[a].aabb)[axis] < Center(m_bodies[b].aabb)[axis];
                     });
    const uint32_t left = static_cast<uint32_t>(m_rayNodes.size());
    m_rayNodes.emplace_back();
    m_rayNodes.emplace_back();
    m_rayNodes[node].first = left;
    m_rayNodes[node].count = 0;
    BuildRayNode(left, begin, mid);
    BuildRayNode(left + 1, mid, end);
}

bool Physics::RaycastLinear(Vec3 origin, Vec3 dir, float, uint32_t mask, float& best, const Body*& bestBody,
                            Vec3& bestN) const {
    const Vec3 inv{1.0f / (std::fabs(dir.x) < 1e-8f ? 1.0f : dir.x), 1.0f / (std::fabs(dir.y) < 1e-8f ? 1.0f : dir.y),
                   1.0f / (std::fabs(dir.z) < 1e-8f ? 1.0f : dir.z)};
    for (const Body& b : m_bodies) {
        if (b.trigger || !(b.layer & mask))
            continue;
        float enter;
        if (!b.Plane() && !RayBox(b.aabb, origin, dir, inv, best, enter))
            continue;
        float t;
        Vec3 nrm;
        if (RaycastBody(b, origin, dir, best, t, nrm) && t <= best) {
            best = t;
            bestBody = &b;
            bestN = nrm;
        }
    }
    return bestBody != nullptr;
}

bool Physics::RaycastTree(Vec3 origin, Vec3 dir, float, uint32_t mask, float& best, const Body*& bestBody,
                          Vec3& bestN) const {
    const Vec3 inv{1.0f / (std::fabs(dir.x) < 1e-8f ? 1.0f : dir.x), 1.0f / (std::fabs(dir.y) < 1e-8f ? 1.0f : dir.y),
                   1.0f / (std::fabs(dir.z) < 1e-8f ? 1.0f : dir.z)};
    auto test = [&](uint32_t bi) {
        const Body& b = m_bodies[bi];
        if (b.trigger || !(b.layer & mask))
            return;
        float t;
        Vec3 nrm;
        if (RaycastBody(b, origin, dir, best, t, nrm) && t <= best) {
            best = t;
            bestBody = &b;
            bestN = nrm;
        }
    };
    for (uint32_t pi : m_rayPlanes)
        test(pi);

    if (!m_rayNodes.empty()) {
        float enter;
        if (RayBox(m_rayNodes[0].box, origin, dir, inv, best, enter)) {
            uint32_t stack[64];
            uint32_t sp = 0;
            stack[sp++] = 0;
            while (sp) {
                const RayNode& n = m_rayNodes[stack[--sp]];
                if (n.count) {
                    for (uint32_t k = 0; k < n.count; ++k) {
                        const uint32_t bi = m_rayItems[n.first + k];
                        if (RayBox(m_bodies[bi].aabb, origin, dir, inv, best, enter))
                            test(bi);
                    }
                    continue;
                }
                // Ближний ребёнок обходится первым: дальний часто отсекается по найденному best
                float t0, t1;
                const bool h0 = RayBox(m_rayNodes[n.first].box, origin, dir, inv, best, t0);
                const bool h1 = RayBox(m_rayNodes[n.first + 1].box, origin, dir, inv, best, t1);
                if (h0 && h1) {
                    const bool nearFirst = t0 <= t1;
                    stack[sp++] = nearFirst ? n.first + 1 : n.first;
                    stack[sp++] = nearFirst ? n.first : n.first + 1;
                } else if (h0) {
                    stack[sp++] = n.first;
                } else if (h1) {
                    stack[sp++] = n.first + 1;
                }
            }
        }
    }
    return bestBody != nullptr;
}

bool Physics::Raycast(Vec3 origin, Vec3 dir, float maxDist, RaycastHit& hit, uint32_t mask) const {
    std::shared_lock lk(m_lock);
    dir = Normalize(dir);
    if (LengthSq(dir) == 0)
        return false;
    float best = maxDist;
    const Body* bestBody = nullptr;
    Vec3 bestN;
    const bool found = m_rayValid ? RaycastTree(origin, dir, maxDist, mask, best, bestBody, bestN)
                                  : RaycastLinear(origin, dir, maxDist, mask, best, bestBody, bestN);
    if (!found)
        return false;
    hit.entity = Access::MakeEntity(bestBody->entity, bestBody->entityGen);
    hit.distance = best;
    hit.point = origin + dir * best;
    hit.normal = bestN;
    return true;
}

// ---------------- геометрия для навигации ----------------

namespace {

// Четырёхугольник a-b-c-d (против часовой стрелки, если смотреть со стороны нормали) -> два треугольника
void AddQuad(StaticGeometry& g, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const int base = static_cast<int>(g.verts.size() / 3);
    for (Vec3 v : {a, b, c, d}) {
        g.verts.push_back(v.x);
        g.verts.push_back(v.y);
        g.verts.push_back(v.z);
    }
    for (int i : {0, 1, 2, 0, 2, 3})
        g.tris.push_back(base + i);
}

// Ориентированный бокс: 6 граней. Для грани с осью a: u = e(a+1), v = e(a+2), u x v = +e(a);
// у отрицательной грани u и v меняются местами, и нормаль смотрит наружу.
void AddBox(StaticGeometry& g, Vec3 center, Quat rot, Vec3 half) {
    for (int a = 0; a < 3; ++a) {
        for (int sign = -1; sign <= 1; sign += 2) {
            Vec3 n{}, u{}, v{};
            n[a] = static_cast<float>(sign);
            u[(a + 1) % 3] = half[(a + 1) % 3];
            v[(a + 2) % 3] = half[(a + 2) % 3];
            if (sign < 0)
                std::swap(u, v);
            const Vec3 c = n * half[a];
            auto W = [&](Vec3 local) { return center + rot.Rotate(local); };
            AddQuad(g, W(c - u - v), W(c + u - v), W(c + u + v), W(c - u + v));
        }
    }
}

} // namespace

bool Physics::CollectStaticGeometry(uint32_t layerMask, const Aabb* limit, StaticGeometry& out) const {
    std::shared_lock lk(m_lock);
    out = {};
    Vec3 lo(3.0e38f), hi(-3.0e38f);
    for (const Body& b : m_bodies) {
        if (b.type != BodyType::Static || b.trigger || b.Plane() || !(b.layer & layerMask))
            continue;
        Vec3 half = b.shape.size;
        if (b.shape.type == Shape::Type::Sphere)
            half = Vec3(b.shape.size.x);
        else if (b.shape.type == Shape::Type::Capsule)
            half = {b.shape.size.x, b.shape.size.y + b.shape.size.x, b.shape.size.x};
        AddBox(out, b.pos, b.rot, half);
        lo = Min(lo, b.aabb.min);
        hi = Max(hi, b.aabb.max);
        out.hasExtent = true;
    }

    // Область сетки: заданная явно или габариты тел с запасом (пол из плоскости виден и за крайними стенами)
    Aabb area{};
    if (limit) {
        area = *limit;
    } else if (out.hasExtent) {
        const Vec3 pad(std::max(5.0f, Length(hi - lo) * 0.05f));
        area = {lo - pad, hi + pad};
    }

    bool planes = false;
    for (const Body& b : m_bodies) {
        if (!b.Plane() || b.trigger || !(b.layer & layerMask))
            continue;
        if (!limit && !out.hasExtent)
            return false;  // одна плоскость без области: неизвестно, какой кусок бесконечности брать
        planes = true;
        const Vec3 n = Normalize(b.rot.Rotate({0, 1, 0}));
        const Vec3 mid = (area.min + area.max) * 0.5f;
        const Vec3 c = mid - n * Dot(mid - b.pos, n);  // проекция центра области на плоскость
        const float r = Length(area.max - area.min);   // квадрат со стороной 2r накрывает область при любом наклоне
        Vec3 u, v;
        TangentBasis(n, u, v);  // v = n x u, значит u x v = n: нормаль квадрата смотрит туда же, куда плоскость
        AddQuad(out, c - u * r - v * r, c + u * r - v * r, c + u * r + v * r, c - u * r + v * r);
        if (!limit && n.y > 0.1f) {  // область по высоте должна включать и саму плоскость
            for (float x : {area.min.x, area.max.x})
                for (float z : {area.min.z, area.max.z}) {
                    const float y = b.pos.y - ((x - b.pos.x) * n.x + (z - b.pos.z) * n.z) / n.y;
                    area.min.y = std::min(area.min.y, y - 1.0f);
                    area.max.y = std::max(area.max.y, y + 1.0f);
                }
        }
    }
    out.min = area.min;
    out.max = area.max;
    return out.hasExtent || planes;
}

BodyType Physics::TypeOf(uint32_t bodyIndex) const {
    std::shared_lock lk(m_lock);
    return bodyIndex < m_bodies.size() ? m_bodies[bodyIndex].type : BodyType::Static;
}

} // namespace ce::core
