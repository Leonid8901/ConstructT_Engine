#include "Grave/Collision.h"

#include <cfloat>
#include <cmath>
#include <utility>

namespace ce::core {

namespace {

constexpr float kEps = 1e-6f;
// Спекулятивный зазор: контакты рождаются чуть раньше касания (глубина < 0 = расстояние).
// Решатель разрешает сближение ровно на этот зазор за шаг — стопки не «дребезжат».
constexpr float kMargin = 0.02f;

struct Segment {
    Vec3 a, b;
};

Segment CapsuleSegment(const Body& c) {
    Vec3 axis = c.rot.Rotate({0, c.shape.size.y, 0});
    return {c.pos - axis, c.pos + axis};
}

Vec3 PlaneNormal(const Body& p) { return p.rot.Rotate({0, 1, 0}); }

float ClosestT(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = b - a;
    float d = Dot(ab, ab);
    return d < kEps ? 0.0f : Clamp(Dot(p - a, ab) / d, 0.0f, 1.0f);
}

// Ericson, Real-Time Collision Detection 5.1.9
void ClosestSegSeg(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, float& s, float& t) {
    Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    if (a <= kEps && e <= kEps) { s = t = 0; return; }
    if (a <= kEps) { s = 0; t = Clamp(f / e, 0, 1); return; }
    float c = Dot(d1, r);
    if (e <= kEps) { t = 0; s = Clamp(-c / a, 0, 1); return; }
    float b = Dot(d1, d2), denom = a * e - b * b;
    s = denom > kEps ? Clamp((b * f - c * e) / denom, 0, 1) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0) { t = 0; s = Clamp(-c / a, 0, 1); }
    else if (t > 1) { t = 1; s = Clamp((b - c) / a, 0, 1); }
}

void AddPoint(ManifoldResult& m, Vec3 p, float depth) {
    for (uint32_t i = 0; i < m.count; ++i)
        if (LengthSq(m.points[i] - p) < 1e-4f) {
            m.depths[i] = std::max(m.depths[i], depth);
            return;
        }
    if (m.count < 4) {
        m.points[m.count] = p;
        m.depths[m.count] = depth;
        ++m.count;
    }
}

// --- примитивные пары (нормаль от первого ко второму) ---

bool SphereSphere(Vec3 ca, float ra, Vec3 cb, float rb, ManifoldResult& m) {
    Vec3 d = cb - ca;
    float rs = ra + rb;
    float d2 = Dot(d, d);
    if (d2 > (rs + kMargin) * (rs + kMargin))
        return false;
    float dist = std::sqrt(d2);
    Vec3 n = dist > kEps ? d / dist : Vec3(0, 1, 0);
    float depth = rs - dist;
    m.normal = n;
    m.count = 0;
    AddPoint(m, ca + n * (ra - depth * 0.5f), depth);
    return true;
}

bool PlaneSphere(const Body& p, Vec3 c, float r, ManifoldResult& m) {
    Vec3 n = PlaneNormal(p);
    float dist = Dot(n, c) - Dot(n, p.pos);
    if (dist > r + kMargin)
        return false;
    m.normal = n;
    m.count = 0;
    AddPoint(m, c - n * ((r + dist) * 0.5f), r - dist);
    return true;
}

bool PlaneBox(const Body& p, const Body& box, ManifoldResult& m) {
    Vec3 n = PlaneNormal(p);
    float pd = Dot(n, p.pos);
    Mat3 R = Mat3::FromQuat(box.rot);
    Vec3 e = box.shape.size;
    Vec3 pts[8];
    float dep[8];
    int count = 0;
    for (int i = 0; i < 8; ++i) {
        Vec3 local{(i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z};
        Vec3 v = box.pos + R * local;
        float d = Dot(n, v) - pd;
        if (d < kMargin) {
            pts[count] = v - n * (d * 0.5f);
            dep[count] = -d;
            ++count;
        }
    }
    if (count == 0)
        return false;
    // оставляем 4 самые глубокие
    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j)
            if (dep[j] > dep[i]) {
                std::swap(dep[i], dep[j]);
                std::swap(pts[i], pts[j]);
            }
    m.normal = n;
    m.count = 0;
    for (int i = 0; i < count && i < 4; ++i)
        AddPoint(m, pts[i], dep[i]);
    return true;
}

bool PlaneCapsule(const Body& p, const Body& cap, ManifoldResult& m) {
    Segment s = CapsuleSegment(cap);
    float r = cap.shape.size.x;
    ManifoldResult a, b;
    bool ha = PlaneSphere(p, s.a, r, a);
    bool hb = PlaneSphere(p, s.b, r, b);
    if (!ha && !hb)
        return false;
    m.normal = PlaneNormal(p);
    m.count = 0;
    if (ha) AddPoint(m, a.points[0], a.depths[0]);
    if (hb) AddPoint(m, b.points[0], b.depths[0]);
    return true;
}

// Нормаль от бокса к сфере
bool BoxSphere(const Body& box, Vec3 c, float r, ManifoldResult& m) {
    Mat3 R = Mat3::FromQuat(box.rot);
    Vec3 e = box.shape.size;
    Vec3 local = R.Transposed() * (c - box.pos);
    Vec3 q = Clamp(local, -e, e);
    Vec3 diff = local - q;
    float d2 = Dot(diff, diff);
    Vec3 nL, pL;
    float depth;
    if (d2 > kEps) {
        if (d2 > (r + kMargin) * (r + kMargin))
            return false;
        float dist = std::sqrt(d2);
        nL = diff / dist;
        depth = r - dist;
        pL = q;
    } else {
        // центр внутри бокса — выталкиваем через ближайшую грань
        int axis = 0;
        float best = FLT_MAX;
        for (int i = 0; i < 3; ++i) {
            float d = e[i] - std::fabs(local[i]);
            if (d < best) { best = d; axis = i; }
        }
        float sgn = local[axis] < 0 ? -1.0f : 1.0f;
        nL = Vec3{};
        nL[axis] = sgn;
        depth = r + best;
        pL = local;
        pL[axis] = e[axis] * sgn;
    }
    m.normal = R * nL;
    m.count = 0;
    AddPoint(m, box.pos + R * pL, depth);
    return true;
}

bool SphereCapsule(Vec3 c, float rs, const Body& cap, ManifoldResult& m) {
    Segment s = CapsuleSegment(cap);
    Vec3 q = s.a + (s.b - s.a) * ClosestT(c, s.a, s.b);
    return SphereSphere(c, rs, q, cap.shape.size.x, m);
}

bool BoxCapsule(const Body& box, const Body& cap, ManifoldResult& m) {
    Segment s = CapsuleSegment(cap);
    float r = cap.shape.size.x;
    Mat3 R = Mat3::FromQuat(box.rot);
    Mat3 Rt = R.Transposed();
    Vec3 e = box.shape.size;
    Vec3 a = Rt * (s.a - box.pos), b = Rt * (s.b - box.pos);

    // ближайшая к боксу точка отрезка: попеременная проекция (сходится для выпуклых тел)
    float t = 0.5f;
    for (int i = 0; i < 4; ++i)
        t = ClosestT(Clamp(a + (b - a) * t, -e, e), a, b);

    float ts[3] = {t, 0.0f, 1.0f};
    float bestDepth = -1;
    m.count = 0;
    for (float ti : ts) {
        ManifoldResult tmp;
        if (BoxSphere(box, s.a + (s.b - s.a) * ti, r, tmp)) {
            if (tmp.depths[0] > bestDepth) {
                bestDepth = tmp.depths[0];
                m.normal = tmp.normal;
            }
            AddPoint(m, tmp.points[0], tmp.depths[0]);
        }
    }
    return m.count > 0;
}

bool CapsuleCapsule(const Body& A, const Body& B, ManifoldResult& m) {
    Segment sa = CapsuleSegment(A), sb = CapsuleSegment(B);
    float ra = A.shape.size.x, rb = B.shape.size.x;
    float s, t;
    ClosestSegSeg(sa.a, sa.b, sb.a, sb.b, s, t);
    ManifoldResult best;
    if (!SphereSphere(sa.a + (sa.b - sa.a) * s, ra, sb.a + (sb.b - sb.a) * t, rb, best))
        return false;
    m = best;
    // концы отрезков дают вторую точку для лежащих рядом капсул
    auto tryPair = [&](Vec3 pa, Vec3 pb) {
        ManifoldResult tmp;
        if (SphereSphere(pa, ra, pb, rb, tmp))
            AddPoint(m, tmp.points[0], tmp.depths[0]);
    };
    tryPair(sa.a, sb.a + (sb.b - sb.a) * ClosestT(sa.a, sb.a, sb.b));
    tryPair(sa.b, sb.a + (sb.b - sb.a) * ClosestT(sa.b, sb.a, sb.b));
    tryPair(sa.a + (sa.b - sa.a) * ClosestT(sb.a, sa.a, sa.b), sb.a);
    tryPair(sa.a + (sa.b - sa.a) * ClosestT(sb.b, sa.a, sa.b), sb.b);
    return true;
}

// Sutherland–Hodgman: оставляем часть полигона, где Dot(n, v) <= offset
int ClipPolygon(const Vec3* in, int count, Vec3 n, float offset, Vec3* out) {
    int outCount = 0;
    for (int i = 0; i < count; ++i) {
        Vec3 a = in[i], b = in[(i + 1) % count];
        float da = Dot(n, a) - offset, db = Dot(n, b) - offset;
        if (da <= 0)
            out[outCount++] = a;
        if ((da <= 0) != (db <= 0)) {
            float t = da / (da - db);
            out[outCount++] = a + (b - a) * t;
        }
    }
    return outCount;
}

// SAT по 15 осям + клиппинг граней (или ребро-ребро)
bool BoxBox(const Body& A, const Body& B, ManifoldResult& m) {
    Mat3 Ra = Mat3::FromQuat(A.rot), Rb = Mat3::FromQuat(B.rot);
    Vec3 ua[3] = {Ra.Column(0), Ra.Column(1), Ra.Column(2)};
    Vec3 ub[3] = {Rb.Column(0), Rb.Column(1), Rb.Column(2)};
    Vec3 ea = A.shape.size, eb = B.shape.size;
    Vec3 t = B.pos - A.pos;

    float bestScore = FLT_MAX, bestOverlap = 0;
    int bestAxis = -1;
    Vec3 bestN;
    auto test = [&](Vec3 L, int id, bool edge) {
        float len2 = Dot(L, L);
        if (len2 < 1e-8f)
            return true;  // почти параллельные рёбра — ось вырождена
        L = L / std::sqrt(len2);
        float ra = ea.x * std::fabs(Dot(ua[0], L)) + ea.y * std::fabs(Dot(ua[1], L)) + ea.z * std::fabs(Dot(ua[2], L));
        float rb = eb.x * std::fabs(Dot(ub[0], L)) + eb.y * std::fabs(Dot(ub[1], L)) + eb.z * std::fabs(Dot(ub[2], L));
        float dist = Dot(t, L);
        float overlap = ra + rb - std::fabs(dist);
        if (overlap < -kMargin)
            return false;
        // Грани стабильнее рёбер; грани A чуть приоритетнее граней B, чтобы опорная грань
        // не «прыгала» между боксами из-за шума float у ровно стоящих стопок.
        float score = edge ? overlap * 1.05f + 0.001f : (id >= 3 ? overlap + 0.0005f : overlap);
        if (score < bestScore) {
            bestScore = score;
            bestOverlap = overlap;
            bestAxis = id;
            bestN = dist < 0 ? -L : L;
        }
        return true;
    };
    for (int i = 0; i < 3; ++i)
        if (!test(ua[i], i, false)) return false;
    for (int i = 0; i < 3; ++i)
        if (!test(ub[i], 3 + i, false)) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!test(Cross(ua[i], ub[j]), 6 + i * 3 + j, true)) return false;

