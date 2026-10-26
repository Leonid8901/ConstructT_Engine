#pragma once
#include "Grave/Body.h"

#include <cstdint>

namespace ce::core {

struct ManifoldResult {
    Vec3 normal;  // от A к B
    uint32_t count = 0;
    Vec3 points[4];
    float depths[4] = {};
};

// Narrowphase: сфера/бокс/капсула/плоскость в любых сочетаниях. До 4 точек контакта.
bool Collide(const Body& a, const Body& b, ManifoldResult& out);
bool RaycastBody(const Body& body, Vec3 origin, Vec3 dir, float maxT, float& t, Vec3& normal);
Aabb ComputeAabb(const Body& body);

} // namespace ce::core
