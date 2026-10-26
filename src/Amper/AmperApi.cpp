// Публичный фасад Amper + Mesh + Light
#include "Amper/Renderer.h"
#include "Core.h"

#include <algorithm>
#include <stdexcept>

namespace ce {

using core::Core;

namespace {
core::Renderer& R() { return *Core().amper; }
}

// ---------------- Amper ----------------

bool AmperApi::RayTracingSupported() const { return R().RtSupported(); }
void AmperApi::SetRayTracing(bool enabled) const { R().SetRayTracing(enabled); }
bool AmperApi::RayTracing() const { return R().RayTracing(); }
void AmperApi::SetSun(Vec3 direction, Vec3 color, float intensity) const { R().SetSun(direction, color, intensity); }
void AmperApi::SetAmbient(Vec3 color, float intensity) const { R().SetAmbient(color, intensity); }
void AmperApi::SetSky(Vec3 zenith, Vec3 horizon) const { R().SetSky(zenith, horizon); }
void AmperApi::SetExposure(float exposure) const { R().SetExposure(exposure); }
void AmperApi::SetShadows(bool enabled) const { R().SetShadows(enabled); }
bool AmperApi::Shadows() const { return R().Shadows(); }
void AmperApi::SetShadowDistance(float meters) const { R().SetShadowDistance(meters); }
void AmperApi::SetAntialiasing(uint32_t samples) const { R().SetSamples(samples); }
uint32_t AmperApi::Antialiasing() const { return R().Samples(); }
uint32_t AmperApi::MaxAntialiasing() const { return R().MaxSamples(); }
bool AmperApi::AntialiasingSupported(uint32_t samples) const { return R().SamplesSupported(samples); }
uint32_t AmperApi::LightCount() const { return R().LightCount(); }
uint32_t AmperApi::DrawCalls() const { return R().DrawCalls(); }
uint32_t AmperApi::Instances() const { return R().Instances(); }

Light AmperApi::AddPointLight(Vec3 position, Vec3 color, float intensity, float range, bool castShadows) const {
    core::GpuLight l;
    l.posRange = {position, range};
    l.colorIntensity = {color, intensity};
    l.params = {castShadows ? 1.0f : 0.0f, 0, 0, 0};
    return R().AddLight(l);
}

// ---------------- Light ----------------

bool Light::Valid() const { return R().LightValid(m_index, m_gen); }

Light& Light::SetPosition(Vec3 p) {
    R().ModifyLight(m_index, m_gen, [&](core::Renderer::LightSlot& s) { s.data.posRange = {p, s.data.posRange.w}; });
    return *this;
}
Light& Light::SetColor(Vec3 c) {
    R().ModifyLight(m_index, m_gen, [&](core::Renderer::LightSlot& s) { s.data.colorIntensity = {c, s.data.colorIntensity.w}; });
    return *this;
}
Light& Light::SetIntensity(float i) {
    R().ModifyLight(m_index, m_gen, [&](core::Renderer::LightSlot& s) { s.data.colorIntensity.w = i; });
    return *this;
}
Light& Light::SetRange(float r) {
    R().ModifyLight(m_index, m_gen, [&](core::Renderer::LightSlot& s) { s.data.posRange.w = r; });
    return *this;
}
Light& Light::SetCastShadows(bool on) {
    R().ModifyLight(m_index, m_gen, [&](core::Renderer::LightSlot& s) { s.data.params.x = on ? 1.0f : 0.0f; });
    return *this;
}
void Light::Remove() { R().RemoveLight(m_index, m_gen); }

// ---------------- Texture / Model ----------------

Texture Texture::Load(const std::string& path, bool srgb) {
    return core::Access::MakeTexture(R().Textures().Load(path, srgb));
}

Texture Texture::Create(const uint8_t* rgba8, uint32_t width, uint32_t height, bool srgb) {
    return core::Access::MakeTexture(R().Textures().Create(rgba8, width, height, srgb));
}

Model Model::Load(const std::string& path) { return core::Access::MakeModel(R().Models().Load(path)); }

namespace {
const core::ModelData* ModelOf(uint32_t id) { return id == UINT32_MAX ? nullptr : R().Models().Get(id); }
} // namespace

uint32_t Model::PartCount() const {
    const auto* m = ModelOf(m_id);
    return m ? static_cast<uint32_t>(m->parts.size()) : 0;
}
Vec3 Model::BoundsMin() const {
    const auto* m = ModelOf(m_id);
    return m ? m->boundsMin : Vec3{};
}
Vec3 Model::BoundsMax() const {
    const auto* m = ModelOf(m_id);
    return m ? m->boundsMax : Vec3{};
}
uint32_t Model::AnimationCount() const {
    const auto* m = ModelOf(m_id);
    return m ? static_cast<uint32_t>(m->clips.size()) : 0;
}
std::string Model::AnimationName(uint32_t index) const {
    const auto* m = ModelOf(m_id);
    return m && index < m->clips.size() ? m->clips[index].name : std::string{};
}
float Model::AnimationDuration(uint32_t index) const {
    const auto* m = ModelOf(m_id);
    return m && index < m->clips.size() ? m->clips[index].duration : 0.0f;
}

// ---------------- Mesh ----------------

Mesh Mesh::Cube() {
    return core::Access::MakeMesh(R().Meshes().Primitive("cube", core::MeshRegistry::GenCube));
}

Mesh Mesh::Sphere(uint32_t segments) {
    return core::Access::MakeMesh(R().Meshes().Primitive(
        "sphere" + std::to_string(segments),
        [&](std::vector<Vertex>& v, std::vector<uint32_t>& i) { core::MeshRegistry::GenSphere(v, i, segments); }));
}

Mesh Mesh::Plane(float size) {
    return core::Access::MakeMesh(R().Meshes().Primitive(
        "plane" + std::to_string(size),
        [&](std::vector<Vertex>& v, std::vector<uint32_t>& i) { core::MeshRegistry::GenPlane(v, i, size); }));
}

Mesh Mesh::Capsule(float radius, float halfHeight, uint32_t segments) {
    return core::Access::MakeMesh(R().Meshes().Primitive(
        "capsule" + std::to_string(radius) + "_" + std::to_string(halfHeight) + "_" + std::to_string(segments),
        [&](std::vector<Vertex>& v, std::vector<uint32_t>& i) {
            core::MeshRegistry::GenCapsule(v, i, radius, halfHeight, segments);
        }));
}

Mesh Mesh::Create(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    const Vec4 def{1, 0, 0, 1};
    const bool needTangents = std::all_of(vertices.begin(), vertices.end(), [&](const Vertex& v) {
        return v.tangent.x == def.x && v.tangent.y == def.y && v.tangent.z == def.z && v.tangent.w == def.w;
    });
    if (!needTangents)
        return core::Access::MakeMesh(R().Meshes().Create(vertices.data(), vertices.size(), indices.data(), indices.size()));
    std::vector<Vertex> v = vertices;
    for (uint32_t i : indices)
        if (i >= v.size())
            throw std::invalid_argument("Mesh::Create: индекс " + std::to_string(i) + " >= числа вершин");
    core::MeshRegistry::ComputeTangents(v, indices);
    return core::Access::MakeMesh(R().Meshes().Create(v.data(), v.size(), indices.data(), indices.size()));
}

Mesh Mesh::LoadOBJ(const std::string& path) {
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    if (!core::MeshRegistry::LoadObj(path, v, i))
        throw std::runtime_error("Mesh::LoadOBJ: не удалось загрузить " + path);
    return Create(v, i);
}

} // namespace ce
