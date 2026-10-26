#include "Vulkan/Gpu.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"

#include <algorithm>

namespace ce::core {

Gpu::Gpu(const VulkanContext& ctx)
    : m_device(ctx.Device()), m_rtSupported(ctx.RayTracingSupported()), m_families(ctx.UniqueFamilies()) {
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = ctx.PhysicalDevice();
    info.device = ctx.Device();
    info.instance = ctx.Instance();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;  // включён всегда (скиннинг, RT)
    VK_CHECK(vmaCreateAllocator(&info, &m_allocator));

    if (m_rtSupported) {
        m_scratchAlign = std::max<VkDeviceSize>(ctx.AsProperties().minAccelerationStructureScratchOffsetAlignment, 16);
        auto load = [&](const char* name) { return vkGetDeviceProcAddr(m_device, name); };
        m_rt.createAs = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
        m_rt.destroyAs = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
        m_rt.buildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            load("vkGetAccelerationStructureBuildSizesKHR"));
        m_rt.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            load("vkCmdBuildAccelerationStructuresKHR"));
        m_rt.asAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            load("vkGetAccelerationStructureDeviceAddressKHR"));
    }
}

Gpu::~Gpu() {
    if (m_allocator)
        vmaDestroyAllocator(m_allocator);
}

Buffer Gpu::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemUsage mem, bool shared,
                         VkDeviceSize alignment) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (shared && m_families.size() > 1) {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = static_cast<uint32_t>(m_families.size());
        bi.pQueueFamilyIndices = m_families.data();
    } else {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (mem) {
    case MemUsage::GpuOnly:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case MemUsage::Upload:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemUsage::Stream:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemUsage::Readback:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    Buffer b;
    VmaAllocationInfo info{};
    if (alignment > 0)
        VK_CHECK(vmaCreateBufferWithAlignment(m_allocator, &bi, &ai, alignment, &b.buffer, &b.alloc, &info));
    else
        VK_CHECK(vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    b.size = size;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo ad{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ad.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(m_device, &ad);
    }
    return b;
}

void Gpu::Destroy(Buffer& b) {
    if (b.buffer)
        vmaDestroyBuffer(m_allocator, b.buffer, b.alloc);
    b = {};
}

Image Gpu::CreateImage2D(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         uint32_t layers, VkSampleCountFlagBits samples, uint32_t mips) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {extent.width, extent.height, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Отдельная память — только целям рендера (крупные, пересоздаются при ресайзе). Текстуры идут в общие
    // блоки VMA: у драйверов лимит на число выделений (на Windows часто 4096)
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    Image img;
    VK_CHECK(vmaCreateImage(m_allocator, &ii, &ai, &img.image, &img.alloc, nullptr));
    img.format = format;
    img.extent = extent;
    img.mips = mips;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, mips, 0, layers};
    VK_CHECK(vkCreateImageView(m_device, &vi, nullptr, &img.view));
    return img;
}

void Gpu::Destroy(Image& img) {
    if (img.view)
        vkDestroyImageView(m_device, img.view, nullptr);
    if (img.image)
        vmaDestroyImage(m_allocator, img.image, img.alloc);
    img = {};
}

} // namespace ce::core
