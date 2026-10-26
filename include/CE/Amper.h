#pragma once
// Amper — движок света. Растеризация (PBR) + аппаратный ray tracing теней по флагу.
#include "CE/Math.h"

#include <cstdint>

namespace ce {

namespace core { struct Access; }

class Light {
public:
    Light() = default;
    bool Valid() const;
    Light& SetPosition(Vec3 p);
    Light& SetColor(Vec3 c);
    Light& SetIntensity(float i);
    Light& SetRange(float r);
    Light& SetCastShadows(bool on);   // работает при включённом ray tracing
    void Remove();

private:
    friend struct core::Access;
    uint32_t m_index = UINT32_MAX;
    uint32_t m_gen = 0;
};

struct AmperApi {
    bool RayTracingSupported() const;
    void SetRayTracing(bool enabled) const;   // если GPU не умеет — тихо остаётся растеризация
    bool RayTracing() const;

    void SetSun(Vec3 direction, Vec3 color, float intensity) const;  // direction — куда светит
    void SetAmbient(Vec3 color, float intensity) const;
    void SetSky(Vec3 zenith, Vec3 horizon) const;
    void SetExposure(float exposure) const;

    // Тени от солнца без RT: каскадные карты теней. При включённом RT вместо них работает ray query.
    void SetShadows(bool enabled) const;
    bool Shadows() const;
    void SetShadowDistance(float meters) const;  // дальность теней от камеры, по умолчанию 80 м

    // Сглаживание краёв (MSAA): 1 — выкл, 2, 4, 8. Применяется со следующего кадра (пересоздаёт цели рендера).
    void SetAntialiasing(uint32_t samples) const;
    uint32_t Antialiasing() const;      // фактическое число сэмплов
    uint32_t MaxAntialiasing() const;   // максимум, который поддерживает GPU
    bool AntialiasingSupported(uint32_t samples) const;  // неподдерживаемое значение округляется вниз

    Light AddPointLight(Vec3 position, Vec3 color, float intensity, float range, bool castShadows = false) const;
    uint32_t LightCount() const;

    uint32_t DrawCalls() const;
    uint32_t Instances() const;
};
inline constexpr AmperApi Amper{};

} // namespace ce
