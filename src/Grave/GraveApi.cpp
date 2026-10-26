#include "Core.h"
#include "Grave/Physics.h"

namespace ce {

using core::Core;

void GraveApi::SetGravity(Vec3 g) const { Core().grave->SetGravity(g); }
Vec3 GraveApi::Gravity() const { return Core().grave->Gravity(); }
void GraveApi::SetSolverIterations(uint32_t n) const { Core().grave->SetIterations(n); }
void GraveApi::SetFixedRate(float hz) const { Core().grave->SetFixedRate(hz); }

bool GraveApi::Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit& hit, uint32_t mask) const {
    return Core().grave->Raycast(origin, direction, maxDistance, hit, mask);
}

void GraveApi::OnContact(std::function<void(const ContactEvent&)> callback) const {
    Core().grave->AddCallback(std::move(callback));
}

void GraveApi::OnTrigger(std::function<void(const TriggerEvent&)> callback) const {
    Core().grave->AddTriggerCallback(std::move(callback));
}

std::vector<Entity> GraveApi::Overlapping(Entity zone) const {
    std::vector<Entity> out;
    Core().grave->Overlapping(core::Access::Index(zone), core::Access::Gen(zone), out);
    return out;
}

bool GraveApi::IsInside(Entity zone, Entity other) const {
    return Core().grave->IsInside(core::Access::Index(zone), core::Access::Gen(zone), core::Access::Index(other),
                                  core::Access::Gen(other));
}

uint32_t GraveApi::BodyCount() const { return Core().grave->BodyCount(); }
uint32_t GraveApi::AwakeBodyCount() const { return Core().grave->AwakeCount(); }

} // namespace ce