    m.normal = bestN;
    m.count = 0;

    if (bestAxis >= 6) {
        int i = (bestAxis - 6) / 3, j = (bestAxis - 6) % 3;
        Vec3 pA = A.pos, pB = B.pos;
        for (int k = 0; k < 3; ++k) {
            if (k != i) pA += ua[k] * (ea[k] * (Dot(ua[k], bestN) > 0 ? 1.0f : -1.0f));
            if (k != j) pB += ub[k] * (eb[k] * (Dot(ub[k], bestN) > 0 ? -1.0f : 1.0f));
        }
        Vec3 da = ua[i] * ea[i], db = ub[j] * eb[j];
        float s, tt;
        ClosestSegSeg(pA - da, pA + da, pB - db, pB + db, s, tt);
        Vec3 ca = pA - da + da * (2 * s), cb = pB - db + db * (2 * tt);
        AddPoint(m, (ca + cb) * 0.5f, bestOverlap);
        return true;
    }

    const bool refIsA = bestAxis < 3;
    const Vec3* ur = refIsA ? ua : ub;
    const Vec3* ui = refIsA ? ub : ua;
    Vec3 er = refIsA ? ea : eb, ei = refIsA ? eb : ea;
    Vec3 pr = refIsA ? A.pos : B.pos, pi = refIsA ? B.pos : A.pos;
    Vec3 nRef = refIsA ? bestN : -bestN;  // от опорного бокса к инцидентному
    int ra = bestAxis % 3;

