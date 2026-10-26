#pragma once
// Таймеры игры. Update — в главном потоке вне стадий; остальное потокобезопасно.
#include "CE/Timer.h"

#include <mutex>
#include <vector>

namespace ce::core {

class TimerSystem {
public:
    uint64_t Add(float delay, float interval, uint32_t times, std::function<void()> fn, Entity owner);
    bool Active(uint64_t id);
    void Cancel(uint64_t id);
    void CancelAll();
    uint32_t Count();
    void Update(float dt);

private:
    struct Item {
        uint64_t id;
        double due;
        float interval;      // 0 — одноразовый
        uint32_t left;       // сколько срабатываний осталось (если не infinite)
        bool infinite;
        bool finished = false;  // отработает в этом Update и будет удалён
        std::function<void()> fn;
        Entity owner;
        bool hasOwner;
    };
    std::mutex m_mutex;
    std::vector<Item> m_items;
    double m_now = 0;
    uint64_t m_nextId = 1;
};

} // namespace ce::core
