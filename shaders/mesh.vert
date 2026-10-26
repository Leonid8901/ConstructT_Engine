#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) flat out uint vInstance;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vTangent;

void main() {
    uint idx = drawIndices[gl_InstanceIndex];
    Instance inst = instances[idx];
    vec4 wp = inst.model * vec4(inPos, 1.0);
    vWorldPos = wp.xyz;
    // Нормальная матрица: у частей моделей матрица узла может быть любой (не только TRS сущности)
    mat3 m3 = mat3(inst.model);
    vNormal = transpose(inverse(m3)) * inNormal;
    vTangent = vec4(m3 * inTangent.xyz, inTangent.w);
    vUV = inUV * inst.extra.xy;
    vInstance = idx;
    gl_Position = frame.viewProj * wp;
}