    Vec3 faceN = ur[ra] * (Dot(nRef, ur[ra]) > 0 ? 1.0f : -1.0f);
    Vec3 faceC = pr + faceN * er[ra];
    int s1 = (ra + 1) % 3, s2 = (ra + 2) % 3;

    // инцидентная грань — наиболее противонаправленная нормали опорной
    int k = 0;
    float bestDot = -1;
    for (int c = 0; c < 3; ++c) {
        float d = std::fabs(Dot(ui[c], faceN));
        if (d > bestDot) { bestDot = d; k = c; }
    }
    Vec3 incN = ui[k] * (Dot(ui[k], faceN) > 0 ? -1.0f : 1.0f);
    Vec3 incC = pi + incN * ei[k];
    Vec3 a1 = ui[(k + 1) % 3] * ei[(k + 1) % 3], a2 = ui[(k + 2) % 3] * ei[(k + 2) % 3];

    Vec3 bufA[16] = {incC + a1 + a2, incC - a1 + a2, incC - a1 - a2, incC + a1 - a2};
    Vec3 bufB[16];
    int n = 4;
    float c1 = Dot(ur[s1], faceC), c2 = Dot(ur[s2], faceC);
    n = ClipPolygon(bufA, n, ur[s1], c1 + er[s1], bufB);
    n = ClipPolygon(bufB, n, -ur[s1], -c1 + er[s1], bufA);
    n = ClipPolygon(bufA, n, ur[s2], c2 + er[s2], bufB);
    n = ClipPolygon(bufB, n, -ur[s2], -c2 + er[s2], bufA);

