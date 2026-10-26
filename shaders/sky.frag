#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    float t = frame.camForward.w;
    vec3 dir = normalize(frame.camForward.xyz + vNdc.x * t * frame.camRight.w * frame.camRight.xyz -
                         vNdc.y * t * frame.camUp.xyz);
    vec3 col;
    if (dir.y >= 0.0)
        col = mix(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(dir.y, 0.5));
    else
        col = mix(frame.skyHorizon.rgb, frame.skyHorizon.rgb * 0.35, clamp(-dir.y * 4.0, 0.0, 1.0));
    float s = max(dot(dir, -normalize(frame.sunDir.xyz)), 0.0);
    col += frame.sunColor.rgb * (pow(s, 1200.0) * 30.0 + pow(s, 12.0) * 0.25);
    outColor = vec4(Tonemap(col), 1.0);
}
