// Тело фрагментного шейдера Amper. AMPER_RT включает тени через ray query.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef AMPER_RT
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"

#ifdef AMPER_RT
layout(set = 0, binding = 3) uniform accelerationStructureEXT tlas;

float Visibility(vec3 origin, vec3 dir, float tMax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF,
                          origin, 0.001, dir, tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
float SunVisibility(vec3 pos, vec3 N, vec3 L) { return Visibility(pos + N * 0.02, L, 10000.0); }
#else
float Visibility(vec3 origin, vec3 dir, float tMax) { return 1.0; }

// Каскадные карты теней (когда ray tracing выключен)
layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;

float SunVisibility(vec3 pos, vec3 N, vec3 L) {
    if (frame.shadowParams.x < 0.5) return 1.0;
    float depth = dot(pos - frame.camPos.xyz, frame.camForward.xyz);
    float maxDist = frame.cascadeSplits[3];
    if (depth >= maxDist) return 1.0;
    int c = 0;
    while (c < 3 && depth > frame.cascadeSplits[c]) ++c;

    // Normal offset: сдвиг вдоль нормали на пару текселей каскада убирает «акне» без отрыва теней
    float texel = frame.cascadeTexel[c];
    vec3 p = pos + N * texel * 2.0;
    vec4 lp = frame.cascadeViewProj[c] * vec4(p, 1.0);
    vec2 uv = lp.xy * 0.5 + 0.5;

    // PCF 3x3 (каждый тап — аппаратный 2x2 из-за compare + linear)
    vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float s = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            s += texture(shadowMap, vec4(uv + vec2(x, y) * ts, float(c), lp.z));
    s /= 9.0;

    float fade = clamp((maxDist - depth) / (maxDist * 0.1), 0.0, 1.0);  // плавное исчезновение на краю
    return mix(1.0, s, fade);
}
#endif

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) flat in uint vInstance;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vTangent;
layout(location = 0) out vec4 outColor;

// Все текстуры движка (bindless): индекс берётся из инстанса
layout(set = 1, binding = 0) uniform sampler2D textures[];

vec4 Tex(uint index, vec2 uv) { return texture(textures[nonuniformEXT(index)], uv); }

const float PI = 3.14159265;

vec3 Brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float rough, float metal, vec3 F0) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = rough * rough;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * d * d);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 spec = D * G * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * NdotL;
}

void main() {
    Instance inst = instances[vInstance];
    vec4 base = vec4(inst.colorRough.rgb, 1.0);
    if (inst.tex.x != NO_TEXTURE)
        base *= Tex(inst.tex.x, vUV);
    if (inst.params.y > 0.0 && base.a < inst.params.y)
        discard;  // alphaCutoff: листва, решётки
    vec3 albedo = base.rgb;
    float rough = inst.colorRough.w;
    float metal = inst.emissiveMetal.w;
    if (inst.tex.y != NO_TEXTURE) {
        vec4 mr = Tex(inst.tex.y, vUV);  // glTF: G — roughness, B — metallic
        rough *= mr.g;
        metal *= mr.b;
    }
    rough = clamp(rough, 0.04, 1.0);
    metal = clamp(metal, 0.0, 1.0);

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing)
        N = -N;  // двусторонние материалы: обратная сторона освещается своей нормалью
    if (inst.tex.z != NO_TEXTURE) {
        vec3 T = vTangent.xyz - N * dot(N, vTangent.xyz);
        if (dot(T, T) > 1e-12) {
            T = normalize(T);
            vec3 B = cross(N, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 tn = Tex(inst.tex.z, vUV).xyz * 2.0 - 1.0;
            tn.xy *= inst.params.x;  // normalScale
            N = normalize(mat3(T, B, N) * tn);
        }
    }
    float ao = 1.0;
    uint occTex = floatBitsToUint(inst.params.w);
    if (occTex != NO_TEXTURE)
        ao = 1.0 + inst.params.z * (Tex(occTex, vUV).r - 1.0);
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metal);
    vec3 origin = vWorldPos + N * 0.02;

    vec3 color = vec3(0.0);

    // Солнце
    vec3 Ls = -normalize(frame.sunDir.xyz);
    if (dot(N, Ls) > 0.0)
        color += Brdf(N, V, Ls, albedo, rough, metal, F0) * frame.sunColor.rgb * frame.sunDir.w *
                 SunVisibility(vWorldPos, N, Ls);

    // Точечные источники
    for (uint i = 0; i < frame.counts.x; ++i) {
        PointLight l = lights[i];
        vec3 d = l.posRange.xyz - vWorldPos;
        float dist2 = dot(d, d);
        float r = l.posRange.w;
        if (dist2 > r * r) continue;
        float dist = sqrt(dist2);
        vec3 L = d / dist;
        float f = dist / r;
        float att = clamp(1.0 - f * f * f * f, 0.0, 1.0);
        att = att * att / (dist2 + 1.0);
        float vis = 1.0;
        if (l.params.x > 0.5 && dot(N, L) > 0.0)
            vis = Visibility(origin, L, max(dist - 0.05, 0.0));
        color += Brdf(N, V, L, albedo, rough, metal, F0) * l.colorIntensity.rgb * l.colorIntensity.w * att * vis;
    }

    // Окружение: полусферический диффузный ambient + отражение градиента неба (без него металл чёрный)
    float NdotV = max(dot(N, V), 0.0);
    vec3 R = reflect(-V, N);
    vec3 sky = R.y >= 0.0 ? mix(frame.skyHorizon.rgb, frame.skyZenith.rgb, sqrt(R.y)) : frame.skyHorizon.rgb * 0.3;
    vec3 Fr = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdotV, 5.0);
    color += frame.ambient.rgb * mix(0.45, 1.0, N.y * 0.5 + 0.5) * albedo * (1.0 - metal) * ao;
    color += sky * Fr * (1.0 - 0.7 * rough) * 0.8 * ao;
    vec3 emissive = inst.emissiveMetal.rgb;
    if (inst.tex.w != NO_TEXTURE)
        emissive *= Tex(inst.tex.w, vUV).rgb;
    color += emissive;

    outColor = vec4(Tonemap(color), 1.0);
}
