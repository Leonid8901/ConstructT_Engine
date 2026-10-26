#include "Nav/NavMesh.h"
#include "Core.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourPathCorridor.h>
#include <Recast.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>

namespace ce::core {

namespace {

std::atomic<uint32_t> g_versionCounter{0};

constexpr int kMaxPath = 512;      // полигонов в одном пути
constexpr int kMaxStraight = 512;  // точек поворота в одном пути
constexpr int kMaxCorners = 4;     // сколько поворотов впереди смотрит агент
constexpr unsigned short kWalkFlag = 1;
constexpr long long kMaxCells = 4'000'000;  // 2000 x 2000 клеток: дальше сетка грубеет сама
constexpr float kMaxHeightCells = 6000.0f;  // предел Recast по высоте (13 бит на спан)
constexpr float kHeightRatio = 0.25f;       // клетка по высоте / по горизонтали: чем мельче, тем ближе сетка к полу

float RandF() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

struct F3 {
    float v[3];
    explicit F3(Vec3 p) : v{p.x, p.y, p.z} {}
    operator const float*() const { return v; }
};
Vec3 ToVec(const float* p) { return {p[0], p[1], p[2]}; }

float Dist(const float* a, const float* b) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float Dist2D(const float* a, const float* b) {
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dz * dz);
}

// Освобождение промежуточных данных Recast, что бы ни случилось по дороге
struct RcScope {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pm = nullptr;
    rcPolyMeshDetail* dm = nullptr;
    ~RcScope() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pm);
        rcFreePolyMeshDetail(dm);
    }
};

} // namespace

struct NavMeshImpl {
    mutable std::shared_mutex mutex;
    dtNavMesh* nav = nullptr;
    std::atomic<uint32_t> version{0};
    NavConfig cfg;
    float cs = 0;
    uint32_t polys = 0;
    dtQueryFilter filter;
    float ext[3] = {2.0f, 2.7f, 2.0f};  // половина коробки поиска ближайшего полигона
    ~NavMeshImpl() {
        if (nav)
            dtFreeNavMesh(nav);
    }
};

namespace {

// Объект запросов Detour на поток: хранит состояние поиска, потому не может быть общим.
struct TlsQuery {
    uint32_t version = 0;
    dtNavMeshQuery* q = nullptr;
    ~TlsQuery() {
        if (q)
            dtFreeNavMeshQuery(q);
    }
};
thread_local TlsQuery t_query;

// Вызывать под общим локом сетки
dtNavMeshQuery* AcquireQuery(const NavMeshImpl& m) {
    if (!m.nav)
        return nullptr;
    if (!t_query.q)
        t_query.q = dtAllocNavMeshQuery();
    if (!t_query.q)
        return nullptr;
    const uint32_t v = m.version.load();
    if (t_query.version != v) {
        if (dtStatusFailed(t_query.q->init(m.nav, 4096))) {
            t_query.version = 0;
            return nullptr;
        }
        t_query.version = v;
    }
    return t_query.q;
}

} // namespace

NavMesh::NavMesh() : m(std::make_unique<NavMeshImpl>()) {}
NavMesh::~NavMesh() = default;

uint32_t NavMesh::Version() const { return m->version.load(); }

uint32_t NavMesh::PolygonCount() const {
    std::shared_lock lk(m->mutex);
    return m->polys;
}

float NavMesh::CellSize() const {
    std::shared_lock lk(m->mutex);
    return m->cs;
}

NavConfig NavMesh::Config() const {
    std::shared_lock lk(m->mutex);
    return m->cfg;
}

void NavMesh::Clear() {
    std::unique_lock lk(m->mutex);
    if (m->nav)
        dtFreeNavMesh(m->nav);
    m->nav = nullptr;
    m->polys = 0;
    m->version = 0;
}

