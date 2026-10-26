#pragma once
// Пример модуля «Погоня»: враг патрулирует, замечает игрока (дальность, угол обзора, линия видимости),
// гонится за ним по navmesh, теряет и идёт к последнему месту, где видел. Игрок — камера (или SetTarget).
// Нужен построенный Nav (Nav.Build) и сущность врага. Враг — любая сущность: с телом (Dynamic/Kinematic) или без.
//
//   auto* chase = Modules.Add<lib::Chase>();
//   chase->OnSpot([](ce::Entity monster) { /* заметил */ });
//   chase->OnCatch([](ce::Entity monster) { /* догнал */ });
//   chase->Add(monster);                                // дальше он живёт сам
//
// Обработчики вызываются в главном потоке в конце кадра, можно свободно трогать мир.
// engine.json → "modules" → "Chase": sight (дальность зрения, 18 м), loseTime (сколько искать, 5 с),
// chaseSpeed (м/с, 4.5), patrolSpeed (1.8), catchDistance (1.6 м).
#include <CustomEngine.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>

namespace lib {

struct ChaseParams {
    float sight = 18.0f;          // дальность зрения, м
    float fov = 120.0f;           // угол обзора по горизонтали, градусы
    float hearing = 4.0f;         // ближе этого замечает и спиной
    float loseTime = 5.0f;        // столько ищет после потери, потом возвращается к патрулю
    float chaseSpeed = 4.5f;
    float patrolSpeed = 1.8f;
    float patrolRadius = 12.0f;   // патруль вокруг места, где враг добавлен
    float catchDistance = 1.6f;
    float catchPause = 3.0f;      // после «догнал» столько не ловит повторно и идёт патрулировать
};

class Chase : public ce::Module {
public:
    enum class State { Patrol, Chase, Search };

    const char* Name() const override { return "Chase"; }

    void OnLoad() override {
        const ce::ModuleSettings s = Settings();
        m_defaults.sight = s.Number("sight", m_defaults.sight);
        m_defaults.loseTime = s.Number("loseTime", m_defaults.loseTime);
        m_defaults.chaseSpeed = s.Number("chaseSpeed", m_defaults.chaseSpeed);
        m_defaults.patrolSpeed = s.Number("patrolSpeed", m_defaults.patrolSpeed);
        m_defaults.catchDistance = s.Number("catchDistance", m_defaults.catchDistance);
        NewTask("Think", ce::Cost::Low, ce::Priority::Normal, [this](ce::Task&) { Think(); });
    }

    void OnSpot(std::function<void(ce::Entity)> fn) { Lock l(m_mutex); m_onSpot = std::move(fn); }
    void OnLose(std::function<void(ce::Entity)> fn) { Lock l(m_mutex); m_onLose = std::move(fn); }
    void OnCatch(std::function<void(ce::Entity)> fn) { Lock l(m_mutex); m_onCatch = std::move(fn); }
    // Кого ищут: по умолчанию камера. Функция зовётся из задачи — должна быть потокобезопасной.
    void SetTarget(std::function<ce::Vec3()> position) { Lock l(m_mutex); m_target = std::move(position); }

