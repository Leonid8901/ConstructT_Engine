#pragma once
#include "CE/Math.h"

namespace ce {

enum class BodyType : uint8_t { Static, Dynamic, Kinematic };

// Коллайдер. Не зависит от Scale сущности.
struct Shape {
    enum class Type : uint8_t { Sphere, Box, Capsule, Plane };
    Type type = Type::Box;
    Vec3 size{0.5f, 0.5f, 0.5f};  // Sphere: x=радиус; Box: полуразмеры; Capsule: x=радиус, y=полувысота цилиндра

    static Shape Sphere(float radius) { return {Type::Sphere, Vec3(radius)}; }
    static Shape Box(Vec3 halfExtents) { return {Type::Box, halfExtents}; }
    static Shape Capsule(float radius, float halfHeight) { return {Type::Capsule, {radius, halfHeight, radius}}; }
    // Бесконечная плоскость: нормаль = локальная +Y сущности, проходит через её позицию. Только Static.
    static Shape Plane() { return {Type::Plane, Vec3(0.0f)}; }
};

struct BodyDesc {
    BodyType type = BodyType::Dynamic;
    Shape shape{};
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
    float linearDamping = 0.01f;
    float angularDamping = 0.05f;
    bool trigger = false;        // без физического отклика, только события
    bool lockRotation = false;   // удобно для персонажей
    uint32_t layer = 1;
    uint32_t mask = 0xFFFFFFFFu;
};

} // namespace ce