bool NavMesh::Build(const std::vector<float>& verts, const std::vector<int>& tris, const NavConfig& cfg, Vec3 bmin,
                    Vec3 bmax, std::string& error) {
    error.clear();
    const int nv = static_cast<int>(verts.size() / 3);
    const int nt = static_cast<int>(tris.size() / 3);
    if (nv < 3 || nt < 1) {
        error = "нет статической геометрии: добавьте сущностям тела BodyType::Static (боксы или плоскость)";
        return false;
    }
    if (!(bmax.x > bmin.x && bmax.y > bmin.y && bmax.z > bmin.z)) {
        error = "пустая область сетки";
        return false;
    }
    if (cfg.agentRadius <= 0 || cfg.agentHeight <= 0 || cfg.maxClimb < 0) {
        error = "размеры агента должны быть положительными";
        return false;
    }

    // ---- параметры Recast из метров ----
    float cs = cfg.cellSize > 0 ? cfg.cellSize : std::max(0.05f, cfg.agentRadius * 0.5f);
    float lo[3] = {bmin.x, bmin.y, bmin.z}, hi[3] = {bmax.x, bmax.y, bmax.z};
    int gw = 0, gh = 0;
    for (;;) {  // огромная карта -> клетки крупнее, а не гигабайты памяти
        rcCalcGridSize(lo, hi, cs, &gw, &gh);
        if (static_cast<long long>(gw) * gh <= kMaxCells && (hi[1] - lo[1]) / (cs * kHeightRatio) <= kMaxHeightCells)
            break;
        cs *= 1.25f;
    }
    if (cs > (cfg.cellSize > 0 ? cfg.cellSize : std::max(0.05f, cfg.agentRadius * 0.5f)) * 1.01f)
        Log("Nav: область велика, клетка сетки увеличена до " + std::to_string(cs) + " м (укажите boundsMin/boundsMax или cellSize)");

    rcConfig c;
    std::memset(&c, 0, sizeof(c));
    c.cs = cs;
    c.ch = cs * kHeightRatio;
    c.width = gw;
    c.height = gh;
    std::memcpy(c.bmin, lo, sizeof(lo));
    std::memcpy(c.bmax, hi, sizeof(hi));
    c.walkableSlopeAngle = Clamp(cfg.maxSlope, 0.0f, 89.0f);
    c.walkableHeight = static_cast<int>(std::ceil(cfg.agentHeight / c.ch));
    c.walkableClimb = static_cast<int>(std::floor(cfg.maxClimb / c.ch));
    c.walkableRadius = static_cast<int>(std::ceil(cfg.agentRadius / c.cs));
    c.maxEdgeLen = std::max(8, static_cast<int>(12.0f / c.cs));
    c.maxSimplificationError = 1.3f;
    c.minRegionArea = 8 * 8;
    c.mergeRegionArea = 20 * 20;
    c.maxVertsPerPoly = 6;
    c.detailSampleDist = c.cs * 6.0f;
    c.detailSampleMaxError = c.ch;

    rcContext ctx(false);
    RcScope rc;

    // ---- растеризация ----
    rc.hf = rcAllocHeightfield();
    if (!rc.hf || !rcCreateHeightfield(&ctx, *rc.hf, c.width, c.height, c.bmin, c.bmax, c.cs, c.ch)) {
        error = "не хватило памяти под сетку высот";
        return false;
    }
    std::vector<unsigned char> areas(static_cast<size_t>(nt), 0);
    rcMarkWalkableTriangles(&ctx, c.walkableSlopeAngle, verts.data(), nv, tris.data(), nt, areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nv, tris.data(), areas.data(), nt, *rc.hf, c.walkableClimb)) {
        error = "растеризация геометрии не удалась";
        return false;
    }

    // ---- фильтры: ступеньки, обрывы, низкие потолки ----
    rcFilterLowHangingWalkableObstacles(&ctx, c.walkableClimb, *rc.hf);
    rcFilterLedgeSpans(&ctx, c.walkableHeight, c.walkableClimb, *rc.hf);
    rcFilterWalkableLowHeightSpans(&ctx, c.walkableHeight, *rc.hf);

    // ---- регионы, контуры, полигоны ----
    rc.chf = rcAllocCompactHeightfield();
    if (!rc.chf || !rcBuildCompactHeightfield(&ctx, c.walkableHeight, c.walkableClimb, *rc.hf, *rc.chf)) {
        error = "не удалось построить компактную сетку высот";
        return false;
    }
    rcFreeHeightField(rc.hf);
    rc.hf = nullptr;
    if (!rcErodeWalkableArea(&ctx, c.walkableRadius, *rc.chf)) {
        error = "не удалось отступить от стен на радиус агента";
        return false;
    }
    if (!rcBuildDistanceField(&ctx, *rc.chf) || !rcBuildRegions(&ctx, *rc.chf, 0, c.minRegionArea, c.mergeRegionArea)) {
        error = "не удалось разбить поверхность на регионы";
        return false;
    }
    rc.cset = rcAllocContourSet();
    if (!rc.cset || !rcBuildContours(&ctx, *rc.chf, c.maxSimplificationError, c.maxEdgeLen, *rc.cset)) {
        error = "не удалось построить контуры";
        return false;
    }
    rc.pm = rcAllocPolyMesh();
    if (!rc.pm || !rcBuildPolyMesh(&ctx, *rc.cset, c.maxVertsPerPoly, *rc.pm)) {
        error = "не удалось построить полигоны";
        return false;
    }
    rc.dm = rcAllocPolyMeshDetail();
    if (!rc.dm || !rcBuildPolyMeshDetail(&ctx, *rc.pm, *rc.chf, c.detailSampleDist, c.detailSampleMaxError, *rc.dm)) {
        error = "не удалось построить сетку высот полигонов";
        return false;
    }
    if (rc.pm->npolys == 0) {
        error = "проходимой поверхности нет: проверьте размеры агента, углы наклона и что пол — статическое тело";
        return false;
    }
    if (rc.pm->nverts >= 0xffff) {
        error = "слишком много вершин: увеличьте cellSize или ограничьте область";
        return false;
    }
    for (int i = 0; i < rc.pm->npolys; ++i) {
        if (rc.pm->areas[i] == RC_WALKABLE_AREA)
            rc.pm->areas[i] = 0;
        rc.pm->flags[i] = kWalkFlag;
    }

    // ---- данные Detour ----
    dtNavMeshCreateParams p;
    std::memset(&p, 0, sizeof(p));
    p.verts = rc.pm->verts;
    p.vertCount = rc.pm->nverts;
    p.polys = rc.pm->polys;
    p.polyAreas = rc.pm->areas;
    p.polyFlags = rc.pm->flags;
    p.polyCount = rc.pm->npolys;
    p.nvp = rc.pm->nvp;
    p.detailMeshes = rc.dm->meshes;
    p.detailVerts = rc.dm->verts;
    p.detailVertsCount = rc.dm->nverts;
    p.detailTris = rc.dm->tris;
    p.detailTriCount = rc.dm->ntris;
    p.walkableHeight = cfg.agentHeight;
    p.walkableRadius = cfg.agentRadius;
    p.walkableClimb = cfg.maxClimb;
    std::memcpy(p.bmin, rc.pm->bmin, sizeof(p.bmin));
    std::memcpy(p.bmax, rc.pm->bmax, sizeof(p.bmax));
    p.cs = c.cs;
    p.ch = c.ch;
    p.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&p, &navData, &navDataSize)) {
        error = "Detour не смог собрать данные сетки";
        return false;
    }
    dtNavMesh* nav = dtAllocNavMesh();
    if (!nav) {
        dtFree(navData);
        error = "не хватило памяти";
        return false;
    }
    if (dtStatusFailed(nav->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        dtFreeNavMesh(nav);
        error = "Detour не принял данные сетки";
        return false;
    }

    // ---- подмена сетки: поиск, идущий в этот момент, подождёт долю миллисекунды ----
    {
        std::unique_lock lk(m->mutex);
        std::swap(m->nav, nav);
        m->cfg = cfg;
        m->cs = cs;
        m->polys = static_cast<uint32_t>(rc.pm->npolys);
        const float xz = std::max(2.0f, cfg.agentRadius * 4.0f);
        m->ext[0] = m->ext[2] = xz;
        m->ext[1] = std::max(2.5f, cfg.agentHeight * 1.5f);
        m->version = ++g_versionCounter;
    }
    if (nav)
        dtFreeNavMesh(nav);  // прежняя сетка (если была)
    return true;
}

