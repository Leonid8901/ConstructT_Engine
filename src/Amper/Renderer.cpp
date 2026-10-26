#include "Amper/Renderer.h"
#include "Core.h"
#include "Profile.h"
#include "Platform/Hardware.h"
#include "Platform/Window.h"
#include "UniX/GpuLanes.h"
#include "UniX/Scheduler.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"
#include "World/WorldImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ce::core {

namespace {

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

VkShaderModule LoadShader(VkDevice device, const std::string& name) {
    // Порядок: shaders/ рядом с exe (раздача игры, запуск из любой папки), shaders/ в текущей папке, каталог сборки
    const std::filesystem::path candidates[] = {ExecutableDir() / "shaders" / name, std::filesystem::path("shaders") / name,
                                                std::filesystem::path(SHADER_DIR) / name};
    std::ifstream file;
    std::filesystem::path path;
    for (const auto& c : candidates) {
        if (c.empty() || c == std::filesystem::path(name))
            continue;
        file.open(c, std::ios::binary | std::ios::ate);
        if (file) {
            path = c;
            break;
        }
        file.clear();
    }
    if (!file)
        throw std::runtime_error("Не найден шейдер " + name + ": положите папку shaders/ рядом с exe (ищу и в " +
                                 std::string(SHADER_DIR) + ")");
    size_t size = static_cast<size_t>(file.tellg());
    if (size == 0 || size % 4 != 0)
        throw std::runtime_error("Повреждён SPIR-V: " + path.string());
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, uint32_t layers = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void MemBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Material -> данные инстанса. Индексы текстур: UINT32_MAX — нет текстуры.
void WriteMaterial(GpuInstance& g, const Material& m) {
    g.colorRough = {m.color, m.roughness};
    g.emissiveMetal = {m.emissive, m.metallic};
    g.tex[0] = m.baseColorMap.Id();
    g.tex[1] = m.metallicRoughnessMap.Id();
    g.tex[2] = m.normalMap.Id();
    g.tex[3] = m.emissiveMap.Id();
    const uint32_t occ = m.occlusionMap.Id();
    float occBits;
    std::memcpy(&occBits, &occ, sizeof(occBits));
    g.params = {m.normalScale, m.alphaCutoff, m.occlusionStrength, occBits};
    g.extra = {m.uvScale.x, m.uvScale.y, m.doubleSided ? 1.0f : 0.0f, 0};
}

} // namespace

Renderer::Renderer(const VulkanContext& ctx, Gpu& gpu, GpuLanes& lanes, Window& window, const EngineConfig& config)
    : m_ctx(ctx), m_gpu(gpu), m_lanes(lanes), m_window(window), m_device(ctx.Device()),
      m_rtSupported(ctx.RayTracingSupported()) {
    m_rtEnabled.store(config.rayTracing && m_rtSupported);
    m_swapchain = std::make_unique<Swapchain>(ctx, window, config.vsync);
    m_meshes = std::make_unique<MeshRegistry>(gpu, lanes);
    m_textures = std::make_unique<TextureRegistry>(ctx, gpu, lanes);
    m_models = std::make_unique<ModelRegistry>(*m_meshes, *m_textures);

    // Форматы глубины: D32 есть почти везде на ПК; спецификация гарантирует D16 и один из X8_D24/D32
    auto supports = [&](VkFormat f, VkFormatFeatureFlags need) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(ctx.PhysicalDevice(), f, &p);
        return (p.optimalTilingFeatures & need) == need;
    };
    const VkFormat depthCandidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM};
    for (VkFormat f : depthCandidates)
        if (supports(f, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            m_depthFormat = f;
            break;
        }
    bool shadowFound = false;
    for (bool linear : {true, false}) {
        for (VkFormat f : depthCandidates) {
            VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if (linear)
                need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if (supports(f, need)) {
                m_shadowFormat = f;
                m_shadowLinear = linear;
                shadowFound = true;
                break;
            }
        }
        if (shadowFound)
            break;
    }
    m_shadowSize = std::clamp(config.shadowMapSize ? config.shadowMapSize : 2048u, 256u,
                              std::min(8192u, ctx.Properties().limits.maxImageDimension2D));
    if (m_depthFormat != VK_FORMAT_D32_SFLOAT || m_shadowFormat != VK_FORMAT_D32_SFLOAT || !m_shadowLinear)
        Log("Amper: глубина " + std::to_string(m_depthFormat) + ", тени " + std::to_string(m_shadowFormat) +
            (m_shadowLinear ? "" : " (без линейной фильтрации)"));

    // Поддерживаемые числа сэмплов: общие для лимитов устройства и обоих форматов целей (бывают «дырявые» наборы, например 1 и 4)
    VkSampleCountFlags counts = ctx.Properties().limits.framebufferColorSampleCounts &
                                ctx.Properties().limits.framebufferDepthSampleCounts;
    auto formatCounts = [&](VkFormat format, VkImageUsageFlags usage) {
        VkImageFormatProperties props{};
        if (vkGetPhysicalDeviceImageFormatProperties(ctx.PhysicalDevice(), format, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL, usage, 0, &props) != VK_SUCCESS)
            return VkSampleCountFlags(VK_SAMPLE_COUNT_1_BIT);
        return props.sampleCounts;
    };
    counts &= formatCounts(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    counts &= formatCounts(m_swapchain->Format(),
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    m_sampleMask = (counts & 0xF) | VK_SAMPLE_COUNT_1_BIT;  // 1/2/4/8
    for (uint32_t s = 8; s >= 1; s /= 2)
        if (m_sampleMask & s) {
            m_maxSamples = s;
            break;
        }
    m_requestedSamples.store(config.msaa);
    m_samples = PickSamples(config.msaa);
    m_appliedSamples.store(static_cast<uint32_t>(m_samples));
    Log("Amper MSAA: " + std::to_string(static_cast<uint32_t>(m_samples)) + "x (max " + std::to_string(m_maxSamples) + "x)");
    CreateTargets();
    CreateShadowResources();
    CreateDescriptors();
    CreateFrames();
    CreateRenderFinished();
    CreatePipelines();
    CreateSkinPipeline();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(m_device);
    for (auto& [index, a] : m_animators)
        for (SkinnedPart& sp : a->skinned)
            DestroySkinnedPart(sp);
    for (Retired& r : m_retired)
        for (SkinnedPart& sp : r.parts)
            DestroySkinnedPart(sp);
    vkDestroyPipeline(m_device, m_skinPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_skinLayout, nullptr);
    for (Frame& f : m_frames) {
        m_gpu.Destroy(f.joints);
        vkDestroyCommandPool(m_device, f.pool, nullptr);
        vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
        vkDestroyFence(m_device, f.fence, nullptr);
        m_gpu.Destroy(f.ubo);
        m_gpu.Destroy(f.instances);
        m_gpu.Destroy(f.drawIndices);
        m_gpu.Destroy(f.lights);
        if (f.tlas)
            m_gpu.Rt().destroyAs(m_device, f.tlas, nullptr);
        m_gpu.Destroy(f.tlasInstances);
        m_gpu.Destroy(f.tlasBuffer);
        m_gpu.Destroy(f.scratch);
    }
    DestroyRenderFinished();
    DestroyPipelines();
    for (VkImageView v : m_shadowLayerViews)
        vkDestroyImageView(m_device, v, nullptr);
    vkDestroySampler(m_device, m_shadowSampler, nullptr);
    m_gpu.Destroy(m_shadowMap);
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_gpu.Destroy(m_depth);
    m_gpu.Destroy(m_msaaColor);
    m_models.reset();
    m_textures.reset();
    m_meshes.reset();
    m_swapchain.reset();
}

// ---------------- создание ресурсов ----------------

// Наибольшее поддерживаемое число сэмплов, не превышающее запрошенное
VkSampleCountFlagBits Renderer::PickSamples(uint32_t requested) const {
    uint32_t best = 1;
    for (uint32_t s = 2; s <= 8; s *= 2)
        if (s <= requested && (m_sampleMask & s))
            best = s;
    return static_cast<VkSampleCountFlagBits>(best);
}

// Цели основного прохода. При MSAA глубина тоже мультисэмпловая; цвет — временный (transient):
// после resolve в swapchain он не нужен, на тайловых GPU может вообще не попасть в память.
void Renderer::CreateTargets() {
    m_gpu.Destroy(m_depth);
    m_gpu.Destroy(m_msaaColor);
    const VkExtent2D extent = m_swapchain->Extent();
    m_depth = m_gpu.CreateImage2D(extent, m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT, 1, m_samples);
    if (m_samples != VK_SAMPLE_COUNT_1_BIT)
        m_msaaColor = m_gpu.CreateImage2D(extent, m_swapchain->Format(),
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 1, m_samples);
}

void Renderer::ApplySampleChange() {
    const VkSampleCountFlagBits want = PickSamples(m_requestedSamples.load());
    if (want == m_samples)
        return;
    vkDeviceWaitIdle(m_device);
    m_samples = want;
    m_appliedSamples.store(static_cast<uint32_t>(want));
    CreateTargets();
    DestroyPipelines();
    CreatePipelines();
    Log("Amper MSAA: " + std::to_string(static_cast<uint32_t>(want)) + "x");
}

void Renderer::CreateShadowResources() {
    m_shadowMap = m_gpu.CreateImage2D({m_shadowSize, m_shadowSize}, m_shadowFormat,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT, kCascades);
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_shadowMap.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_shadowFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        VK_CHECK(vkCreateImageView(m_device, &vi, nullptr, &m_shadowLayerViews[c]));
    }

    // Аппаратное сравнение глубины + билинейная фильтрация = бесплатный 2x2 PCF на каждый тап
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = m_shadowLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = si.magFilter;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // за краем карты — освещено
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(m_device, &si, nullptr, &m_shadowSampler));

    // Дескриптор статически использует карту, поэтому она всегда должна быть в читаемом layout
    m_lanes.Immediate(GpuLane::Graphics, [&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
    });
}

