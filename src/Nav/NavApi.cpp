// Публичный фасад навигации (CE/Nav.h) поверх NavSystem.
#include "CE/Nav.h"
#include "Core.h"
#include "Nav/NavSystem.h"

namespace ce {

namespace {
core::NavSystem& N() { return *core::Core().nav; }
} // namespace

float NavPath::Length() const {
    float len = 0;
    for (size_t i = 1; i < points.size(); ++i)
        len += ce::Length(points[i] - points[i - 1]);
    return len;
}

// ---------------- NavAgent ----------------

bool NavAgent::Valid() const { return m_index != UINT32_MAX && N().Valid(m_index, m_gen); }
bool NavAgent::MoveTo(Vec3 target) { return m_index != UINT32_MAX && N().MoveTo(m_index, m_gen, target); }
void NavAgent::Stop() {
    if (m_index != UINT32_MAX)
        N().Stop(m_index, m_gen);
}
bool NavAgent::Moving() const { return m_index != UINT32_MAX && N().Flag(m_index, m_gen, core::NavSystem::Query::Moving); }
bool NavAgent::Arrived() const { return m_index != UINT32_MAX && N().Flag(m_index, m_gen, core::NavSystem::Query::Arrived); }
bool NavAgent::Partial() const { return m_index != UINT32_MAX && N().Flag(m_index, m_gen, core::NavSystem::Query::Partial); }
Vec3 NavAgent::Target() const { return m_index != UINT32_MAX ? N().Target(m_index, m_gen) : Vec3{}; }
float NavAgent::RemainingDistance() const { return m_index != UINT32_MAX ? N().Remaining(m_index, m_gen) : 0.0f; }
NavAgent& NavAgent::SetSpeed(float metersPerSecond) {
    if (m_index != UINT32_MAX)
        N().SetSpeed(m_index, m_gen, metersPerSecond);
    return *this;
}
void NavAgent::Remove() {
    if (m_index != UINT32_MAX)
        N().Remove(m_index, m_gen);
    m_index = UINT32_MAX;
}

// ---------------- Nav ----------------

bool NavApi::Build(const NavConfig& config) const { return N().Build(config); }
void NavApi::Clear() const { N().Clear(); }
bool NavApi::Ready() const { return N().Mesh().Ready(); }
uint32_t NavApi::PolygonCount() const { return N().Mesh().PolygonCount(); }

bool NavApi::FindPath(Vec3 from, Vec3 to, NavPath& path) const { return N().Mesh().FindPath(from, to, path); }
bool NavApi::NearestPoint(Vec3 p, Vec3& out, float radius) const { return N().Mesh().Nearest(p, radius, out); }
bool NavApi::RandomPoint(Vec3& out) const { return N().Mesh().RandomPoint(out); }
bool NavApi::RandomPointAround(Vec3 center, float radius, Vec3& out) const {
    return N().Mesh().RandomPointAround(center, radius, out);
}
bool NavApi::Raycast(Vec3 from, Vec3 to, NavHit& hit) const { return N().Mesh().Raycast(from, to, hit); }

NavAgent NavApi::CreateAgent(Entity entity, const NavAgentParams& params) const { return N().Create(entity, params); }
uint32_t NavApi::AgentCount() const { return N().AgentCount(); }

void NavApi::DebugGeometry(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, float lift) const {
    N().Mesh().DebugGeometry(vertices, indices, lift);
}

} // namespace ce
