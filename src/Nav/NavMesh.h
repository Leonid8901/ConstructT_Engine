#pragma once
// Ядро навигации: сетка проходимости (Recast), запросы пути (Detour) и «ходок» — движение по коридору пути.
// От остального движка не зависит: на входе набор треугольников, на выходе пути и точки.
//
// Потоки: запросы берут общий лок и свои per-thread объекты Detour, поэтому идут параллельно.
// Build строит всё без лока и подменяет сетку под эксклюзивным локом на мгновение.
#include "CE/Nav.h"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ce::core {

struct NavMeshImpl;

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // verts — x y z подряд, tris — тройки индексов (нормаль наружу). bmin/bmax — область сетки.
    bool Build(const std::vector<float>& verts, const std::vector<int>& tris, const NavConfig& config, Vec3 bmin,
               Vec3 bmax, std::string& error);
    void Clear();
    bool Ready() const { return Version() != 0; }
    uint32_t Version() const;  // 0 — сетки нет; меняется при каждой пересборке
    uint32_t PolygonCount() const;
    float CellSize() const;  // фактический (может быть больше запрошенного на огромных картах)
    NavConfig Config() const;

    bool FindPath(Vec3 from, Vec3 to, NavPath& out) const;
    bool Nearest(Vec3 p, float radius, Vec3& out) const;
    bool RandomPoint(Vec3& out) const;
    bool RandomPointAround(Vec3 center, float radius, Vec3& out) const;
    bool Raycast(Vec3 from, Vec3 to, NavHit& hit) const;
    void DebugGeometry(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, float lift) const;

private:
    friend class NavWalker;
    std::unique_ptr<NavMeshImpl> m;
};

struct WalkerImpl;

// Движение по сетке: коридор полигонов от старта к цели, из которого каждый кадр достаётся
// ближайшая точка поворота. Один ходок — один агент (не потокобезопасен; сетка — потокобезопасна).
class NavWalker {
public:
    explicit NavWalker(const NavMesh& mesh);
    ~NavWalker();
    NavWalker(const NavWalker&) = delete;
    NavWalker& operator=(const NavWalker&) = delete;

    // false — старт или цель не нашлись на сетке, пути нет. partial — цель недостижима, идём к ближайшей точке.
    bool SetTarget(Vec3 feet, Vec3 target, bool& partial);
    void Clear();
    bool HasPath() const;
    uint32_t MeshVersion() const;  // под какую версию сетки построен путь (устаревший надо строить заново)

    struct Step {
        Vec3 pos;            // положение на сетке (с её высотой)
        Vec3 steer;          // куда идти сейчас: следующая точка поворота
        float remaining = 0; // длина пути до конца
        bool atEnd = false;  // дошли до конца коридора
    };
    // false — путь устарел или потерян (нужен новый SetTarget).
    bool Advance(Vec3 feet, float dt, Step& out);
    Vec3 EndPoint() const;

private:
    std::unique_ptr<WalkerImpl> w;
};

} // namespace ce::core
