#pragma once
#include "Platform/Settings.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ce::core {

class Window;

struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;
    uint32_t compute = UINT32_MAX;   // выделенная async-compute семья, иначе = graphics
    uint32_t transfer = UINT32_MAX;  // выделенная DMA-семья, иначе compute/graphics
};

// Instance, debug messenger, surface, physical/logical device.
// Требует Vulkan 1.3 (dynamic rendering + synchronization2); ray query — опционально.
// Выбор GPU: дискретный > встроенный > виртуальный > программный, при равенстве — больше VRAM.
// devicePreference ("auto", номер GPU в списке лога или часть имени) — из engine.json.
class VulkanContext {
public:
    explicit VulkanContext(const Window& window, const std::string& devicePreference = "auto");
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance Instance() const { return m_instance; }
    VkSurfaceKHR Surface() const { return m_surface; }
    VkPhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
    VkDevice Device() const { return m_device; }
    const QueueFamilies& Families() const { return m_families; }
    std::vector<uint32_t> UniqueFamilies() const;
    const VkPhysicalDeviceProperties& Properties() const { return m_props; }

    bool RayTracingSupported() const { return m_rtSupported; }
    const GpuInfo& Info() const { return m_info; }
    bool Anisotropy() const { return m_anisotropy; }
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR& AsProperties() const { return m_asProps; }

private:
    void CreateInstance();
    void SetupDebugMessenger();
    void PickPhysicalDevice(const std::string& preference);
    void CreateDevice();
    bool IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies& out, std::string& reason) const;
    void FillInfo();

    bool m_validation = false;
    bool m_rtSupported = false;
    bool m_anisotropy = false;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QueueFamilies m_families;
    VkPhysicalDeviceProperties m_props{};
    GpuInfo m_info;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
};

} // namespace ce::core