bool NavMesh::FindPath(Vec3 from, Vec3 to, NavPath& out) const {
    out.points.clear();
    out.partial = false;
    std::shared_lock lk(m->mutex);
    dtNavMeshQuery* q = AcquireQuery(*m);
    if (!q)
        return false;
    const F3 s(from), e(to);
    float sp[3], ep[3];
    dtPolyRef sr = 0, er = 0;
    q->findNearestPoly(s, m->ext, &m->filter, &sr, sp);
    q->findNearestPoly(e, m->ext, &m->filter, &er, ep);
    if (!sr || !er)
        return false;

    dtPolyRef polys[kMaxPath];
    int np = 0;
    if (dtStatusFailed(q->findPath(sr, er, sp, ep, &m->filter, polys, &np, kMaxPath)) || np == 0)
        return false;
    float endPos[3] = {ep[0], ep[1], ep[2]};
    if (polys[np - 1] != er) {  // цель недостижима: идём в ближайшую к ней точку достигнутого
        out.partial = true;
        q->closestPointOnPoly(polys[np - 1], ep, endPos, nullptr);
    }

    float straight[kMaxStraight * 3];
    unsigned char flags[kMaxStraight];
    dtPolyRef refs[kMaxStraight];
    int ns = 0;
    if (dtStatusFailed(q->findStraightPath(sp, endPos, polys, np, straight, flags, refs, &ns, kMaxStraight)) || ns == 0)
        return false;
    out.points.reserve(static_cast<size_t>(ns));
    for (int i = 0; i < ns; ++i)
        out.points.push_back(ToVec(&straight[i * 3]));
    return true;
}

