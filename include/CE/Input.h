#pragma once
#include "CE/Math.h"

namespace ce {

enum class Key : int {
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47, Semicolon = 59, Equal = 61,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    KpSubtract = 333, KpAdd = 334,
    LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt
};

enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

// Снимок ввода делается один раз за кадр (стадия Sys), читать можно из любой задачи.
struct InputApi {
    bool Down(Key k) const;
    bool Pressed(Key k) const;    // нажата в этом кадре
    bool Released(Key k) const;
    bool MouseDown(MouseButton b) const;
    bool MousePressed(MouseButton b) const;
    Vec2 MousePosition() const;
    Vec2 MouseDelta() const;
    float Scroll() const;
    void LockCursor(bool locked) const;
};
inline constexpr InputApi Input{};

} // namespace ce
