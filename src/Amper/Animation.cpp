#include "Amper/Animation.h"

#include <algorithm>
#include <cmath>

namespace ce::core {

void RestPose(const ModelData& m, Pose& out) {
    const size_t n = m.nodes.size();
    out.t.resize(n);
    out.r.resize(n);
    out.s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out.t[i] = m.nodes[i].t;
        out.r[i] = m.nodes[i].r;
        out.s[i] = m.nodes[i].s;
    }
}

namespace {

// Hermite для CUBICSPLINE: p0, m0 (out-tangent ключа k), p1, m1 (in-tangent ключа k+1), dt — длина интервала
float Hermite(float p0, float m0, float p1, float m1, float t, float dt) {
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * dt * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * dt * m1;
}

} // namespace

void SampleClip(const AnimClip& clip, float time, Pose& pose) {
    for (const AnimChannel& ch : clip.channels) {
        if (ch.node >= pose.t.size() || ch.times.empty())
            continue;
        const size_t comps = ch.path == AnimPath::Rotation ? 4 : 3;
        const size_t keys = ch.times.size();
        const bool cubic = ch.interp == AnimInterp::CubicSpline;
        // значение ключа k: у CUBICSPLINE тройка (in, value, out), берём value
        auto value = [&](size_t k, size_t c) { return ch.values[(cubic ? k * 3 + 1 : k) * comps + c]; };

        float out[4] = {0, 0, 0, 1};
        if (keys == 1 || time <= ch.times.front()) {
            for (size_t c = 0; c < comps; ++c) out[c] = value(0, c);
        } else if (time >= ch.times.back()) {
            for (size_t c = 0; c < comps; ++c) out[c] = value(keys - 1, c);
        } else {
            const size_t k1 = static_cast<size_t>(std::upper_bound(ch.times.begin(), ch.times.end(), time) - ch.times.begin());
            const size_t k0 = k1 - 1;
            const float dt = ch.times[k1] - ch.times[k0];
            const float t = dt > 0 ? (time - ch.times[k0]) / dt : 0.0f;
            if (ch.interp == AnimInterp::Step) {
                for (size_t c = 0; c < comps; ++c) out[c] = value(k0, c);
            } else if (cubic) {
                for (size_t c = 0; c < comps; ++c) {
                    const float m0 = ch.values[(k0 * 3 + 2) * comps + c];  // out-tangent k0
                    const float m1 = ch.values[(k1 * 3 + 0) * comps + c];  // in-tangent k1
                    out[c] = Hermite(value(k0, c), m0, value(k1, c), m1, t, dt);
                }
            } else if (ch.path == AnimPath::Rotation) {
                const Quat q = Slerp(Quat{value(k0, 0), value(k0, 1), value(k0, 2), value(k0, 3)},
                                     Quat{value(k1, 0), value(k1, 1), value(k1, 2), value(k1, 3)}, t);
                out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
            } else {
                for (size_t c = 0; c < comps; ++c) out[c] = value(k0, c) + (value(k1, c) - value(k0, c)) * t;
            }
        }
        switch (ch.path) {
        case AnimPath::Translation: pose.t[ch.node] = {out[0], out[1], out[2]}; break;
        case AnimPath::Scale: pose.s[ch.node] = {out[0], out[1], out[2]}; break;
        case AnimPath::Rotation: pose.r[ch.node] = Normalize(Quat{out[0], out[1], out[2], out[3]}); break;
        }
    }
}

void BlendPose(Pose& a, const Pose& b, float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    for (size_t i = 0; i < a.t.size(); ++i) {
        a.t[i] = Lerp(a.t[i], b.t[i], w);
        a.s[i] = Lerp(a.s[i], b.s[i], w);
        a.r[i] = Slerp(a.r[i], b.r[i], w);
    }
}

void ComputeGlobals(const ModelData& m, const Pose& pose, std::vector<Mat4>& g) {
    g.resize(m.nodes.size());
    for (uint32_t i : m.order) {
        const ModelNode& n = m.nodes[i];
        const Mat4 local = n.hasMatrix ? n.matrix : Mat4::TRS(pose.t[i], pose.r[i], pose.s[i]);
        g[i] = n.parent >= 0 ? g[n.parent] * local : local;
    }
}

} // namespace ce::core