    Vec3 pts[16];
    float dep[16];
    int count = 0;
    for (int i = 0; i < n; ++i) {
        float sep = Dot(faceN, bufA[i] - faceC);
        if (sep <= kMargin) {
            pts[count] = bufA[i] - faceN * (sep * 0.5f);
            dep[count] = -sep;
            ++count;
        }
    }
    if (count == 0)
        return false;

    if (count <= 4) {
        for (int i = 0; i < count; ++i)
            AddPoint(m, pts[i], dep[i]);
        return m.count > 0;
    }

    // Сокращаем до 4 точек: самая глубокая + жадно самые удалённые друг от друга
    bool used[16] = {};
    int first = 0;
    for (int i = 1; i < count; ++i)
        if (dep[i] > dep[first]) first = i;
    used[first] = true;
    AddPoint(m, pts[first], dep[first]);
    while (m.count < 4) {
        int pick = -1;
        float bestD = -1;
        for (int i = 0; i < count; ++i) {
            if (used[i]) continue;
            float dmin = FLT_MAX;
            for (uint32_t j = 0; j < m.count; ++j)
                dmin = std::min(dmin, LengthSq(pts[i] - m.points[j]));
            if (dmin > bestD) { bestD = dmin; pick = i; }
        }
        if (pick < 0) break;
        used[pick] = true;
        AddPoint(m, pts[pick], dep[pick]);
    }
    return true;
}

// Упорядоченная пара: тип A <= тип B (Sphere < Box < Capsule < Plane)
bool CollideOrdered(const Body& A, const Body& B, ManifoldResult& m) {
    using T = Shape::Type;
    T ta = A.shape.type, tb = B.shape.type;
    bool flip = false;
    bool hit = false;
    if (ta == T::Sphere) {
        float r = A.shape.size.x;
        switch (tb) {
        case T::Sphere: hit = SphereSphere(A.pos, r, B.pos, B.shape.size.x, m); break;
        case T::Box: hit = BoxSphere(B, A.pos, r, m); flip = true; break;
        case T::Capsule: hit = SphereCapsule(A.pos, r, B, m); break;
        case T::Plane: hit = PlaneSphere(B, A.pos, r, m); flip = true; break;
        }
    } else if (ta == T::Box) {
        switch (tb) {
        case T::Box: hit = BoxBox(A, B, m); break;
        case T::Capsule: hit = BoxCapsule(A, B, m); break;
        case T::Plane: hit = PlaneBox(B, A, m); flip = true; break;
        default: break;
        }
    } else if (ta == T::Capsule) {
        switch (tb) {
        case T::Capsule: hit = CapsuleCapsule(A, B, m); break;
        case T::Plane: hit = PlaneCapsule(B, A, m); flip = true; break;
        default: break;
        }
    }
    if (hit && flip)
        m.normal = -m.normal;
    return hit && m.count > 0;
}

} // namespace

bool Collide(const Body& a, const Body& b, ManifoldResult& out) {
    if (a.shape.type > b.shape.type) {
        if (!CollideOrdered(b, a, out))
            return false;
        out.normal = -out.normal;
        return true;
    }
    return CollideOrdered(a, b, out);
}

