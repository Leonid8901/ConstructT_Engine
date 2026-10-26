#pragma once
#include "CE/Shapes.h"

#include <cstdint>

namespace ce::core {

struct Aabb {
    Vec3 min, max;
};

struct Body {
    Vec3 pos;
    Quat rot;
    Vec3 v, w;
    Vec3 force, torque;
    Vec3 pv, pw;  // псевдо-скорости split impulse: только исправляют проникновение, энергию не добавляют
    float invMass = 0;
    Vec3 invInertiaLocal;
    Mat3 invInertiaWorld = Mat3::Diagonal(Vec3(0.0f));
    Shape shape;
    BodyType type = BodyType::Dynamic;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linDamp = 0.01f;
    float angDamp = 0.05f;
    bool trigger = false;
    bool lockRot = false;
    bool sleeping = false;
    float sleepTimer = 0;
    uint32_t layer = 1;
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t entity = UINT32_MAX;
    uint32_t entityGen = 0;
    Aabb aabb;

    bool Dynamic() const { return type == BodyType::Dynamic; }
    bool Plane() const { return shape.type == Shape::Type::Plane; }
};

} // namespace ce::core
