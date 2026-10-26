#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Core.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Vulkan/VkCheck.h"

#include <stdexcept>

namespace ce::core {

struct WindowCallbacks {
    static Window* Get(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }
    static void Resize(GLFWwindow* w, int, int) { Get(w)->m_resized = true; }
    static void Key(GLFWwindow* w, int key, int, int action, int) { Get(w)->m_input->OnKey(key, action); }
    static void Button(GLFWwindow* w, int b, int action, int) { Get(w)->m_input->OnMouseButton(b, action); }
    static void Cursor(GLFWwindow* w, double x, double y) { Get(w)->m_input->OnCursor(x, y); }
    static void Scroll(GLFWwindow* w, double, double dy) { Get(w)->m_input->OnScroll(dy); }
};

Window::Window(uint32_t width, uint32_t height, const std::string& title, InputState* input,
               const std::string& platform)
    : m_input(input) {
    glfwSetErrorCallback([](int code, const char* desc) { Log("GLFW " + std::to_string(code) + ": " + desc); });
    int hint = GLFW_ANY_PLATFORM;
    if (platform == "x11") hint = GLFW_PLATFORM_X11;
    else if (platform == "wayland") hint = GLFW_PLATFORM_WAYLAND;
    else if (platform == "win32") hint = GLFW_PLATFORM_WIN32;
    if (hint != GLFW_ANY_PLATFORM && !glfwPlatformSupported(hint)) {
        Log("Окно: платформа \"" + platform + "\" не поддерживается этой сборкой GLFW — выбор автоматически");
        hint = GLFW_ANY_PLATFORM;
    }
    glfwInitHint(GLFW_PLATFORM, hint);
    bool ok = glfwInit();
    if (!ok && hint != GLFW_ANY_PLATFORM) {  // например, просили wayland, а сессия X11
        Log("Окно: не удалось запустить \"" + platform + "\" — пробую автоматически");
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        ok = glfwInit();
    }
    if (!ok)
        throw std::runtime_error("Не удалось создать окно (glfwInit). На Linux нужен X11 или Wayland (DISPLAY / WAYLAND_DISPLAY).");
    if (!glfwVulkanSupported()) {
        glfwTerminate();
        throw std::runtime_error("Vulkan не найден: нет загрузчика vulkan-1 / libvulkan.so.1 или драйвера. "
                                 "Установите драйвер видеокарты (на Linux — mesa-vulkan-drivers или драйвер NVIDIA).");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    m_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, WindowCallbacks::Resize);
    glfwSetKeyCallback(m_window, WindowCallbacks::Key);
    glfwSetMouseButtonCallback(m_window, WindowCallbacks::Button);
    glfwSetCursorPosCallback(m_window, WindowCallbacks::Cursor);
    glfwSetScrollCallback(m_window, WindowCallbacks::Scroll);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

Window::~Window() {
    if (m_window)
        glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::ShouldClose() const { return glfwWindowShouldClose(m_window); }
void Window::PollEvents() const { glfwPollEvents(); }
void Window::WaitEvents() const { glfwWaitEvents(); }
void Window::SetTitle(const std::string& title) const { glfwSetWindowTitle(m_window, title.c_str()); }
void Window::FramebufferSize(int& width, int& height) const { glfwGetFramebufferSize(m_window, &width, &height); }

bool Window::ConsumeResized() {
    bool r = m_resized;
    m_resized = false;
    return r;
}

void Window::SetCursorLocked(bool locked) const {
    glfwSetInputMode(m_window, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

std::string Window::PlatformName() const {
    switch (glfwGetPlatform()) {
    case GLFW_PLATFORM_WIN32: return "Win32";
    case GLFW_PLATFORM_X11: return "X11";
    case GLFW_PLATFORM_WAYLAND: return "Wayland";
    case GLFW_PLATFORM_COCOA: return "Cocoa";
    default: return "unknown";
    }
}

VkSurfaceKHR Window::CreateSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VK_CHECK(glfwCreateWindowSurface(instance, m_window, nullptr, &surface));
    return surface;
}

std::vector<const char*> Window::RequiredInstanceExtensions() {
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    return {exts, exts + count};
}

// ---------------- Input ----------------

void InputState::NewFrame(Window& window) {
    int lock = m_lockRequest.exchange(-1);
    if (lock >= 0)
        window.SetCursorLocked(lock == 1);

    m_cur = m_live;
    m_pressed = m_livePressed;
    m_released = m_liveReleased;
    m_livePressed.fill(0);
    m_liveReleased.fill(0);
    m_curBtn = m_liveBtn;
    m_pressedBtn = m_livePressedBtn;
    m_livePressedBtn.fill(0);
    m_delta = m_mouseLive - m_mouse;
    m_mouse = m_mouseLive;
    m_scroll = m_scrollLive;
    m_scrollLive = 0;
}

void InputState::OnKey(int key, int action) {
    if (key < 0 || key >= kKeys || action == GLFW_REPEAT)
        return;
    m_live[key] = action == GLFW_PRESS ? 1 : 0;
    (action == GLFW_PRESS ? m_livePressed : m_liveReleased)[key] = 1;
}

void InputState::OnMouseButton(int button, int action) {
    if (button < 0 || button >= kButtons)
        return;
    m_liveBtn[button] = action == GLFW_PRESS ? 1 : 0;
    if (action == GLFW_PRESS)
        m_livePressedBtn[button] = 1;
}

void InputState::OnCursor(double x, double y) {
    m_mouseLive = {static_cast<float>(x), static_cast<float>(y)};
    if (!m_hasMouse) {
        m_mouse = m_mouseLive;
        m_hasMouse = true;
    }
}

void InputState::OnScroll(double dy) { m_scrollLive += static_cast<float>(dy); }

} // namespace ce::core