void Renderer::CreateDescriptors() {
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    bindings.push_back({4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    bindings.push_back({5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});  // drawIndices
    if (m_rtSupported)
        bindings.push_back({3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout));

    std::vector<VkDescriptorPoolSize> sizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
    };
    if (m_rtSupported)
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight});
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kFramesInFlight;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool));
}

void Renderer::CreateFrames() {
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = m_lanes.Family(GpuLane::Graphics);
        VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &f.cmd));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &f.imageAvailable));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(m_device, &fi, nullptr, &f.fence));

        f.ubo = m_gpu.CreateBuffer(sizeof(FrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemUsage::Stream);
        f.lights = m_gpu.CreateBuffer(sizeof(GpuLight) * kMaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Stream);

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = m_descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &m_setLayout;
        VK_CHECK(vkAllocateDescriptorSets(m_device, &dai, &f.set));

        EnsureInstanceCapacity(f, 1024);
        EnsureDrawIndexCapacity(f, 1024 * (1 + kCascades));
        WriteDescriptor(f, 0);
        WriteDescriptor(f, 2);
        WriteDescriptor(f, 4);
        if (m_rtSupported)
            EnsureTlasCapacity(f, 64);
    }
}

void Renderer::WriteDescriptor(Frame& f, uint32_t binding) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    VkDescriptorBufferInfo bi{};
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorImageInfo ii{m_shadowSampler, m_shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    switch (binding) {
    case 0: bi = {f.ubo.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi; break;
    case 1: bi = {f.instances.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; break;
    case 2: bi = {f.lights.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; break;
    case 5: bi = {f.drawIndices.buffer, 0, VK_WHOLE_SIZE}; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; break;
    case 3:
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &f.tlas;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w.pNext = &as;
        break;
    case 4:
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        break;
    }
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void Renderer::EnsureInstanceCapacity(Frame& f, uint32_t count) {
    if (count <= f.instanceCapacity)
        return;
    uint32_t cap = std::max(f.instanceCapacity, 1024u);
    while (cap < count)
        cap *= 2;
    m_gpu.Destroy(f.instances);
    f.instances = m_gpu.CreateBuffer(sizeof(GpuInstance) * cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Stream);
    f.instanceCapacity = cap;
    WriteDescriptor(f, 1);
}

void Renderer::EnsureDrawIndexCapacity(Frame& f, uint32_t count) {
    if (count <= f.drawIndexCapacity)
        return;
    uint32_t cap = std::max(f.drawIndexCapacity, 1024u);
    while (cap < count)
        cap *= 2;
    m_gpu.Destroy(f.drawIndices);
    f.drawIndices = m_gpu.CreateBuffer(sizeof(uint32_t) * cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Stream);
    f.drawIndexCapacity = cap;
    WriteDescriptor(f, 5);
}

void Renderer::EnsureTlasCapacity(Frame& f, uint32_t count) {
    if (count <= f.tlasCapacity && f.tlas)
        return;
    uint32_t cap = std::max(f.tlasCapacity, 64u);
    while (cap < count)
        cap *= 2;
    const RtFunctions& rt = m_gpu.Rt();
    if (f.tlas)
        rt.destroyAs(m_device, f.tlas, nullptr);
    m_gpu.Destroy(f.tlasInstances);
    m_gpu.Destroy(f.tlasBuffer);
    m_gpu.Destroy(f.scratch);

    f.tlasInstances = m_gpu.CreateBuffer(sizeof(VkAccelerationStructureInstanceKHR) * cap,
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         MemUsage::Stream, false, 16);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.data.deviceAddress = f.tlasInstances.address;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.buildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &cap, &sizes);

    f.tlasBuffer = m_gpu.CreateBuffer(sizes.accelerationStructureSize,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      MemUsage::GpuOnly);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = f.tlasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(rt.createAs(m_device, &ci, nullptr, &f.tlas));
    f.scratch = m_gpu.CreateBuffer(sizes.buildScratchSize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   MemUsage::GpuOnly, false, m_gpu.ScratchAlignment());
    f.tlasCapacity = cap;
    WriteDescriptor(f, 3);
}

void Renderer::CreatePipelines() {
    m_colorFormat = m_swapchain->Format();

    // set 0 — данные кадра (по набору на кадр в полёте), set 1 — общий bindless-массив текстур
    const VkDescriptorSetLayout sets[2] = {m_setLayout, m_textures->Layout()};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = sets;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t)};  // индекс каскада
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(m_device, &li, nullptr, &m_pipelineLayout));

    auto build = [&](const char* vs, const char* fs, bool mesh, bool shadow = false, bool twoSided = false) {
        VkShaderModule vert = LoadShader(m_device, vs);
        VkShaderModule frag = fs ? LoadShader(m_device, fs) : VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

        VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[4] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (mesh) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = shadow ? 1 : 4;
            vi.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = mesh && !twoSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        if (shadow) {
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.depthBiasEnable = VK_TRUE;  // против «акне»; основное смещение — normal offset в шейдере
            rs.depthBiasConstantFactor = 1.0f;
            rs.depthBiasSlopeFactor = 1.75f;
        }
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = shadow ? VK_SAMPLE_COUNT_1_BIT : m_samples;  // карты теней без MSAA
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = mesh ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = mesh ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = shadow ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER;  // кадр — reverse-Z
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = shadow ? 0 : 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = shadow ? 0 : 1;
        ri.pColorAttachmentFormats = shadow ? nullptr : &m_colorFormat;
        ri.depthAttachmentFormat = shadow ? m_shadowFormat : m_depthFormat;

        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &ri;
        pi.stageCount = frag ? 2 : 1;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = m_pipelineLayout;
        VkPipeline p;
        VkResult r = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
        vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag)
            vkDestroyShaderModule(m_device, frag, nullptr);
        VK_CHECK(r);
        return p;
    };

    m_skyPipeline = build("sky.vert.spv", "sky.frag.spv", false);
    m_meshPipeline = build("mesh.vert.spv", "mesh.frag.spv", true);
    m_meshPipeline2s = build("mesh.vert.spv", "mesh.frag.spv", true, false, true);
    m_shadowPipeline = build("shadow.vert.spv", nullptr, true, true);
    if (m_rtSupported) {
        m_meshRtPipeline = build("mesh.vert.spv", "mesh_rt.frag.spv", true);
        m_meshRtPipeline2s = build("mesh.vert.spv", "mesh_rt.frag.spv", true, false, true);
    }
}

