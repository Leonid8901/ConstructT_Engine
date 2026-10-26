#pragma once
// Amper — движок света и рендера.
//   Prepare (стадия Prepare, Actor, параллельно): сбор инстансов по мешам, данные кадра, TLAS-инстансы.
//   Render  (главный поток, Engine): TLAS build -> небо -> инстансированная отрисовка -> present.
// Ray tracing — флаг: PBR-растеризация всегда, тени через ray query поверх, если флаг включён и GPU умеет.
#include "Amper/Animation.h"
#include "Amper/MeshRegistry.h"
#include "Amper/ModelRegistry.h"
#include "Amper/TextureRegistry.h"
#include "CE/Amper.h"
#include "CE/Engine.h"
#include "Vulkan/Gpu.h"
#include "Vulkan/Swapchain.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ce {
class Task;
}

namespace ce::core {

class VulkanContext;
class GpuLanes;
struct EntityData;
class Window;

// Совпадает со struct Instance в shaders/common.glsl (std430, 144 байта)
struct GpuInstance {
    float model[16];
    Vec4 colorRough;     // rgb — базовый цвет, w — roughness
    Vec4 emissiveMetal;  // rgb — свечение, w — metallic
    uint32_t tex[4];     // baseColor, metallicRoughness, normal, emissive; UINT32_MAX — нет
    Vec4 params;         // normalScale, alphaCutoff, occlusionStrength, индекс occlusion (биты uint)
    Vec4 extra;          // uvScale.xy, z = двусторонний, w — резерв
};
static_assert(sizeof(GpuInstance) == 144, "GpuInstance должен совпадать с шейдером");

struct GpuLight {
    Vec4 posRange;
    Vec4 colorIntensity;
    Vec4 params;  // x = отбрасывает тени
};

struct FrameUbo {
    Mat4 viewProj;
    Vec4 camPos;
    Vec4 sunDir;      // xyz — куда светит, w — интенсивность
    Vec4 sunColor;
    Vec4 ambient;     // rgb * intensity, w — экспозиция
    Vec4 skyZenith;
    Vec4 skyHorizon;
    Vec4 camForward;  // w = tan(fov/2)
    Vec4 camRight;    // w = aspect
    Vec4 camUp;
    uint32_t lightCount;
    uint32_t rtEnabled;
    uint32_t pad0, pad1;
    Mat4 cascadeViewProj[4];  // свет -> клип для каждого каскада
    float cascadeSplits[4];   // дальняя граница каскада (глубина вдоль взгляда камеры)
    float cascadeTexel[4];    // размер текселя каскада в метрах (для normal offset)
    Vec4 shadowParams;        // x = тени включены, y = кодировать sRGB в шейдере (swapchain UNORM)
};

class Renderer {
public:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kMaxLights = 256;
    static constexpr uint32_t kCascades = 4;

    Renderer(const VulkanContext& ctx, Gpu& gpu, GpuLanes& lanes, Window& window, const EngineConfig& config);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshRegistry& Meshes() { return *m_meshes; }
    TextureRegistry& Textures() { return *m_textures; }
    ModelRegistry& Models() { return *m_models; }

    void Prepare(Task& task);
    void Render();

    bool RtSupported() const { return m_rtSupported; }
    void SetRayTracing(bool on) { m_rtEnabled.store(on && m_rtSupported); }
    bool RayTracing() const { return m_rtEnabled.load(); }

    void SetSun(Vec3 dir, Vec3 color, float intensity);
    void SetAmbient(Vec3 color, float intensity);
    void SetSky(Vec3 zenith, Vec3 horizon);
    void SetExposure(float e);
    void SetShadows(bool on) { m_shadowsEnabled.store(on); }
    bool Shadows() const { return m_shadowsEnabled.load(); }
    void SetShadowDistance(float d);

    void SetSamples(uint32_t n) { m_requestedSamples.store(n); }
    uint32_t Samples() const { return m_appliedSamples.load(); }
    uint32_t MaxSamples() const { return m_maxSamples; }
    bool SamplesSupported(uint32_t n) const { return n == 1 || (n <= 8 && (m_sampleMask & n) && (n & (n - 1)) == 0); }

    Light AddLight(const GpuLight& l);
    template <class Fn>
    void ModifyLight(uint32_t index, uint32_t gen, Fn&& fn) {
        std::lock_guard lk(m_settingsMutex);
        if (index < m_lights.size() && m_lights[index].alive && m_lights[index].gen == gen)
            fn(m_lights[index]);
    }
    bool LightValid(uint32_t index, uint32_t gen);
    void RemoveLight(uint32_t index, uint32_t gen);
    uint32_t LightCount();

