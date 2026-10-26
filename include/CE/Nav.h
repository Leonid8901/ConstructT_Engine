#pragma once
// Навигация: navmesh (Recast/Detour), поиск пути и агенты, которые сами идут к цели.
//
//   Nav.Build();                                   // один раз после создания уровня (OnStart)
//   NavAgent enemy = Nav.CreateAgent(monster);     // monster: сущность с телом (или без)
//   enemy.MoveTo(Camera.Position());               // дальше агент идёт сам, обходя стены
//
// Геометрия для сетки берётся из статических тел Grave: боксы и плоскости (капсулы и сферы — как
// ограничивающие боксы). Триггеры и движущиеся тела в сетку не попадают.
// Все запросы потокобезопасны и могут вызываться из задач UniX. Build/Clear можно звать и при работающих
// запросах (они на мгновение подождут подмены сетки). Но тела, созданные в той же стадии, в физику ещё не
// попали: стройте сетку в OnStart, из фоновой задачи или на следующем кадре после создания уровня.
#include "CE/Math.h"
#include "CE/World.h"

#include <cstdint>
#include <vector>

namespace ce {

namespace core { struct Access; }

struct NavConfig {
    // Размеры «стандартного» агента: по ним сетка отступает от стен и отсекает низкие проходы.
    float agentRadius = 0.4f;
    float agentHeight = 1.8f;
    float maxClimb = 0.4f;     // ступенька, на которую агент может подняться (и спрыгнуть)
    float maxSlope = 45.0f;    // градусы; круче — непроходимо
    float cellSize = 0.0f;     // точность сетки; 0 — радиус / 2. Больше значение — быстрее и грубее
    // Область, где строится сетка. По умолчанию — по габаритам статических тел плюс запас.
    bool useBounds = false;
    Vec3 boundsMin{};
    Vec3 boundsMax{};
    uint32_t layerMask = 0xFFFFFFFFu;  // тела каких слоёв Grave считаются геометрией
};

struct NavPath {
    std::vector<Vec3> points;  // от старта к цели, по ломаной, в точках поворотов
    bool partial = false;      // цель недостижима: путь ведёт в ближайшую к ней достижимую точку
    float Length() const;
};

struct NavHit {
    Vec3 point;     // где луч упёрся в край сетки (стена или обрыв)
    Vec3 normal;
    float distance = 0;
};

struct NavAgentParams {
    float speed = 3.5f;          // м/с
    float acceleration = 20.0f;  // м/с²: разгон и торможение
    float turnSpeed = 10.0f;     // рад/с: разворот лицом по ходу движения (0 — не поворачивать)
    float stopDistance = 0.3f;   // ближе к цели — считается, что пришёл
    float footOffset = -1.0f;    // от центра сущности до пола; -1 — половина agentHeight
};

// Агент: сущность, которая идёт по сетке к цели. Дескриптор; после Remove или удаления сущности невалиден.
class NavAgent {
public:
    NavAgent() = default;
    bool Valid() const;

    // Построить путь и идти. false — цель не нашлась на сетке или путь не построился
    // (агент в этом случае остановлен). Недостижимая цель: идёт в ближайшую достижимую точку, Partial() == true.
    bool MoveTo(Vec3 target);
    void Stop();                 // встать на месте
    bool Moving() const;         // идёт к цели
    bool Arrived() const;        // дошёл (до цели или до её ближайшей достижимой точки)
    bool Partial() const;        // цель недостижима
    Vec3 Target() const;
    float RemainingDistance() const;  // длина оставшегося пути (на начало кадра)
    NavAgent& SetSpeed(float metersPerSecond);
    void Remove();

private:
    friend struct core::Access;
    uint32_t m_index = UINT32_MAX;
    uint32_t m_gen = 0;
};

struct NavApi {
    bool Build(const NavConfig& config = {}) const;  // из статических тел Grave; true — сетка построена
    void Clear() const;
    bool Ready() const;
    uint32_t PolygonCount() const;

    // Путь по сетке. false — старт или цель слишком далеко от сетки (searchRadius ≈ 2 м) либо пути нет.
    bool FindPath(Vec3 from, Vec3 to, NavPath& path) const;
    // Ближайшая точка сетки к p в пределах radius (по каждой оси).
    bool NearestPoint(Vec3 p, Vec3& out, float radius = 2.0f) const;
    // Случайная точка сетки. RandomPoint берёт любое место карты, в том числе изолированные площадки, куда
    // не дойти (крыша, замкнутая комната): для патруля проверяйте путь. RandomPointAround даёт точку не дальше
    // radius от center и достижимую от него без обходов через стены (патруль, «где-нибудь рядом»).
    bool RandomPoint(Vec3& out) const;
    bool RandomPointAround(Vec3 center, float radius, Vec3& out) const;
    // Луч по сетке от from к to: true — упёрся в край (стена, обрыв), hit — где.
    // false — прямая видимая дорога свободна.
    bool Raycast(Vec3 from, Vec3 to, NavHit& hit) const;

    NavAgent CreateAgent(Entity entity, const NavAgentParams& params = {}) const;
    uint32_t AgentCount() const;

    // Треугольники сетки (для отладки: Mesh::Create(vertices, indices) и сущность поверх пола).
    void DebugGeometry(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, float lift = 0.05f) const;
};
inline constexpr NavApi Nav{};

} // namespace ce