void Renderer::DestroyPipelines() {
    for (VkPipeline p : {m_skyPipeline, m_meshPipeline, m_meshPipeline2s, m_shadowPipeline, m_meshRtPipeline,
                         m_meshRtPipeline2s})
        if (p)
            vkDestroyPipeline(m_device, p, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_skyPipeline = m_meshPipeline = m_meshPipeline2s = m_shadowPipeline = m_meshRtPipeline = m_meshRtPipeline2s =
        VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
}

void Renderer::CreateRenderFinished() {
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_renderFinished.resize(m_swapchain->ImageCount());
    for (auto& s : m_renderFinished)
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &s));
}

void Renderer::DestroyRenderFinished() {
    for (VkSemaphore s : m_renderFinished)
        vkDestroySemaphore(m_device, s, nullptr);
    m_renderFinished.clear();
}

void Renderer::HandleResize() {
    m_swapchain->Recreate();  // внутри vkDeviceWaitIdle
    DestroyRenderFinished();
    CreateRenderFinished();
    CreateTargets();
    if (m_swapchain->Format() != m_colorFormat) {
        DestroyPipelines();
        CreatePipelines();
    }
}

// ---------------- скелетная анимация ----------------

void Renderer::CreateSkinPipeline() {
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinJob)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(m_device, &li, nullptr, &m_skinLayout));
    VkShaderModule cs = LoadShader(m_device, "skin.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = m_skinLayout;
    const VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_skinPipeline);
    vkDestroyShaderModule(m_device, cs, nullptr);
    VK_CHECK(r);
}

void Renderer::EnsureJointCapacity(Frame& f, uint32_t count) {
    if (count <= f.jointCapacity && f.joints)
        return;
    uint32_t cap = std::max(f.jointCapacity, 256u);
    while (cap < count)
        cap *= 2;
    m_gpu.Destroy(f.joints);
    f.joints = m_gpu.CreateBuffer(sizeof(Mat4) * cap,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  MemUsage::Stream);
    f.jointCapacity = cap;
}

// Скиннутые вершины сущности + BLAS с ALLOW_UPDATE (каждый кадр обновляется, а не строится заново)
void Renderer::CreateSkinnedPart(SkinnedPart& sp, const MeshGpu& mesh) {
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (m_rtSupported)
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    sp.out = m_gpu.CreateBuffer(VkDeviceSize(mesh.vertexCount) * sizeof(Vertex), usage, MemUsage::GpuOnly);
    if (!m_rtSupported)
        return;

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& tri = geom.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexStride = sizeof(Vertex);
    tri.maxVertex = mesh.vertexCount - 1;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    const uint32_t primitives = mesh.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_gpu.Rt().buildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);

    sp.blasBuffer = m_gpu.CreateBuffer(sizes.accelerationStructureSize,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       MemUsage::GpuOnly);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = sp.blasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(m_gpu.Rt().createAs(m_device, &ci, nullptr, &sp.blas));
    sp.scratch = m_gpu.CreateBuffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    MemUsage::GpuOnly, false, m_gpu.ScratchAlignment());
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = sp.blas;
    sp.blasAddress = m_gpu.Rt().asAddress(m_device, &ai);
}

