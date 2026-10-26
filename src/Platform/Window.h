#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace ce::core {

class InputState;

class Window {
public:
    // platform: "auto" | "x11" | "wayland" | "win32" (engine.json). На Linux "auto" — Wayland, если сессия Wayland, иначе X11.
    Window(uint32_t width, uint32_t height, const std::string& title, InputState* input,
           const std::string& platform = "auto");
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool ShouldClose() const;
    void PollEvents() const;
    void WaitEvents() const;
    void SetTitle(const std::string& title) const;
    void FramebufferSize(int& width, int& height) const;
    bool ConsumeResized();
    void SetCursorLocked(bool locked) const;
    std::string PlatformName() const;

    VkSurfaceKHR CreateSurface(VkInstance instance) const;
    static std::vector<const char*> RequiredInstanceExtensions();

private:
    GLFWwindow* m_window = nullptr;
    InputState* m_input = nullptr;
    bool m_resized = false;

    friend struct WindowCallbacks;
};

} // namespace ce::core