bool NavMesh::Nearest(Vec3 p, float radius, Vec3& out) const {
    std::shared_lock lk(m->mutex);
    dtNavMeshQuery* q = AcquireQuery(*m);
    if (!q)
        return false;
    const float ext[3] = {radius, radius, radius};
    dtPolyRef ref = 0;
    float pt[3];
    q->findNearestPoly(F3(p), ext, &m->filter, &ref, pt);
    if (!ref)
        return false;
    out = ToVec(pt);
    return true;
}

bool NavMesh::RandomPoint(Vec3& out) const {
    std::shared_lock lk(m->mutex);
    dtNavMeshQuery* q = AcquireQuery(*m);
    if (!q)
        return false;
    dtPolyRef ref = 0;
    float pt[3];
    if (dtStatusFailed(q->findRandomPoint(&m->filter, RandF, &ref, pt)))
        return false;
    out = ToVec(pt);
    return true;
}

bool NavMesh::RandomPointAround(Vec3 center, float radius, Vec3& out) const {
    std::shared_lock lk(m->mutex);
    dtNavMeshQuery* q = AcquireQuery(*m);
    if (!q)
        return false;
    dtPolyRef ref = 0;
    float start[3];
    q->findNearestPoly(F3(center), m->ext, &m->filter, &ref, start);
    if (!ref)
        return false;
    // Случайная точка круга, к которой идём от центра по поверхности сетки: стены останавливают движение,
    // поэтому результат всегда на сетке, достижим и не дальше radius. Упёрлись в стену — пробуем другую точку.
    radius = std::max(0.0f, radius);
    float best[3] = {start[0], start[1], start[2]};
    for (int attempt = 0; attempt < 12; ++attempt) {
        const float angle = RandF() * 2.0f * kPi;
        const float r = radius * std::sqrt(RandF());
        const float target[3] = {start[0] + std::cos(angle) * r, start[1], start[2] + std::sin(angle) * r};
        float res[3];
        dtPolyRef visited[16];
        int nv = 0;
        if (dtStatusFailed(q->moveAlongSurface(ref, start, target, &m->filter, res, visited, &nv, 16)) || nv == 0)
            continue;
        float h = 0;
        if (dtStatusSucceed(q->getPolyHeight(visited[nv - 1], res, &h)))
            res[1] = h;
        std::memcpy(best, res, sizeof(best));
        const float dx = res[0] - target[0], dz = res[2] - target[2];
        if (dx * dx + dz * dz < 0.05f * 0.05f)  // дошли без помех: точка равномерна по кругу
            break;
    }
    out = ToVec(best);
    return true;
}