void Renderer::DestroySkinnedPart(SkinnedPart& sp) {
    if (sp.blas)
        m_gpu.Rt().destroyAs(m_device, sp.blas, nullptr);
    m_gpu.Destroy(sp.blasBuffer);
    m_gpu.Destroy(sp.scratch);
    m_gpu.Destroy(sp.out);
    sp = {};
}

// Аниматор сущности: создаётся при первой встрече, пересоздаётся при смене модели/поколения.
// Команды Play/Stop доходят через animSerial.
Renderer::Animator* Renderer::SyncAnimator(uint32_t entity, const EntityData& d, const ModelData& md) {
    auto& slot = m_animators[entity];
    if (slot && (slot->gen != d.gen || slot->model != d.model)) {
        m_retired.push_back({std::move(slot->skinned), m_prepareCount});
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<Animator>();
        Animator& a = *slot;
        a.entity = entity;
        a.gen = d.gen;
        a.model = d.model;
        a.skinBase.assign(md.skins.size(), 0);
        for (uint32_t p = 0; p < md.parts.size(); ++p)
            if (md.parts[p].skin >= 0)
                if (const MeshGpu* mesh = m_meshes->Get(md.parts[p].mesh); mesh && mesh->skin) {
                    SkinnedPart sp;
                    sp.part = p;
                    sp.mesh = md.parts[p].mesh;
                    CreateSkinnedPart(sp, *mesh);
                    a.skinned.push_back(sp);
                }
        RestPose(md, a.pose);
        a.globals.clear();
    }
    Animator& a = *slot;
    a.seen = m_prepareCount;
    a.speed = d.animSpeed;
    if (a.serial != d.animSerial) {  // новая команда: переход от того, что играло, к новому клипу
        a.serial = d.animSerial;
        a.prevClip = a.clip;
        a.prevTime = a.time;
        a.prevLoop = a.loop;
        a.fadeDur = a.globals.empty() ? 0.0f : d.animFade;  // первый кадр — без перехода
        a.fade = 0;
        a.clip = d.animClip < md.clips.size() ? static_cast<int32_t>(d.animClip) : -1;
        a.time = a.speed < 0 && a.clip >= 0 ? md.clips[a.clip].duration : 0.0f;
        a.loop = d.animLoop;
        a.playing = a.clip >= 0;
    }
    return &a;
}

namespace {
float Advance(float t, float dt, float duration, bool loop, bool& playing) {
    if (duration <= 0) {
        playing = false;
        return 0;
    }
    t += dt;
    if (loop) {
        t = std::fmod(t, duration);
        return t < 0 ? t + duration : t;
    }
    if (t >= duration) {  // однократная: стоп на последнем кадре (или первом при обратной скорости)
        t = duration;
        if (dt > 0) playing = false;
    } else if (t <= 0) {
        t = 0;
        if (dt < 0) playing = false;
    }
    return t;
}
} // namespace

void Renderer::Animate(Animator& a, float dt) {
    const ModelData& md = *m_models->Get(a.model);
    const float adt = dt * a.speed;
    if (a.clip >= 0)
        a.time = Advance(a.time, adt, md.clips[a.clip].duration, a.loop, a.playing);
    RestPose(md, a.pose);
    if (a.clip >= 0)
        SampleClip(md.clips[a.clip], a.time, a.pose);

    if (a.fade < a.fadeDur) {  // переход: старая поза -> новая
        a.fade += dt;
        bool prevPlaying = true;
        if (a.prevClip >= 0)
            a.prevTime = Advance(a.prevTime, adt, md.clips[a.prevClip].duration, a.prevLoop, prevPlaying);
        RestPose(md, a.prevPose);
        if (a.prevClip >= 0)
            SampleClip(md.clips[a.prevClip], a.prevTime, a.prevPose);
        BlendPose(a.prevPose, a.pose, a.fade / a.fadeDur);
        std::swap(a.pose, a.prevPose);
    }
    ComputeGlobals(md, a.pose, a.globals);
}

// Скиннинг и BLAS скиннутых частей: до сборки TLAS
void Renderer::RecordSkinning(Frame& f) {
    if (f.skinJobs.empty())
        return;
    VkCommandBuffer cmd = f.cmd;
    // Прошлый кадр мог ещё читать эти вершины (вершинный вход, сборка BLAS): ждём, потом пишем
    MemBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | (m_rtSupported ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0),
               VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipeline);
    for (const SkinJob& job : f.skinJobs) {
        vkCmdPushConstants(cmd, m_skinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinJob), &job);
        vkCmdDispatch(cmd, (job.count + 63) / 64, 1, 1);
    }
    VkPipelineStageFlags2 dst = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    VkAccessFlags2 dstAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    if (f.rt && !f.skinBlas.empty()) {
        dst |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        dstAccess |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                     VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    }
    // + прошлый кадр читал BLAS лучами во фрагментном шейдере: обновлять можно только после
    MemBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | (f.rt ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0),
               VK_ACCESS_2_SHADER_WRITE_BIT, dst, dstAccess);
    if (!f.rt || f.skinBlas.empty())
        return;

    for (SkinnedPart* sp : f.skinBlas) {
        const MeshGpu* mesh = m_meshes->Get(sp->mesh);
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geom.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = sp->out.address;
        tri.vertexStride = sizeof(Vertex);
        tri.maxVertex = mesh->vertexCount - 1;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = mesh->indices.address;
        VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        build.mode = sp->built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.srcAccelerationStructure = sp->built ? sp->blas : VK_NULL_HANDLE;
        build.dstAccelerationStructure = sp->blas;
        build.geometryCount = 1;
        build.pGeometries = &geom;
        build.scratchData.deviceAddress = sp->scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = mesh->indexCount / 3;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        m_gpu.Rt().cmdBuild(cmd, 1, &build, &ranges);
        sp->built = true;
    }
    // BLAS готовы -> TLAS их читает
    MemBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
               VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

