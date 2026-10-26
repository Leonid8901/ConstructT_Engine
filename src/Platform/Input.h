#pragma once
#include "CE/Math.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ce::core {

class Window;

// Живое состояние пишут колбэки GLFW (главный поток), снимок кадра читают задачи.
class InputState {
public:
    static constexpr int kKeys = 512;
    static constexpr int kButtons = 8;

    void NewFrame(Window& window);

    void OnKey(int key, int action);
    void OnMouseButton(int button, int action);
    void OnCursor(double x, double y);
    void OnScroll(double dy);

    // Нажатия защёлкиваются: даже короткий тап между двумя кадрами не теряется
    bool Down(int k) const { return k >= 0 && k < kKeys && (m_cur[k] || m_pressed[k]); }
    bool Pressed(int k) const { return k >= 0 && k < kKeys && m_pressed[k]; }
    bool Released(int k) const { return k >= 0 && k < kKeys && m_released[k]; }
    bool MouseDown(int b) const { return b >= 0 && b < kButtons && (m_curBtn[b] || m_pressedBtn[b]); }
    bool MousePressed(int b) const { return b >= 0 && b < kButtons && m_pressedBtn[b]; }
    Vec2 MousePosition() const { return m_mouse; }
    Vec2 MouseDelta() const { return m_delta; }
    float Scroll() const { return m_scroll; }
    void RequestCursorLock(bool locked) { m_lockRequest.store(locked ? 1 : 0); }

private:
    std::array<uint8_t, kKeys> m_live{}, m_cur{}, m_livePressed{}, m_liveReleased{}, m_pressed{}, m_released{};
    std::array<uint8_t, kButtons> m_liveBtn{}, m_curBtn{}, m_livePressedBtn{}, m_pressedBtn{};
    Vec2 m_mouseLive, m_mouse, m_delta;
    float m_scrollLive = 0, m_scroll = 0;
    bool m_hasMouse = false;
    std::atomic<int> m_lockRequest{-1};
};

} // namespace ce::core