    // Добавить врага (повторно — обновит параметры). Агент навигации создаётся сам.
    void Add(ce::Entity monster) { Add(monster, m_defaults); }
    void Add(ce::Entity monster, const ChaseParams& params) {
        Lock l(m_mutex);
        for (Hunter& h : m_hunters)
            if (h.entity == monster) {
                h.params = params;
                return;
            }
        ce::NavAgentParams ap;
        ap.speed = params.patrolSpeed;
        Hunter h;
        h.entity = monster;
        h.params = params;
        h.agent = ce::Nav.CreateAgent(monster, ap);
        h.home = monster.Position();
        m_hunters.push_back(std::move(h));
    }
    void Remove(ce::Entity monster) {
        Lock l(m_mutex);
        for (size_t i = 0; i < m_hunters.size(); ++i)
            if (m_hunters[i].entity == monster) {
                m_hunters[i].agent.Remove();
                m_hunters.erase(m_hunters.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }
    State StateOf(ce::Entity monster) {
        Lock l(m_mutex);
        for (const Hunter& h : m_hunters)
            if (h.entity == monster)
                return h.state;
        return State::Patrol;
    }
    uint32_t Count() {
        Lock l(m_mutex);
        return static_cast<uint32_t>(m_hunters.size());
    }

    void OnUnload() override {
        Lock l(m_mutex);
        for (Hunter& h : m_hunters)
            h.agent.Remove();
        m_hunters.clear();
    }

private:
    using Lock = std::lock_guard<std::mutex>;
    struct Hunter {
        ce::Entity entity;
        ce::NavAgent agent;
        ChaseParams params;
        State state = State::Patrol;
        ce::Vec3 home, lastSeen;
        double lastSeenAt = 0, nextRepath = 0, waitUntil = 0, pauseUntil = 0;
        bool waiting = false;
        bool seen = false;       // сущность уже была живой (созданная в задаче появляется после синхронизации кадра)
        int grace = 0;
    };

    bool CanSee(const Hunter& h, ce::Vec3 eye, ce::Vec3 player) const {
        ce::Vec3 d = player - eye;
        const float dist = ce::Length(d);
        if (dist > h.params.sight || dist < 1e-3f)
            return false;
        if (dist > h.params.hearing) {  // дальше — только в конусе перед глазами
            ce::Vec3 flat{d.x, 0, d.z};
            ce::Vec3 fwd = h.entity.Forward();
            fwd.y = 0;
            if (ce::LengthSq(flat) > 1e-6f && ce::LengthSq(fwd) > 1e-6f) {
                const float cosHalf = std::cos(h.params.fov * 0.5f * 3.14159265f / 180.0f);
                if (ce::Dot(ce::Normalize(flat), ce::Normalize(fwd)) < cosHalf)
                    return false;
            }
        }
        // Между ними не должно быть стен. Луч начинаем за пределами собственного тела врага, иначе он упрётся в него.
        const ce::Vec3 dir = d / dist;
        const float skip = 0.6f;
        ce::RaycastHit hit;
        return dist <= skip + 0.3f || !ce::Grave.Raycast(eye + dir * skip, dir, dist - skip - 0.3f, hit);
    }

    // события — в главный поток: обработчики не должны зависеть от того, в какой задаче нас вызвали
    void Emit(const std::function<void(ce::Entity)>& fn, ce::Entity e) {
        if (fn)
            ce::Timer.After(0.0f, [fn, e] { fn(e); });
    }

    void Think() {
        if (!ce::Nav.Ready())
            return;
        const double now = ce::Engine::Time();
        Lock l(m_mutex);
        const ce::Vec3 player = m_target ? m_target() : ce::Camera.Position();
        for (size_t i = 0; i < m_hunters.size();) {
            Hunter& h = m_hunters[i];
            if (h.entity.Valid()) {
                h.seen = true;
            } else if (h.seen || ++h.grace > 3) {  // удалён (или так и не появился)
                h.agent.Remove();
                m_hunters.erase(m_hunters.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            } else {
                ++i;
                continue;
            }
            Step(h, player, now);
            ++i;
        }
    }

    void Step(Hunter& h, ce::Vec3 player, double now) {
        const ce::Vec3 eye = h.entity.Position() + ce::Vec3(0, 0.6f, 0);
        const bool paused = now < h.pauseUntil;
        const bool sees = !paused && CanSee(h, eye, player);

        if (sees) {
            h.lastSeen = player;
            h.lastSeenAt = now;
            if (h.state != State::Chase) {
                h.state = State::Chase;
                h.agent.SetSpeed(h.params.chaseSpeed);
                h.nextRepath = 0;
                Emit(m_onSpot, h.entity);
            }
        } else if (h.state == State::Chase) {
            h.state = State::Search;  // потерял из виду: идти туда, где видел в последний раз
            ce::Vec3 goal;
            if (ce::Nav.NearestPoint(h.lastSeen, goal, 8.0f))
                h.agent.MoveTo(goal);
        }

        switch (h.state) {
        case State::Chase:
            if (now >= h.nextRepath) {
                h.nextRepath = now + 0.3;
                ce::Vec3 goal;
                if (ce::Nav.NearestPoint(player, goal, 8.0f))
                    h.agent.MoveTo(goal);
                else
                    h.agent.Stop();  // игрок улетел от сетки
            }
            {
                ce::Vec3 d = h.entity.Position() - player;
                d.y = 0;
                if (ce::Length(d) < h.params.catchDistance && !paused) {
                    h.pauseUntil = now + h.params.catchPause;
                    h.state = State::Patrol;
                    h.agent.SetSpeed(h.params.patrolSpeed);
                    h.agent.Stop();
                    h.waitUntil = now + h.params.catchPause;
                    Emit(m_onCatch, h.entity);
                }
            }
            break;
        case State::Search:
            if (now - h.lastSeenAt > h.params.loseTime) {  // дошёл до места и осматривается, пока не истечёт loseTime
                h.state = State::Patrol;
                h.agent.SetSpeed(h.params.patrolSpeed);
                h.waitUntil = now + 1.0;
                Emit(m_onLose, h.entity);
            }
            break;
        case State::Patrol:
            if (h.agent.Moving()) {
                h.waiting = false;
            } else if (!h.waiting) {  // дошёл (или только начал): постоять секунду, потом новая точка
                h.waiting = true;
                h.waitUntil = std::max(h.waitUntil, now + 1.0);
            } else if (now >= h.waitUntil) {
                h.waiting = false;
                ce::Vec3 goal;
                if (ce::Nav.RandomPointAround(h.home, h.params.patrolRadius, goal))
                    h.agent.MoveTo(goal);
            }
            break;
        }
    }

    ChaseParams m_defaults;
    std::mutex m_mutex;
    std::vector<Hunter> m_hunters;
    std::function<void(ce::Entity)> m_onSpot, m_onLose, m_onCatch;
    std::function<ce::Vec3()> m_target;
};

} // namespace lib