// ---------------- освещение ----------------

void Renderer::SetSun(Vec3 dir, Vec3 color, float intensity) {
    std::lock_guard lk(m_settingsMutex);
    m_sunDir = Normalize(dir);
    m_sunColor = color;
    m_sunIntensity = intensity;
}
void Renderer::SetAmbient(Vec3 color, float intensity) {
    std::lock_guard lk(m_settingsMutex);
    m_ambient = color;
    m_ambientIntensity = intensity;
}
void Renderer::SetSky(Vec3 zenith, Vec3 horizon) {
    std::lock_guard lk(m_settingsMutex);
    m_skyZenith = zenith;
    m_skyHorizon = horizon;
}
void Renderer::SetShadowDistance(float d) {
    std::lock_guard lk(m_settingsMutex);
    m_shadowDistance = std::clamp(d, 5.0f, 1000.0f);
}

// Каскады: «практическое» разбиение (смесь логарифмического и линейного), на каждый каскад —
// ограничивающая сфера среза фрустума (размер не зависит от поворота камеры) и привязка к сетке
// текселей (тени не «мерцают» при движении).
void Renderer::ComputeCascades(FrameUbo& ubo, ShadowCull& cull, Vec3 camPos, Vec3 fwd, Vec3 right, Vec3 up,
                               float tanHalf, float aspect, Vec3 sunDir, float distance) {
    constexpr float kNear = 0.05f, kLambda = 0.75f;
    float splits[kCascades + 1];
    splits[0] = kNear;
    for (uint32_t i = 1; i <= kCascades; ++i) {
        float p = static_cast<float>(i) / kCascades;
        splits[i] = kLambda * kNear * std::pow(distance / kNear, p) + (1 - kLambda) * (kNear + (distance - kNear) * p);
    }

    Vec3 L = Normalize(sunDir);
    Vec3 upL = std::fabs(L.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Mat4 lightRot = Mat4::LookAt({0, 0, 0}, L, upL);
    cull.lightView = lightRot;

    for (uint32_t c = 0; c < kCascades; ++c) {
        Vec3 corners[8];
        int k = 0;
        for (float d : {splits[c], splits[c + 1]}) {
            Vec3 cc = camPos + fwd * d;
            float hh = d * tanHalf, hw = hh * aspect;
            for (float sx : {-1.0f, 1.0f})
                for (float sy : {-1.0f, 1.0f})
                    corners[k++] = cc + right * (sx * hw) + up * (sy * hh);
        }
        Vec3 center{};
        for (Vec3 p : corners)
            center += p;
        center = center / 8.0f;
        float r = 0;
        for (Vec3 p : corners)
            r = std::max(r, Length(p - center));
        r = std::ceil(r * 16.0f) / 16.0f;

        float texel = 2.0f * r / static_cast<float>(m_shadowSize);
        Vec3 cl = lightRot.TransformPoint(center);
        cl.x = std::floor(cl.x / texel) * texel;
        cl.y = std::floor(cl.y / texel) * texel;
        // Ближняя плоскость может отсечь далёкие тенеотбрасыватели — их спасает «пэнкейкинг» в shadow.vert
        Mat4 ortho = Mat4::Ortho(cl.x - r, cl.x + r, cl.y - r, cl.y + r, -cl.z - r, -cl.z + r);
        ubo.cascadeViewProj[c] = ortho * lightRot;
        ubo.cascadeSplits[c] = splits[c + 1];
        ubo.cascadeTexel[c] = texel;
        // Для отсечения: запас в 4 текселя покрывает normal offset и PCF на краю каскада
        cull.box[c] = {cl.x, cl.y, r + 4.0f * texel, -cl.z + r};
    }
}

void Renderer::RecordShadowPass(Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                 kCascades);

    VkViewport viewport{0, 0, static_cast<float>(m_shadowSize), static_cast<float>(m_shadowSize), 0, 1};
    VkRect2D scissor{{0, 0}, {m_shadowSize, m_shadowSize}};
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_shadowLayerViews[c];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &c);
        for (const Draw& d : f.shadowDraws[c]) {  // только то, что попадает в объём каскада
            const MeshGpu* mesh = m_meshes->Get(d.mesh);
            VkDeviceSize offset = 0;
            const VkBuffer vb = d.vertices ? d.vertices : mesh->vertices.buffer;  // скиннутые — свои вершины
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, d.count, 0, 0, d.first);
        }
        vkCmdEndRendering(cmd);
    }

    ImageBarrier(cmd, m_shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
}

void Renderer::SetExposure(float e) {
    std::lock_guard lk(m_settingsMutex);
    m_exposure = e;
}

Light Renderer::AddLight(const GpuLight& l) {
    std::lock_guard lk(m_settingsMutex);
    uint32_t idx;
    if (!m_freeLights.empty()) {
        idx = m_freeLights.back();
        m_freeLights.pop_back();
    } else {
        idx = static_cast<uint32_t>(m_lights.size());
        m_lights.emplace_back();
    }
    LightSlot& s = m_lights[idx];
    s.data = l;
    s.alive = true;
    return Access::MakeLight(idx, s.gen);
}

bool Renderer::LightValid(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_settingsMutex);
    return index < m_lights.size() && m_lights[index].alive && m_lights[index].gen == gen;
}

void Renderer::RemoveLight(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_settingsMutex);
    if (index < m_lights.size() && m_lights[index].alive && m_lights[index].gen == gen) {
        m_lights[index].alive = false;
        ++m_lights[index].gen;
        m_freeLights.push_back(index);
    }
}

uint32_t Renderer::LightCount() {
    std::lock_guard lk(m_settingsMutex);
    return static_cast<uint32_t>(m_lights.size() - m_freeLights.size());
}

// ---------------- Prepare (воркеры) ----------------

