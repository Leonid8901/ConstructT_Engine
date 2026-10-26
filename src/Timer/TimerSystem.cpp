#include "Timer/TimerSystem.h"

#include "Core.h"

#include <algorithm>

namespace ce::core {

uint64_t TimerSystem::Add(float delay, float interval, uint32_t times, std::function<void()> fn, Entity owner) {
    if (!fn)
        return 0;
    std::lock_guard lk(m_mutex);
    Item it;
    it.id = m_nextId++;
    it.due = m_now + std::max(delay, 0.0f);
    it.interval = interval;
    it.left = times;
    it.infinite = times == 0;
    it.fn = std::move(fn);
    it.owner = owner;
    it.hasOwner = Access::Index(owner) != UINT32_MAX;
    m_items.push_back(std::move(it));
    return m_items.back().id;
}

bool TimerSystem::Active(uint64_t id) {
    std::lock_guard lk(m_mutex);
    for (const Item& it : m_items)
        if (it.id == id)
            return true;
    return false;
}

void TimerSystem::Cancel(uint64_t id) {
    std::lock_guard lk(m_mutex);
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](const Item& it) { return it.id == id; }),
                  m_items.end());
}

void TimerSystem::CancelAll() {
    std::lock_guard lk(m_mutex);
    m_items.clear();
}

uint32_t TimerSystem::Count() {
    std::lock_guard lk(m_mutex);
    return static_cast<uint32_t>(m_items.size());
}

void TimerSystem::Update(float dt) {
    struct Due {
        double when;
        uint64_t id;
        uint32_t fires;  // сколько раз вызвать (догон пропущенных)
    };
    std::vector<Due> due;
    {
        std::lock_guard lk(m_mutex);
        m_now += dt;
        // таймеры умерших владельцев
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                     [](const Item& it) { return it.hasOwner && !it.owner.Valid(); }),
                      m_items.end());
        for (Item& it : m_items) {
            if (it.due > m_now || it.finished)
                continue;
            const double when = it.due;
            uint32_t fires = 1;
            if (it.interval <= 0) {
                it.finished = true;
            } else {
                // сколько интервалов накопилось с момента it.due (не больше 16 за кадр)
                fires = 1 + std::min<uint32_t>(15, static_cast<uint32_t>((m_now - it.due) / it.interval));
                if (!it.infinite) {
                    fires = std::min(fires, it.left);
                    it.left -= fires;
                    it.finished = it.left == 0;
                }
                it.due += double(it.interval) * fires;
                if (it.due <= m_now)
                    it.due = m_now + it.interval;  // остальное пропускаем
            }
            due.push_back({when, it.id, fires});
        }
    }
    std::sort(due.begin(), due.end(), [](const Due& a, const Due& b) {
        return a.when != b.when ? a.when < b.when : a.id < b.id;
    });
    for (const Due& d : due) {
        std::function<void()> fn;
        bool last = false;
        {
            std::lock_guard lk(m_mutex);
            auto it = std::find_if(m_items.begin(), m_items.end(), [&](const Item& i) { return i.id == d.id; });
            if (it == m_items.end())
                continue;  // отменён обработчиком, сработавшим раньше в этом же кадре
            fn = it->fn;
            last = it->finished;
            if (last)
                m_items.erase(it);
        }
        for (uint32_t k = 0; k < d.fires; ++k) {
            if (k > 0 && !last && !Active(d.id))
                break;  // отменил сам себя
            fn();
        }
    }
}

} // namespace ce::core