    uint32_t DrawCalls() const { return m_drawCalls.load(); }
    uint32_t Instances() const { return m_instances.load(); }

    struct LightSlot {
        GpuLight data;
        uint32_t gen = 0;
        bool alive = false;
    };

private:
    // first/count — диапазон в буфере drawIndices (индексы инстансов, прошедших отсечение)
    struct Draw {
        uint32_t mesh, first, count;
        bool doubleSided;
        VkBuffer vertices = VK_NULL_HANDLE;  // не пусто — свои вершины (скиннинг), индексы — меша
    };

    // Часть модели со скином у конкретной сущности: свои скиннутые вершины и свой BLAS
    struct SkinnedPart {
        uint32_t part = 0;
        uint32_t mesh = 0;
        Buffer out;
        Buffer blasBuffer, scratch;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkDeviceAddress blasAddress = 0;
        bool built = false;
        uint32_t jointBase = 0;  // в матрицах, в буфере костей кадра
    };
    // Состояние анимации сущности с анимированной моделью
    struct Animator {
        uint32_t entity = 0, gen = 0, model = 0;
        uint32_t serial = UINT32_MAX;
        int32_t clip = -1, prevClip = -1;
        float time = 0, prevTime = 0, speed = 1;
        bool loop = true, prevLoop = true, playing = false;
        float fade = 0, fadeDur = 0;  // переход идёт, пока fade < fadeDur
        Pose pose, prevPose;
        std::vector<Mat4> globals;
        std::vector<SkinnedPart> skinned;  // размер фиксирован после создания (на элементы держим указатели)
        std::vector<uint32_t> skinBase;    // по скинам модели: начало палитры костей в буфере кадра
        uint64_t seen = 0;
    };
    struct Retired {  // ресурсы удалённых аниматоров: освобождаем, когда кадры в полёте их отпустили
        std::vector<SkinnedPart> parts;
        uint64_t frame = 0;
    };
    struct SkinJob {  // push constants skin.comp (совпадает с шейдером)
        VkDeviceAddress src, skin, joints, dst;
        uint32_t count, pad;
    };

    // Один инстанс к отрисовке: сущность целиком (меш) или часть её модели
    struct InstanceRef {
        uint32_t entity;
        uint32_t part;  // UINT32_MAX — меш сущности, иначе индекс части модели
        uint32_t key;   // группа отрисовки: doubleSided * meshCount + mesh
        uint32_t slot;  // номер внутри группы
        const Animator* anim;  // не пусто — матрица узла из анимации
    };
    struct SkinRef {
        uint32_t entity;
        uint32_t part;
        SkinnedPart* sp;
    };

    // Объём каскада в пространстве света: центр по X/Y, полуразмер и дальняя граница по глубине
    struct ShadowCull {
        Mat4 lightView;
        std::array<Vec4, 4> box{};  // (cx, cy, half, far)
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer ubo, instances, lights;
        uint32_t instanceCapacity = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Buffer tlasInstances, tlasBuffer, scratch;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        uint32_t tlasCapacity = 0;
        uint32_t tlasCount = 0;
        Buffer drawIndices;  // [камера | каскад 0 | … | каскад 3], uint32 индексы инстансов
        uint32_t drawIndexCapacity = 0;
        bool rt = false;
        bool shadows = false;
        std::vector<Draw> draws;                                // основной проход
        std::array<std::vector<Draw>, kCascades> shadowDraws;  // проходы каскадов
        Buffer joints;  // палитры костей аниматоров (Stream, по адресу из skin.comp)
        uint32_t jointCapacity = 0;
        std::vector<SkinJob> skinJobs;
        std::vector<SkinnedPart*> skinBlas;  // BLAS скиннутых частей к сборке/обновлению
    };