void Renderer::Prepare(Task& task) {
    CE_ZONE("Amper.Prepare");
    Frame& f = m_frames[m_frameIndex];
    int64_t w0 = NowNs();
    VK_CHECK(vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    Core().scheduler->NoteWait(NowNs() - w0);

    WorldImpl& world = *Core().world;
    const uint32_t cap = world.Capacity();
    const uint32_t meshCount = m_meshes->Count();

    // 1. Список инстансов (сущность с мешем — один, с моделью — по одному на часть) и ранг внутри
    //    группы отрисовки. Группа = меш + односторонний/двусторонний: один draw на группу.
    //    Последовательно и дёшево; тяжёлая работа — в ParallelFor ниже.
    ++m_prepareCount;
    // Ресурсы удалённых аниматоров: кадр, который мог их читать, уже прошёл свой fence
    std::erase_if(m_retired, [&](Retired& r) {
        if (r.frame + kFramesInFlight > m_prepareCount)
            return false;
        for (SkinnedPart& sp : r.parts)
            DestroySkinnedPart(sp);
        return true;
    });

    const uint32_t meshGroups = meshCount * 2;
    m_meshCounts.assign(meshGroups, 0);
    m_refs.clear();
    m_skinRefs.clear();
    m_activeAnimators.clear();
    auto add = [&](uint32_t entity, uint32_t part, uint32_t mesh, bool twoSided, const Animator* anim) {
        if (mesh >= meshCount || !m_meshes->Get(mesh))
            return;
        const uint32_t key = (twoSided ? meshCount : 0) + mesh;
        m_refs.push_back({entity, part, key, m_meshCounts[key]++, anim});
    };
    for (uint32_t i = 0; i < cap; ++i) {
        const EntityData& d = world.At(i);
        if (!d.alive)
            continue;
        const ModelData* md = d.model != UINT32_MAX ? m_models->Get(d.model) : nullptr;
        // Аниматор живёт и у скрытой сущности: состояние анимации не сбрасывается от SetVisible(false)
        Animator* anim = md && md->animated() ? SyncAnimator(i, d, *md) : nullptr;
        if (anim)
            m_activeAnimators.push_back(anim);
        if (!d.visible)
            continue;
        if (md) {
            for (uint32_t p = 0; p < md->parts.size(); ++p) {
                if (anim && md->parts[p].skin >= 0) {
                    // Часть со скином: свои вершины -> своя группа отрисовки из одного инстанса
                    for (SkinnedPart& sp : anim->skinned)
                        if (sp.part == p)
                            m_skinRefs.push_back({i, p, &sp});
                    continue;
                }
                add(i, p, md->parts[p].mesh, md->parts[p].material.doubleSided, anim);
            }
        } else if (d.mesh != UINT32_MAX) {
            add(i, UINT32_MAX, d.mesh, d.material.doubleSided, nullptr);
        }
    }
    // Аниматоры исчезнувших сущностей (или сменивших модель на неанимированную)
    for (auto it = m_animators.begin(); it != m_animators.end();) {
        if (it->second->seen != m_prepareCount) {
            m_retired.push_back({std::move(it->second->skinned), m_prepareCount});
            it = m_animators.erase(it);
        } else {
            ++it;
        }
    }
    // Группы скиннутых частей идут после групп мешей
    const uint32_t groups = meshGroups + static_cast<uint32_t>(m_skinRefs.size());
    m_meshCounts.resize(groups, 0);
    for (uint32_t k = 0; k < m_skinRefs.size(); ++k) {
        m_refs.push_back({m_skinRefs[k].entity, m_skinRefs[k].part, meshGroups + k, 0,
                          m_animators[m_skinRefs[k].entity].get()});
        m_meshCounts[meshGroups + k] = 1;
    }

    // Анимации: поза -> матрицы узлов (параллельно по аниматорам), обратная связь в мир
    {
        const float dt = Core().dt.load(std::memory_order_relaxed);
        const uint32_t animCount = static_cast<uint32_t>(m_activeAnimators.size());
        task.ParallelFor(animCount, [&](uint32_t begin, uint32_t end) {
            for (uint32_t k = begin; k < end; ++k) {
                Animator& a = *m_activeAnimators[k];
                Animate(a, dt);
                EntityData& d = world.At(a.entity);  // пишем только поля обратной связи; читают их в Update
                d.animTime = a.time;
                d.animPlaying = a.playing;
            }
        });
    }
    // Палитры костей: по скину каждого аниматора — joint = globals[кость] * inverseBind
    f.skinJobs.clear();
    f.skinBlas.clear();
    {
        uint32_t joints = 0;
        for (Animator* a : m_activeAnimators) {
            const ModelData& md = *m_models->Get(a->model);
            for (size_t s = 0; s < md.skins.size(); ++s) {
                a->skinBase[s] = joints;
                joints += static_cast<uint32_t>(md.skins[s].joints.size());
            }
        }
        if (joints > 0) {
            EnsureJointCapacity(f, joints);
            auto* palette = static_cast<Mat4*>(f.joints.mapped);
            for (Animator* a : m_activeAnimators) {
                const ModelData& md = *m_models->Get(a->model);
                for (size_t s = 0; s < md.skins.size(); ++s) {
                    const ModelSkin& skin = md.skins[s];
                    for (size_t j = 0; j < skin.joints.size(); ++j) {
                        const Mat4 m = a->globals[skin.joints[j]] * skin.inverseBind[j];
                        std::memcpy(&palette[a->skinBase[s] + j], &m, sizeof(Mat4));  // write-combined: только запись
                    }
                }
            }
        }
        for (const SkinRef& sr : m_skinRefs) {
            const Animator& a = *m_animators[sr.entity];
            const ModelData& md = *m_models->Get(a.model);
            const MeshGpu* mesh = m_meshes->Get(sr.sp->mesh);
            SkinJob job{};
            job.src = mesh->vertices.address;
            job.skin = mesh->skin.address;
            job.joints = f.joints.address + VkDeviceSize(a.skinBase[md.parts[sr.part].skin]) * sizeof(Mat4);
            job.dst = sr.sp->out.address;
            job.count = mesh->vertexCount;
            f.skinJobs.push_back(job);
        }
    }
    m_meshOffsets.resize(groups);
    uint32_t total = 0;
    for (uint32_t g = 0; g < groups; ++g) {
        m_meshOffsets[g] = total;
        total += m_meshCounts[g];
    }

    f.rt = m_rtEnabled.load() && m_rtSupported;
    f.shadows = !f.rt && m_shadowsEnabled.load();  // при RT тени даёт ray query, карты не нужны
    EnsureInstanceCapacity(f, std::max(total, 1u));
    EnsureDrawIndexCapacity(f, std::max(total, 1u) * (1 + kCascades));
    if (f.rt)
        EnsureTlasCapacity(f, std::max(total, 1u));
    f.tlasCount = f.rt ? total : 0;
    if (f.rt)
        for (const SkinRef& sr : m_skinRefs)
            f.skinBlas.push_back(sr.sp);

    // 2. Камера, свет, каскады — до инстансов, потому что по ним идёт отсечение
    Vec3 camPos;
    float yaw, pitch, fov;
    {
        CameraState& cam = *Core().camera;
        std::lock_guard lk(cam.mutex);
        camPos = cam.pos;
        yaw = cam.yaw;
        pitch = cam.pitch;
        fov = Radians(cam.fovDeg);
    }
    Vec3 fwd{std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
    Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
    Vec3 up = Cross(right, fwd);
    VkExtent2D ext = m_swapchain->Extent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(ext.height, 1u));
    const float tanV = std::tan(fov * 0.5f);
    const float tanH = tanV * aspect;

    FrameUbo ubo{};
    ShadowCull cull{};
    ubo.viewProj = Mat4::PerspectiveReverseZ(fov, aspect, 0.05f) * Mat4::LookAt(camPos, camPos + fwd, {0, 1, 0});
    ubo.camPos = {camPos, static_cast<float>(Core().time.load(std::memory_order_relaxed))};
    ubo.camForward = {fwd, tanV};
    ubo.camRight = {right, aspect};
    ubo.camUp = {up, 0};
    {
        std::lock_guard lk(m_settingsMutex);
        ubo.sunDir = {m_sunDir, m_sunIntensity};
        ubo.sunColor = {m_sunColor, 0};
        ubo.ambient = {m_ambient * m_ambientIntensity, m_exposure};
        ubo.skyZenith = {m_skyZenith, 0};
        ubo.skyHorizon = {m_skyHorizon, 0};
        if (f.shadows)
            ComputeCascades(ubo, cull, camPos, fwd, right, up, tanV, aspect, m_sunDir, m_shadowDistance);
        ubo.shadowParams = {f.shadows ? 1.0f : 0.0f, m_swapchain->ManualSrgb() ? 1.0f : 0.0f, 0, 0};
        auto* lights = static_cast<GpuLight*>(f.lights.mapped);
        uint32_t n = 0;
        for (const LightSlot& s : m_lights)
            if (s.alive && n < kMaxLights)
                lights[n++] = s.data;
        ubo.lightCount = n;
    }
    ubo.rtEnabled = f.rt ? 1 : 0;
    std::memcpy(f.ubo.mapped, &ubo, sizeof(ubo));

    // Плоскости фрустума камеры (нормали внутрь, дальней нет — reverse-Z до бесконечности)
    auto plane = [&](Vec3 n) {
        n = Normalize(n);
        return Vec4{n, -Dot(n, camPos)};
    };
    const std::array<Vec4, 5> planes = {
        Vec4{fwd, -Dot(fwd, camPos) - 0.05f},
        plane(fwd * tanH - right), plane(fwd * tanH + right),
        plane(fwd * tanV - up), plane(fwd * tanV + up),
    };
    const bool shadows = f.shadows;
    m_visMask.resize(std::max(total, 1u));

    // 3. Матрицы, TLAS-инстансы и отсечение (параллельно).
    // Буферы лежат в видеопамяти с доступом CPU (write-combined): структуру собираем на стеке и
    // копируем целиком, чтобы не читать из этой памяти (битовые поля TLAS-инстанса читают слово).
    auto* instances = static_cast<GpuInstance*>(f.instances.mapped);
    auto* tlas = f.rt ? static_cast<VkAccelerationStructureInstanceKHR*>(f.tlasInstances.mapped) : nullptr;
    const uint32_t refCount = static_cast<uint32_t>(m_refs.size());
    task.ParallelFor(refCount, [&](uint32_t begin, uint32_t end) {
        for (uint32_t ri = begin; ri < end; ++ri) {
            const InstanceRef& ref = m_refs[ri];
            const EntityData& d = world.At(ref.entity);
            const ModelPart* part =
                ref.part == UINT32_MAX ? nullptr : &m_models->Get(d.model)->parts[ref.part];
            const Material& mat = part ? part->material : d.material;
            const MeshGpu* mesh = m_meshes->Get(part ? part->mesh : d.mesh);
            const uint32_t idx = m_meshOffsets[ref.key] + ref.slot;
            const bool skinned = ref.key >= meshGroups;
            const SkinnedPart* sp = skinned ? m_skinRefs[ref.key - meshGroups].sp : nullptr;
            const Mat4 entityM = Mat4::TRS(d.pos, d.rot, d.scale);
            // Скиннутые вершины уже в координатах модели; у анимированной модели узлы берут позу аниматора
            const Mat4 model = !part || skinned ? entityM
                               : ref.anim && !ref.anim->globals.empty() ? entityM * ref.anim->globals[part->node]
                                                                        : entityM * part->local;

            GpuInstance g;
            std::memcpy(g.model, model.m, sizeof(g.model));
            WriteMaterial(g, mat);
            std::memcpy(&instances[idx], &g, sizeof(g));

            if (tlas) {
                VkAccelerationStructureInstanceKHR ti{};
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        ti.transform.matrix[r][c] = model.m[c * 4 + r];
                ti.instanceCustomIndex = 0;
                ti.mask = 0xFF;
                ti.instanceShaderBindingTableRecordOffset = 0;
                ti.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                ti.accelerationStructureReference = sp ? sp->blasAddress : mesh->blasAddress;
                std::memcpy(&tlas[idx], &ti, sizeof(ti));
            }

            // Ограничивающая сфера в мире (MaxScale учитывает и масштаб узла модели)
            const Vec3 c = model.TransformPoint(mesh->boundCenter);
            const float r = mesh->boundRadius * model.MaxScale();

            uint8_t mask = 1;
            if (skinned) {  // сфера меша — для позы по умолчанию, анимация из неё выходит: не отсекаем
                m_visMask[idx] = 0x1F;
                continue;
            }
            for (const Vec4& p : planes)
                if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) {
                    mask = 0;
                    break;
                }
            if (shadows) {
                // В каскад попадает всё, что пересекает его прямоугольник по X/Y и не лежит целиком
                // дальше его дальней границы. Ближнюю не проверяем: ближе неё работает «пэнкейкинг».
                const Vec3 lp = cull.lightView.TransformPoint(c);
                for (uint32_t k = 0; k < kCascades; ++k) {
                    const Vec4& b = cull.box[k];
                    const float lim = b.z + r;
                    if (std::fabs(lp.x - b.x) <= lim && std::fabs(lp.y - b.y) <= lim && -lp.z - r <= b.w)
                        mask |= static_cast<uint8_t>(1u << (k + 1));
                }
            }
            m_visMask[idx] = mask;
        }
    });

    // 4. Компактные списки индексов: по мешу — непрерывный диапазон, один draw на меш и проход
    auto* out = static_cast<uint32_t*>(f.drawIndices.mapped);
    uint32_t written = 0;
    auto build = [&](uint8_t bit, std::vector<Draw>& draws) {
        draws.clear();
        uint32_t drawn = 0;
        for (uint32_t g = 0; g < groups; ++g) {  // сначала все односторонние группы, потом двусторонние
            const uint32_t count = m_meshCounts[g];
            if (count == 0)
                continue;
            const uint32_t base = m_meshOffsets[g];
            const uint32_t first = written;
            for (uint32_t k = 0; k < count; ++k)
                if (m_visMask[base + k] & bit)
                    out[written++] = base + k;
            if (written > first) {
                if (g < meshGroups) {
                    draws.push_back({g % meshCount, first, written - first, g >= meshCount, VK_NULL_HANDLE});
                } else {
                    const SkinRef& sr = m_skinRefs[g - meshGroups];
                    const bool twoSided = m_models->Get(world.At(sr.entity).model)->parts[sr.part].material.doubleSided;
                    draws.push_back({sr.sp->mesh, first, written - first, twoSided, sr.sp->out.buffer});
                }
                drawn += written - first;
            }
        }
        return drawn;
    };
    const uint32_t visible = build(1, f.draws);
    for (uint32_t c = 0; c < kCascades; ++c) {
        if (shadows)
            build(static_cast<uint8_t>(1u << (c + 1)), f.shadowDraws[c]);
        else
            f.shadowDraws[c].clear();
    }

    m_drawCalls.store(static_cast<uint32_t>(f.draws.size()));
    m_instances.store(visible);
}

