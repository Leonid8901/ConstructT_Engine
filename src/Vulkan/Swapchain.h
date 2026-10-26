#pragma once
#include <vulkan/vulkan.h>

#include <vector>

namespace ce::core {

class VulkanContext;
class Window;

class Swapchain {
public:
    Swapchain(const VulkanContext& ctx, const Window& window, bool vsync);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void Recreate();  // ждёт, пока окно не перестанет быть свёрнутым

    VkSwapchainKHR Handle() const { return m_swapchain; }
    VkFormat Format() const { return m_format; }
    bool ManualSrgb() const { return m_manualSrgb; }  // формат UNORM: гамму кодирует шейдер
    VkExtent2D Extent() const { return m_extent; }
    uint32_t ImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage Image(uint32_t i) const { return m_images[i]; }
    VkImageView View(uint32_t i) const { return m_views[i]; }

private:
    void Create(VkSwapchainKHR oldSwapchain);
    void DestroyViews();

    const VulkanContext& m_ctx;
    const Window& m_window;
    bool m_vsync;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    bool m_manualSrgb = false;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
};

} // namespace ce::core
