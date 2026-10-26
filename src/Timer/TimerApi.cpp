#include "Core.h"
#include "Timer/TimerSystem.h"

#include <algorithm>

namespace ce {

using core::Core;

bool TimerHandle::Active() const { return m_id != 0 && Core().timers->Active(m_id); }
void TimerHandle::Cancel() {
    if (m_id != 0)
        Core().timers->Cancel(m_id);
}

TimerHandle TimerApi::After(float seconds, std::function<void()> fn, Entity owner) const {
    return core::Access::MakeTimer(Core().timers->Add(seconds, 0.0f, 1, std::move(fn), owner));
}

TimerHandle TimerApi::Every(float seconds, std::function<void()> fn, Entity owner, uint32_t times) const {
    return core::Access::MakeTimer(Core().timers->Add(seconds, std::max(seconds, 0.001f), times, std::move(fn), owner));
}

void TimerApi::CancelAll() const { Core().timers->CancelAll(); }
uint32_t TimerApi::Count() const { return Core().timers->Count(); }

} // namespace ce