bool NavMesh::Raycast(Vec3 from, Vec3 to, NavHit& hit) const {
    std::shared_lock lk(m->mutex);
    dtNavMeshQuery* q = AcquireQuery(*m);
    if (!q)
        return false;
    const F3 s(from), e(to);
    dtPolyRef sr = 0;
    float sp[3];
    q->findNearestPoly(s, m->ext, &m->filter, &sr, sp);
    if (!sr)
        return false;
    float t = 0, normal[3] = {0, 1, 0};
    dtPolyRef path[64];
    int pc = 0;
    if (dtStatusFailed(q->raycast(sr, sp, e, &m->filter, &t, normal, path, &pc, 64)) || t >= FLT_MAX * 0.5f)
        return false;  // дошли до конца без препятствий
    float pt[3] = {sp[0] + (e[0] - sp[0]) * t, sp[1] + (e[1] - sp[1]) * t, sp[2] + (e[2] - sp[2]) * t};
    if (pc > 0) {
        float h = 0;
        if (dtStatusSucceed(q->getPolyHeight(path[pc - 1], pt, &h)))
            pt[1] = h;
    }
    hit.point = ToVec(pt);
    hit.normal = ToVec(normal);
    hit.distance = t * Dist(sp, e);
    return true;
}

void NavMesh::DebugGeometry(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, float lift) const {
    vertices.clear();
    indices.clear();
    std::shared_lock lk(m->mutex);
    if (!m->nav)
        return;
    const dtNavMesh* nav = m->nav;
    for (int ti = 0; ti < nav->getMaxTiles(); ++ti) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header)
            continue;
        for (int pi = 0; pi < tile->header->polyCount; ++pi) {
            const dtPoly* poly = &tile->polys[pi];
            if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;
            const dtPolyDetail* pd = &tile->detailMeshes[pi];
            for (int k = 0; k < pd->triCount; ++k) {
                const unsigned char* t = &tile->detailTris[(pd->triBase + k) * 4];
                Vec3 p[3];
                for (int e = 0; e < 3; ++e) {
                    const float* v = t[e] < poly->vertCount ? &tile->verts[poly->verts[t[e]] * 3]
                                                            : &tile->detailVerts[(pd->vertBase + t[e] - poly->vertCount) * 3];
                    p[e] = {v[0], v[1] + lift, v[2]};
                }
                if (Cross(p[1] - p[0], p[2] - p[0]).y < 0)  // обход против часовой стрелки сверху: виден сверху
                    std::swap(p[1], p[2]);
                const uint32_t base = static_cast<uint32_t>(vertices.size());
                for (int e = 0; e < 3; ++e) {
                    Vertex vtx;
                    vtx.position = p[e];
                    vtx.normal = {0, 1, 0};
                    vertices.push_back(vtx);
                    indices.push_back(base + static_cast<uint32_t>(e));
                }
            }
        }
    }
}

// ---------------- NavWalker ----------------

struct WalkerImpl {
    const NavMeshImpl* mesh = nullptr;
    dtNavMeshQuery* q = nullptr;
    uint32_t qVersion = 0;
    dtPathCorridor corridor;
    bool has = false;
    uint32_t version = 0;
    float optTimer = 0;

    ~WalkerImpl() {
        if (q)
            dtFreeNavMeshQuery(q);
    }
    // под общим локом сетки
    bool EnsureQuery() {
        if (!mesh->nav)
            return false;
        if (!q)
            q = dtAllocNavMeshQuery();
        if (!q)
            return false;
        const uint32_t v = mesh->version.load();
        if (qVersion != v) {
            if (dtStatusFailed(q->init(mesh->nav, 1024))) {
                qVersion = 0;
                return false;
            }
            qVersion = v;
        }
        return true;
    }
};

