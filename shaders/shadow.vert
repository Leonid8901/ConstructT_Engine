#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(push_constant) uniform Push { uint cascade; } pc;

void main() {
    vec4 wp = instances[drawIndices[gl_InstanceIndex]].model * vec4(inPos, 1.0);
    vec4 p = frame.cascadeViewProj[pc.cascade] * wp;
    // «Пэнкейкинг»: тенеотбрасыватели перед ближней плоскостью прижимаем к ней, а не отсекаем
    p.z = max(p.z, 0.0);
    gl_Position = p;
}