Aabb ComputeAabb(const Body& b) {
    constexpr float kAabbMargin = 0.03f;
    Vec3 half;
    switch (b.shape.type) {
    case Shape::Type::Sphere:
        half = Vec3(b.shape.size.x);
        break;
    case Shape::Type::Box: {
        Mat3 R = Mat3::FromQuat(b.rot);
        Vec3 e = b.shape.size;
        for (int i = 0; i < 3; ++i)
            half[i] = std::fabs(R.m[i][0]) * e.x + std::fabs(R.m[i][1]) * e.y + std::fabs(R.m[i][2]) * e.z;
        break;
    }
    case Shape::Type::Capsule:
        half = Abs(b.rot.Rotate({0, b.shape.size.y, 0})) + Vec3(b.shape.size.x);
        break;
    case Shape::Type::Plane:
        half = Vec3(1e30f);
        break;
    }
    half += Vec3(kAabbMargin);
    return {b.pos - half, b.pos + half};
}

namespace {

bool RaySphere(Vec3 o, Vec3 d, Vec3 c, float r, float& t) {
    Vec3 m = o - c;
    float b = Dot(m, d), cc = Dot(m, m) - r * r;
    if (cc > 0 && b > 0)
        return false;
    float disc = b * b - cc;
    if (disc < 0)
        return false;
    t = std::max(-b - std::sqrt(disc), 0.0f);
    return true;
}

} // namespace

bool RaycastBody(const Body& body, Vec3 o, Vec3 d, float maxT, float& t, Vec3& normal) {
    switch (body.shape.type) {
    case Shape::Type::Sphere: {
        if (!RaySphere(o, d, body.pos, body.shape.size.x, t) || t > maxT)
            return false;
        normal = Normalize(o + d * t - body.pos);
        return true;
    }
    case Shape::Type::Box: {
        Mat3 R = Mat3::FromQuat(body.rot), Rt = R.Transposed();
        Vec3 lo = Rt * (o - body.pos), ld = Rt * d, e = body.shape.size;
        float tmin = 0, tmax = maxT;
        int axis = -1;
        float sign = 1;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(ld[i]) < kEps) {
                if (lo[i] < -e[i] || lo[i] > e[i]) return false;
                continue;
            }
            float inv = 1.0f / ld[i];
            float t1 = (-e[i] - lo[i]) * inv, t2 = (e[i] - lo[i]) * inv;
            float s = -1;
            if (t1 > t2) { std::swap(t1, t2); s = 1; }
            if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
        t = tmin;
        Vec3 nL{};
        if (axis >= 0) nL[axis] = sign; else nL = -ld;
        normal = Normalize(R * nL);
        return true;
    }
    case Shape::Type::Capsule: {
        Mat3 R = Mat3::FromQuat(body.rot), Rt = R.Transposed();
        Vec3 lo = Rt * (o - body.pos), ld = Rt * d;
        float r = body.shape.size.x, hh = body.shape.size.y;
        float best = FLT_MAX;
        Vec3 bestN;
        // цилиндрическая часть (ось Y)
        float a = ld.x * ld.x + ld.z * ld.z;
        if (a > kEps) {
            float b = lo.x * ld.x + lo.z * ld.z, c = lo.x * lo.x + lo.z * lo.z - r * r;
            float disc = b * b - a * c;
            if (disc >= 0) {
                float tc = (-b - std::sqrt(disc)) / a;
                if (tc < 0 && c <= 0) tc = 0;
                float y = lo.y + ld.y * tc;
                if (tc >= 0 && y >= -hh && y <= hh) {
                    best = tc;
                    Vec3 p = lo + ld * tc;
                    bestN = Normalize(Vec3{p.x, 0, p.z});
                }
            }
        }
        for (float cy : {-hh, hh}) {
            float ts;
            Vec3 c{0, cy, 0};
            if (RaySphere(lo, ld, c, r, ts) && ts < best) {
                best = ts;
                bestN = Normalize(lo + ld * ts - c);
            }
        }
        if (best > maxT)
            return false;
        t = best;
        normal = R * bestN;
        return true;
    }
    case Shape::Type::Plane: {
        Vec3 n = PlaneNormal(body);
        float denom = Dot(n, d);
        if (denom >= -kEps)
            return false;
        t = (Dot(n, body.pos) - Dot(n, o)) / denom;
        if (t < 0 || t > maxT)
            return false;
        normal = n;
        return true;
    }
    }
    return false;
}

} // namespace ce::core