NavWalker::NavWalker(const NavMesh& mesh) : w(std::make_unique<WalkerImpl>()) {
    w->mesh = mesh.m.get();
    w->corridor.init(kMaxPath);
}
NavWalker::~NavWalker() = default;

bool NavWalker::HasPath() const { return w->has; }
uint32_t NavWalker::MeshVersion() const { return w->version; }
void NavWalker::Clear() { w->has = false; }
Vec3 NavWalker::EndPoint() const { return ToVec(w->corridor.getTarget()); }

bool NavWalker::SetTarget(Vec3 feet, Vec3 target, bool& partial) {
    partial = false;
    w->has = false;
    std::shared_lock lk(w->mesh->mutex);
    if (!w->EnsureQuery())
        return false;
    dtNavMeshQuery* q = w->q;
    const dtQueryFilter* filter = &w->mesh->filter;
    const F3 s(feet), e(target);
    float sp[3], ep[3];
    dtPolyRef sr = 0, er = 0;
    q->findNearestPoly(s, w->mesh->ext, filter, &sr, sp);
    q->findNearestPoly(e, w->mesh->ext, filter, &er, ep);
    if (!sr || !er)
        return false;
    dtPolyRef polys[kMaxPath];
    int np = 0;
    if (dtStatusFailed(q->findPath(sr, er, sp, ep, filter, polys, &np, kMaxPath)) || np == 0)
        return false;
    float endPos[3] = {ep[0], ep[1], ep[2]};
    if (polys[np - 1] != er) {
        partial = true;
        q->closestPointOnPoly(polys[np - 1], ep, endPos, nullptr);
    }
    w->corridor.reset(sr, sp);
    w->corridor.setCorridor(endPos, polys, np);
    w->has = true;
    w->version = w->mesh->version.load();
    w->optTimer = 0;
    return true;
}

bool NavWalker::Advance(Vec3 feet, float dt, Step& out) {
    if (!w->has)
        return false;
    std::shared_lock lk(w->mesh->mutex);
    if (!w->mesh->nav || w->mesh->version.load() != w->version) {  // сетку пересобрали: путь больше недействителен
        w->has = false;
        return false;
    }
    if (!w->EnsureQuery())
        return false;
    dtNavMeshQuery* q = w->q;
    const dtQueryFilter* filter = &w->mesh->filter;
    dtPathCorridor& cor = w->corridor;

    cor.movePosition(F3(feet), q, filter);  // коридор следует за реальным положением (физика может сдвинуть)
    w->optTimer += dt;
    if (w->optTimer > 0.5f) {  // раз в полсекунды — локальная перестройка пути
        w->optTimer = 0;
        cor.optimizePathTopology(q, filter);
    }
    const float* pos = cor.getPos();
    out.pos = ToVec(pos);

    float cv[kMaxCorners * 3];
    unsigned char cf[kMaxCorners];
    dtPolyRef cp[kMaxCorners];
    const int nc = cor.findCorners(cv, cf, cp, kMaxCorners, q, filter);

    int first = 0;  // углы прямо под ногами пропускаем
    while (first < nc && !(cf[first] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) && Dist2D(pos, &cv[first * 3]) <= 0.05f)
        ++first;
    if (first >= nc) {
        out.atEnd = true;
        out.steer = out.pos;
        out.remaining = 0;
        return true;
    }
    out.atEnd = false;
    out.steer = ToVec(&cv[first * 3]);
    float rem = Dist(pos, &cv[first * 3]);
    for (int i = first; i + 1 < nc; ++i)
        rem += Dist(&cv[i * 3], &cv[(i + 1) * 3]);
    if (!(cf[nc - 1] & DT_STRAIGHTPATH_END))  // дальше ещё есть путь, который мы пока не разворачивали
        rem += Dist(&cv[(nc - 1) * 3], cor.getTarget());
    out.remaining = rem;

    // спрямление: если следующий за ближайшим угол виден напрямую, коридор срезает поворот
    const float range = std::max(2.0f, w->mesh->cfg.agentRadius * 30.0f);
    cor.optimizePathVisibility(&cv[std::min(first + 1, nc - 1) * 3], range, q, filter);
    return true;
}

} // namespace ce::core
