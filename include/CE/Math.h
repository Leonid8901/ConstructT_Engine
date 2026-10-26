#pragma once
// Собственная математика движка: пользователю не нужен glm.
// Правая система координат, Y вверх, камера смотрит в -Z.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ce {

constexpr float kPi = 3.14159265358979323846f;
constexpr float Radians(float deg) { return deg * (kPi / 180.0f); }
constexpr float Degrees(float rad) { return rad * (180.0f / kPi); }

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator/(Vec3 o) const { return {x / o.x, y / o.y, z / o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }
constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
constexpr float LengthSq(Vec3 v) { return Dot(v, v); }
inline float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(Vec3 v) {
    float l = Length(v);
    return l > 1e-12f ? v / l : Vec3{};
}
constexpr Vec3 Min(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
constexpr Vec3 Max(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 Abs(Vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
constexpr Vec3 Clamp(Vec3 v, Vec3 lo, Vec3 hi) { return Min(Max(v, lo), hi); }
constexpr Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
constexpr float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    constexpr Vec3 Xyz() const { return {x, y, z}; }
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat AxisAngle(Vec3 axis, float radians) {
        axis = Normalize(axis);
        float s = std::sin(radians * 0.5f);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
    }
    // Углы в радианах; порядок применения: roll(Z) -> pitch(X) -> yaw(Y)
    static Quat Euler(float pitch, float yaw, float roll = 0.0f);

    Vec3 Rotate(Vec3 v) const {
        Vec3 u{x, y, z};
        Vec3 t = Cross(u, v) * 2.0f;
        return v + t * w + Cross(u, t);
    }
    constexpr Quat Conjugate() const { return {-x, -y, -z, w}; }
};

constexpr Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Normalize(Quat q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
inline Quat Quat::Euler(float pitch, float yaw, float roll) {
    return AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, pitch) * AxisAngle({0, 0, 1}, roll);
}
constexpr float Dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
// Сферическая интерполяция по кратчайшей дуге (для анимаций)
inline Quat Slerp(Quat a, Quat b, float t) {
    float d = Dot(a, b);
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f)  // почти совпадают: линейно + нормализация
        return Normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    const float th = std::acos(d), s = std::sin(th);
    const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// m[row][col]
struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Mat3 Diagonal(Vec3 d) {
        Mat3 r;
        r.m[0][0] = d.x; r.m[1][1] = d.y; r.m[2][2] = d.z;
        return r;
    }
    static Mat3 FromQuat(Quat q) {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat3 r;
        r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy - wz);     r.m[0][2] = 2 * (xz + wy);
        r.m[1][0] = 2 * (xy + wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz - wx);
        r.m[2][0] = 2 * (xz - wy);     r.m[2][1] = 2 * (yz + wx);     r.m[2][2] = 1 - 2 * (xx + yy);
        return r;
    }
    constexpr Vec3 operator*(Vec3 v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Mat3 Transposed() const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[j][i];
        return r;
    }
    constexpr Vec3 Column(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
};

// Column-major: m[col * 4 + row]
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 Identity() { return {}; }

    static Mat4 TRS(Vec3 t, Quat r, Vec3 s) {
        Mat3 R = Mat3::FromQuat(r);
        Mat4 o;
        for (int c = 0; c < 3; ++c) {
            float sc = s[c];
            o.m[c * 4 + 0] = R.m[0][c] * sc;
            o.m[c * 4 + 1] = R.m[1][c] * sc;
            o.m[c * 4 + 2] = R.m[2][c] * sc;
            o.m[c * 4 + 3] = 0;
        }
        o.m[12] = t.x; o.m[13] = t.y; o.m[14] = t.z; o.m[15] = 1;
        return o;
    }

    // Reverse-Z (ближняя плоскость -> 1, бесконечность -> 0), Y-flip под Vulkan
    static Mat4 PerspectiveReverseZ(float fovY, float aspect, float zNear) {
        float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 o;
        for (float& v : o.m) v = 0;
        o.m[0] = f / aspect;
        o.m[5] = -f;
        o.m[11] = -1.0f;
        o.m[14] = zNear;
        return o;
    }

    // Ортографическая проекция под Vulkan (глубина 0..1, правосторонняя: смотрим в -Z)
    static Mat4 Ortho(float l, float r, float b, float t, float zNear, float zFar) {
        Mat4 o;
        o.m[0] = 2.0f / (r - l);
        o.m[5] = 2.0f / (t - b);
        o.m[10] = -1.0f / (zFar - zNear);
        o.m[12] = -(r + l) / (r - l);
        o.m[13] = -(t + b) / (t - b);
        o.m[14] = -zNear / (zFar - zNear);
        return o;
    }

    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = Normalize(target - eye);
        Vec3 s = Normalize(Cross(f, up));
        Vec3 u = Cross(s, f);
        Mat4 o;
        o.m[0] = s.x; o.m[4] = s.y; o.m[8] = s.z;   o.m[12] = -Dot(s, eye);
        o.m[1] = u.x; o.m[5] = u.y; o.m[9] = u.z;   o.m[13] = -Dot(u, eye);
        o.m[2] = -f.x; o.m[6] = -f.y; o.m[10] = -f.z; o.m[14] = Dot(f, eye);
        o.m[3] = 0; o.m[7] = 0; o.m[11] = 0; o.m[15] = 1;
        return o;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r.m[c * 4 + row] = m[0 * 4 + row] * b.m[c * 4 + 0] + m[1 * 4 + row] * b.m[c * 4 + 1] +
                                   m[2 * 4 + row] * b.m[c * 4 + 2] + m[3 * 4 + row] * b.m[c * 4 + 3];
        return r;
    }

    Vec3 TransformPoint(Vec3 p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    Vec3 TransformVector(Vec3 v) const {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z, m[1] * v.x + m[5] * v.y + m[9] * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }
    // Наибольшая длина базисного вектора: во сколько раз матрица может растянуть радиус
    float MaxScale() const {
        auto len2 = [&](int c) { return m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]; };
        return std::sqrt(std::max(len2(0), std::max(len2(1), len2(2))));
    }
};

} // namespace ce
