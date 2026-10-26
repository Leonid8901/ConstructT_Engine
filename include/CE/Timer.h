#pragma once
// Таймеры: «через 3 секунды», «каждые 0.5 секунды». Время идёт вместе с кадрами движка (dt), функции вызываются
// в главном потоке между стадиями кадра, после физики, поэтому внутри можно свободно трогать мир.
//
//   Timer.After(3.0f, [] { door.Destroy(); });                    // один раз
//   TimerHandle t = Timer.Every(0.5f, [] { Spawn(); }, monster);  // пока жив monster
//   t.Cancel();
//
// Если указан owner, таймер сам отменяется, когда сущность удалена. Создавать и отменять таймеры можно из задач
// UniX, из обработчиков триггеров и из самих таймеров. Точность — один кадр: функция вызывается в первом кадре,
// когда время вышло (не раньше). После долгого кадра повторяющийся таймер догоняет пропущенные срабатывания
// (не больше 16 за кадр), остальные пропускает.
#include "CE/World.h"

#include <cstdint>
#include <functional>

namespace ce {

namespace core { struct Access; }

class TimerHandle {
public:
    TimerHandle() = default;
    bool Active() const;  // ещё сработает (повторяющийся — пока не отменён)
    void Cancel();        // можно звать повторно и после срабатывания
private:
    friend struct core::Access;
    uint64_t m_id = 0;
};

struct TimerApi {
    // Через seconds секунд (<= 0 — в ближайшем кадре).
    TimerHandle After(float seconds, std::function<void()> fn, Entity owner = {}) const;
    // Каждые seconds секунд (минимум 1 мс); times — сколько раз всего, 0 — пока не отменят.
    TimerHandle Every(float seconds, std::function<void()> fn, Entity owner = {}, uint32_t times = 0) const;
    void CancelAll() const;
    uint32_t Count() const;  // активных таймеров
};
inline constexpr TimerApi Timer{};

} // namespace ce
