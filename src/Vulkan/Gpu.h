#pragma once
// Память GPU (VMA) и указатели на функции ray tracing.
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace ce::core {

class VulkanContext;

// Stream — данные, которые CPU пишет каждый кадр, а GPU читает (инстансы, UBO, свет).
// Предпочитает видеопамять, видимую CPU (окно BAR 256 МБ или Resizable BAR); если её нет или она
// закончилась, VMA сам откатится на обычную host-visible память. Писать только последовательно, не читать.
enum class MemUsage { GpuOnly, Upload, Readback, Stream };

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mips = 1;
};

struct RtFunctions {
    PFN_vkCreateAccelerationStructureKHR createAs = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAs = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress = nullptr;
};

class Gpu {
public:
    explicit Gpu(const VulkanContext& ctx);
    ~Gpu();

    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    // shared=true — буфер доступен всем семьям очередей без ownership transfer (для GPU-лейнов UniX)
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemUsage mem, bool shared = false,
                        VkDeviceSize alignment = 0);
    void Destroy(Buffer& b);

    Image CreateImage2D(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        uint32_t layers = 1, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                        uint32_t mips = 1);
    void Destroy(Image& img);

    VkDevice Device() const { return m_device; }
    const RtFunctions& Rt() const { return m_rt; }
    bool RayTracing() const { return m_rtSupported; }
    VkDeviceSize ScratchAlignment() const { return m_scratchAlign; }

private:
    VkDevice m_device;
    VmaAllocator m_allocator = nullptr;
    RtFunctions m_rt;
    bool m_rtSupported;
    VkDeviceSize m_scratchAlign = 256;
    std::vector<uint32_t> m_families;
};

} // namespace ce::core