// ---------------- Render (главный поток) ----------------

void Renderer::Record(Frame& f, uint32_t imageIndex) {
    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // Ресурсы, загруженные через transfer/compute лейны, делаем видимыми графической очереди
    // Загрузки мешей (копирование, сборка BLAS) делаем видимыми каждый кадр. Флаг «были новые меши»
    // не годится: меш, ставший готовым между Record и следующим Prepare, рисовался без барьера.
    // Если очередь у загрузок своя, порядок уже дал fence на CPU, а барьер даёт видимость памяти;
    // если очередь общая (llvmpipe, часть iGPU), барьер ещё и упорядочивает исполнение.
    MemBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | (m_rtSupported ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0),
               VK_ACCESS_2_TRANSFER_WRITE_BIT | (m_rtSupported ? VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR : 0),
               VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                   (m_rtSupported ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0),
               VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                   (m_rtSupported ? VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR : 0));

    RecordSkinning(f);  // скиннутые вершины и их BLAS — до TLAS и отрисовки

    if (f.rt) {
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geom.geometry.instances.data.deviceAddress = f.tlasInstances.address;
        VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geom;
        build.dstAccelerationStructure = f.tlas;
        build.scratchData.deviceAddress = f.scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = f.tlasCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        m_gpu.Rt().cmdBuild(cmd, 1, &build, &ranges);
        MemBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }

    const VkDescriptorSet sets[2] = {f.set, m_textures->Set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, sets, 0, nullptr);
    if (f.shadows)
        RecordShadowPass(f);

    VkImage image = m_swapchain->Image(imageIndex);
    VkExtent2D extent = m_swapchain->Extent();
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    ImageBarrier(cmd, m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;
    if (msaa)  // общий для кадров в полёте: ждём запись прошлого кадра
        ImageBarrier(cmd, m_msaaColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // небо закрашивает весь экран
    if (msaa) {
        // Рисуем в мультисэмпловый цвет, в конце прохода GPU усредняет сэмплы в изображение swapchain
        color.imageView = m_msaaColor.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = m_swapchain->View(imageIndex);
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = m_swapchain->View(imageIndex);
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};  // reverse-Z

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    int bound = -1;  // 0 — односторонний пайплайн, 1 — двусторонний (списки отсортированы: сначала односторонние)
    for (const Draw& d : f.draws) {
        if (bound != (d.doubleSided ? 1 : 0)) {
            bound = d.doubleSided ? 1 : 0;
            VkPipeline p = f.rt ? (d.doubleSided ? m_meshRtPipeline2s : m_meshRtPipeline)
                                : (d.doubleSided ? m_meshPipeline2s : m_meshPipeline);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        }
        const MeshGpu* mesh = m_meshes->Get(d.mesh);
        VkDeviceSize offset = 0;
        const VkBuffer vb = d.vertices ? d.vertices : mesh->vertices.buffer;  // скиннутые — свои вершины
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh->indexCount, d.count, 0, 0, d.first);  // один вызов на группу
    }

    vkCmdEndRendering(cmd);
    // dstStage = COLOR_ATTACHMENT_OUTPUT: та же стадия, на которой сигналится семафор для present, —
    // так переход layout гарантированно завершён до сигнала
    ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE);
    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::Render() {
    CE_ZONE("Amper.Render");
    ApplySampleChange();
    Frame& f = m_frames[m_frameIndex];

    uint32_t imageIndex = 0;
    int64_t w0 = NowNs();
    VkResult acq = vkAcquireNextImageKHR(m_device, m_swapchain->Handle(), UINT64_MAX, f.imageAvailable,
                                         VK_NULL_HANDLE, &imageIndex);
    Core().scheduler->NoteWait(NowNs() - w0);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        HandleResize();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        VK_CHECK(acq);

    VK_CHECK(vkResetFences(m_device, 1, &f.fence));
    VK_CHECK(vkResetCommandPool(m_device, f.pool, 0));
    Record(f, imageIndex);

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = m_renderFinished[imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(m_lanes.Submit(GpuLane::Graphics, si, f.fence));

    VkSwapchainKHR sc = m_swapchain->Handle();
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_renderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &imageIndex;
    w0 = NowNs();
    VkResult pres = m_lanes.Present(pi);
    Core().scheduler->NoteWait(NowNs() - w0);

    bool resized = m_window.ConsumeResized();
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || resized)
        HandleResize();
    else if (pres != VK_SUCCESS)
        VK_CHECK(pres);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

} // namespace ce::core
