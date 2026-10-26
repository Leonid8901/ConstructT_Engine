#pragma once
// Grave — физический движок (Actor). Тела добавляются через Entity::AddBody.
#include "CE/World.h"

#include <functional>
#include <vector>

namespace ce {

struct RaycastHit {
    Entity entity;
    Vec3 point;
    Vec3 normal;
    float distance = 0;
};

struct ContactEvent {
    Entity a, b;
    Vec3 point;
    Vec3 normal;     // от a к b
    bool trigger = false;
};

// Вход/выход тела из триггера. zone — сущность-триггер, other — тело, вошедшее в него или вышедшее.
// При выходе из-за удаления other может быть уже невалидным (other.Valid() == false): так счётчики «сколько внутри»
// не расходятся. Если невалиден zone, событие не доставляется.
struct TriggerEvent {
    Entity zone;
    Entity other;
    bool enter = true;
};

struct GraveApi {
    void SetGravity(Vec3 g) const;
    Vec3 Gravity() const;
    void SetSolverIterations(uint32_t n) const;
    void SetFixedRate(float hz) const;         // частота фиксированного шага, по умолчанию 120 (240 — для очень высоких стопок)
    // Потокобезопасен, можно вызывать из ParallelFor.
    bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit& hit, uint32_t mask = 0xFFFFFFFFu) const;
    // Вызывается в главном потоке для каждого нового контакта/входа в триггер.
    void OnContact(std::function<void(const ContactEvent&)> callback) const;
    // Вход и выход из триггеров (BodyDesc::trigger), в главном потоке после физики. Для одной зоны проще
    // Entity::OnTriggerEnter / OnTriggerExit. Работает и для спящих тел, и для неподвижной зоны с неподвижным телом.
    void OnTrigger(std::function<void(const TriggerEvent&)> callback) const;
    // Кто сейчас внутри зоны (по состоянию на последний шаг физики). Потокобезопасно.
    std::vector<Entity> Overlapping(Entity zone) const;
    bool IsInside(Entity zone, Entity other) const;
    uint32_t BodyCount() const;
    uint32_t AwakeBodyCount() const;
};
inline constexpr GraveApi Grave{};

} // namespace ce