    void CreateFrames();
    void CreateDescriptors();
    void CreatePipelines();
    void DestroyPipelines();
    void CreateTargets();  // глубина + MSAA-цвет под размер swapchain
    void ApplySampleChange();
    VkSampleCountFlagBits PickSamples(uint32_t requested) const;
    void CreateShadowResources();
    void ComputeCascades(FrameUbo& ubo, ShadowCull& cull, Vec3 camPos, Vec3 fwd, Vec3 right, Vec3 up, float tanHalf,
                         float aspect, Vec3 sunDir, float distance);
    void RecordShadowPass(Frame& f);
    void CreateRenderFinished();
    void DestroyRenderFinished();
    void EnsureInstanceCapacity(Frame& f, uint32_t count);
    void EnsureDrawIndexCapacity(Frame& f, uint32_t count);
    void EnsureTlasCapacity(Frame& f, uint32_t count);
    void WriteDescriptor(Frame& f, uint32_t binding);
    void HandleResize();
    void Record(Frame& f, uint32_t imageIndex);
    Animator* SyncAnimator(uint32_t entity, const EntityData& d, const ModelData& md);
    void CreateSkinnedPart(SkinnedPart& sp, const MeshGpu& mesh);
    void DestroySkinnedPart(SkinnedPart& sp);
    void Animate(Animator& a, float dt);
    void RecordSkinning(Frame& f);
    void CreateSkinPipeline();
    void EnsureJointCapacity(Frame& f, uint32_t count);

    const VulkanContext& m_ctx;
    Gpu& m_gpu;
    GpuLanes& m_lanes;
    Window& m_window;
    VkDevice m_device;
    bool m_rtSupported;
    std::atomic<bool> m_rtEnabled{false};

    std::unique_ptr<Swapchain> m_swapchain;
    std::unique_ptr<MeshRegistry> m_meshes;
    std::unique_ptr<TextureRegistry> m_textures;
    std::unique_ptr<ModelRegistry> m_models;
    Image m_depth;
    Image m_msaaColor;  // только при m_samples > 1: рисуем в него, resolve — в изображение swapchain
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;   // D32 -> X8_D24 -> D16, что поддерживает GPU
    VkFormat m_shadowFormat = VK_FORMAT_D32_SFLOAT;
    bool m_shadowLinear = true;                       // линейная фильтрация глубины (бесплатный 2x2 PCF)
    uint32_t m_shadowSize = 2048;

    // MSAA
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t m_maxSamples = 1;
    VkSampleCountFlags m_sampleMask = VK_SAMPLE_COUNT_1_BIT;
    std::atomic<uint32_t> m_requestedSamples{1};
    std::atomic<uint32_t> m_appliedSamples{1};

    std::array<Frame, kFramesInFlight> m_frames{};
    std::vector<VkSemaphore> m_renderFinished;
    uint32_t m_frameIndex = 0;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_skyPipeline = VK_NULL_HANDLE;
    VkPipeline m_meshPipeline = VK_NULL_HANDLE;
    VkPipeline m_meshRtPipeline = VK_NULL_HANDLE;
    VkPipeline m_meshPipeline2s = VK_NULL_HANDLE;    // двусторонние материалы (без отсечения задних граней)
    VkPipeline m_meshRtPipeline2s = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;

    // Каскадные карты теней
    Image m_shadowMap;
    std::array<VkImageView, kCascades> m_shadowLayerViews{};
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    std::atomic<bool> m_shadowsEnabled{true};
    float m_shadowDistance = 80.0f;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;

    // Состояние сцены освещения
    std::mutex m_settingsMutex;
    Vec3 m_sunDir = Normalize(Vec3{-0.4f, -1.0f, -0.3f});
    Vec3 m_sunColor{1.0f, 0.96f, 0.88f};
    float m_sunIntensity = 3.0f;
    Vec3 m_ambient{0.35f, 0.42f, 0.55f};
    float m_ambientIntensity = 0.4f;
    Vec3 m_skyZenith{0.18f, 0.36f, 0.75f};
    Vec3 m_skyHorizon{0.7f, 0.8f, 0.92f};
    float m_exposure = 1.0f;
    std::vector<LightSlot> m_lights;
    std::vector<uint32_t> m_freeLights;

    // Переиспользуемые буферы Prepare
    std::vector<InstanceRef> m_refs;
    std::vector<SkinRef> m_skinRefs;
    std::unordered_map<uint32_t, std::unique_ptr<Animator>> m_animators;  // по индексу сущности
    std::vector<Animator*> m_activeAnimators;
    std::vector<Retired> m_retired;
    uint64_t m_prepareCount = 0;
    VkPipelineLayout m_skinLayout = VK_NULL_HANDLE;
    VkPipeline m_skinPipeline = VK_NULL_HANDLE;
    std::vector<uint32_t> m_meshCounts;   // по группам (2 * число мешей)
    std::vector<uint32_t> m_meshOffsets;
    std::vector<uint8_t> m_visMask;  // по инстансам: бит 0 — камера, биты 1..4 — каскады

    std::atomic<uint32_t> m_drawCalls{0};
    std::atomic<uint32_t> m_instances{0};
};

} // namespace ce::core
