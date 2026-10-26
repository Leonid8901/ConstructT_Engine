#include "Amper/TextureRegistry.h"
#include "Core.h"
#include "UniX/GpuLanes.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"

#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ce::core {

namespace {

void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t mip, uint32_t count, VkImageLayout oldL, VkImageLayout newL,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
             VkAccessFlags2 dstAccess) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

TextureRegistry::TextureRegistry(const VulkanContext& ctx, Gpu& gpu, GpuLanes& lanes)
    : m_ctx(ctx), m_gpu(gpu), m_lanes(lanes), m_device(ctx.Device()) {
    // Размер массива: 4096, но не больше лимита устройства для update-after-bind
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(ctx.PhysicalDevice(), &p2);
    m_capacity = std::min<uint32_t>({4096u, p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                     p12.maxDescriptorSetUpdateAfterBindSampledImages});
    m_slots = std::make_unique<Slot[]>(m_capacity);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (ctx.Anisotropy()) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(8.0f, ctx.Properties().limits.maxSamplerAnisotropy);
    }
    VK_CHECK(vkCreateSampler(m_device, &si, nullptr, &m_sampler));

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                                           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_capacity,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 1;
    li.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_layout));

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_capacity};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool));

    VkDescriptorSetVariableDescriptorCountAllocateInfo vc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    vc.descriptorSetCount = 1;
    vc.pDescriptorCounts = &m_capacity;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = &vc;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_layout;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &ai, &m_set));
}

TextureRegistry::~TextureRegistry() {
    const uint32_t n = Count();
    for (uint32_t i = 0; i < n; ++i)
        m_gpu.Destroy(m_slots[i].image);
    vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    vkDestroySampler(m_device, m_sampler, nullptr);
}

uint32_t TextureRegistry::Create(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb) {
    if (!rgba || width == 0 || height == 0)
        throw std::invalid_argument("Texture::Create: пустое изображение");
    const uint32_t maxDim = m_ctx.Properties().limits.maxImageDimension2D;
    if (width > maxDim || height > maxDim)
        throw std::invalid_argument("Texture::Create: " + std::to_string(width) + "x" + std::to_string(height) +
                                    " больше лимита GPU " + std::to_string(maxDim));

    uint32_t id = m_count.load();
    while (true) {
        if (id >= m_capacity)
            throw std::runtime_error("Texture: закончились слоты (" + std::to_string(m_capacity) + ")");
        if (m_count.compare_exchange_weak(id, id + 1))
            break;
    }

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(m_ctx.PhysicalDevice(), format, &fp);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const bool canBlit = (fp.optimalTilingFeatures & blit) == blit;
    uint32_t mips = 1;
    if (canBlit)
        for (uint32_t s = std::max(width, height); s > 1; s /= 2)
            ++mips;

    Image img = m_gpu.CreateImage2D({width, height}, format,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_SAMPLE_COUNT_1_BIT, mips);

    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    Buffer staging = m_gpu.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    std::memcpy(staging.mapped, rgba, bytes);

    // Копирование и мипы — на графической очереди: блиты есть только там
    m_lanes.Immediate(GpuLane::Graphics, [&](VkCommandBuffer cmd) {
        Barrier(cmd, img.image, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);  // мипы 1+ пишет блит — его стадия тоже должна ждать переход
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        int32_t w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
        for (uint32_t m = 1; m < mips; ++m) {
            Barrier(cmd, img.image, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            const int32_t nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
            VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            region.srcOffsets[1] = {w, h, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            region.dstOffsets[1] = {nw, nh, 1};
            VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
            bi.srcImage = img.image;
            bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bi.dstImage = img.image;
            bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bi.regionCount = 1;
            bi.pRegions = &region;
            bi.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &bi);
            w = nw;
            h = nh;
        }
        // Мипы 0..mips-2 сейчас в TRANSFER_SRC, последний — в TRANSFER_DST
        if (mips > 1)
            Barrier(cmd, img.image, 0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        Barrier(cmd, img.image, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    m_gpu.Destroy(staging);
    m_slots[id].image = img;

    VkDescriptorImageInfo ii{m_sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_set;
    w.dstBinding = 0;
    w.dstArrayElement = id;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    {
        std::lock_guard lk(m_writeMutex);
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    }
    return id;
}

uint32_t TextureRegistry::CreateFromMemory(const uint8_t* data, size_t size, bool srgb, const std::string& name) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels)
        throw std::runtime_error("Texture: не удалось декодировать " + name + ": " + stbi_failure_reason());
    try {
        const uint32_t id = Create(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h), srgb);
        stbi_image_free(pixels);
        return id;
    } catch (...) {
        stbi_image_free(pixels);
        throw;
    }
}

uint32_t TextureRegistry::Load(const std::string& path, bool srgb) {
    const std::string key = path + (srgb ? "|srgb" : "|linear");
    {
        std::lock_guard lk(m_cacheMutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end())
            return it->second;
    }
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!f)
        throw std::runtime_error("Texture::Load: нет файла " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint32_t id = CreateFromMemory(bytes.data(), bytes.size(), srgb, path);
    std::lock_guard lk(m_cacheMutex);
    return m_cache.emplace(key, id).first->second;  // параллельная загрузка того же файла: остаётся первая
}

} // namespace ce::core
