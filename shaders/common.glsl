// Общие структуры Amper (совпадают с GpuInstance / GpuLight / FrameUbo в Renderer.h)
struct Instance {
    mat4 model;
    vec4 colorRough;     // rgb — базовый цвет, w — roughness
    vec4 emissiveMetal;  // rgb — свечение, w — metallic
    uvec4 tex;           // baseColor, metallicRoughness, normal, emissive; 0xFFFFFFFF — нет
    vec4 params;         // normalScale, alphaCutoff, occlusionStrength, индекс occlusion (floatBitsToUint)
    vec4 extra;          // uvScale.xy, z — двусторонний
};
const uint NO_TEXTURE = 0xFFFFFFFFu;

struct PointLight {
    vec4 posRange;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 0, binding = 0) uniform FrameData {
    mat4 viewProj;
    vec4 camPos;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 camForward;
    vec4 camRight;
    vec4 camUp;
    uvec4 counts;
    mat4 cascadeViewProj[4];
    vec4 cascadeSplits;
    vec4 cascadeTexel;
    vec4 shadowParams;
} frame;

layout(std430, set = 0, binding = 1) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 2) readonly buffer Lights { PointLight lights[]; };
// Индексы инстансов, прошедших отсечение: gl_InstanceIndex -> индекс в instances[]
layout(std430, set = 0, binding = 5) readonly buffer DrawIndices { uint drawIndices[]; };

vec3 Tonemap(vec3 c) {
    c *= frame.ambient.w;
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);  // ACES (Narkowicz)
    if (frame.shadowParams.y > 0.5) {  // swapchain UNORM: кодируем sRGB сами (обычно это делает формат *_SRGB)
        vec3 lo = c * 12.92;
        vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
        c = mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
    }
    return c;
}
