#pragma once
// Пример модуля «Скример»: резкий звук, белая вспышка света и рывок угла обзора. Всё на публичном API движка.
//
//   auto* scare = Modules.Add<lib::Jumpscare>();     // nullptr, если игрок выключил в engine.json
//   if (scare) scare->Trigger();                      // из любой задачи и из обработчиков
//
// engine.json → "modules" → "Jumpscare": cooldown (с между скримерами, 8), volume (громкость, 1),
// flash (вспышка, true), fovKick (рывок угла обзора, true), enabled (false — модуль не грузится вообще).
// Для игроков с чувствительностью к вспышкам: "flash": false, "fovKick": false оставляют один звук.
#include <CustomEngine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace lib {

class Jumpscare : public ce::Module {
public:
    const char* Name() const override { return "Jumpscare"; }

    void OnLoad() override {
        const ce::ModuleSettings s = Settings();
        m_cooldown = s.Number("cooldown", 8.0f);
        m_volume = s.Number("volume", 1.0f);
        m_flash = s.Bool("flash", true);
        m_fovKick = s.Bool("fovKick", true);
        m_sound = MakeStinger();
        // Эффекты живут в своей задаче: Trigger можно звать из любого потока, он только запоминает «когда».
        NewTask("Effects", ce::Cost::Low, ce::Priority::Normal, [this](ce::Task&) { Tick(); });
    }

    void OnUnload() override { Finish(); }

    // false — идёт перезарядка (cooldown) или скример уже играет. intensity 1 — обычный, 2 — вдвое сильнее.
    bool Trigger(float intensity = 1.0f) {
        const double now = ce::Engine::Time();
        std::lock_guard lk(m_mutex);
        if (now < m_readyAt)
            return false;
        m_readyAt = now + m_cooldown;
        m_pending = true;
        m_intensity = std::max(intensity, 0.1f);
        return true;
    }

    bool Playing() const {
        std::lock_guard lk(m_mutex);
        return m_active;
    }

private:
    static constexpr float kLength = 0.7f;  // секунд на эффекты глаза; звук длиннее

    static ce::Sound MakeStinger() {
        constexpr uint32_t kRate = 44100;
        const float seconds = 1.6f;
        std::vector<float> data(static_cast<size_t>(seconds * kRate));
        uint32_t noise = 12345u;
        for (size_t i = 0; i < data.size(); ++i) {
            const float t = static_cast<float>(i) / kRate;
            // три диссонирующих тона с падающей высотой + щелчок шума в начале
            const float sweep = 1.0f - 0.35f * t / seconds;
            float v = std::sin(6.2832f * 233.0f * sweep * t) + std::sin(6.2832f * 247.0f * sweep * t) +
                      0.7f * std::sin(6.2832f * 330.0f * sweep * t);
            noise = noise * 1664525u + 1013904223u;
            const float n = (static_cast<float>(noise >> 8) / 8388608.0f) - 1.0f;
            v = v * 0.28f + n * 0.9f * std::exp(-t * 18.0f);
            const float attack = std::min(t / 0.002f, 1.0f);  // почти мгновенный удар
            data[i] = std::clamp(v * attack * std::exp(-t * 2.2f), -0.95f, 0.95f);
        }
        return ce::Sound::Create(data.data(), static_cast<uint32_t>(data.size()), 1, kRate);
    }

    void Tick() {
        bool start = false;
        float intensity = 1;
        {
            std::lock_guard lk(m_mutex);
            if (m_pending) {
                m_pending = false;
                start = true;
                intensity = m_intensity;
            }
        }
        const double now = ce::Engine::Time();
        if (start) {
            if (m_active)
                Finish();  // предыдущий закончить аккуратно (вернуть угол обзора)
            ce::SoundParams p;
            p.volume = std::min(m_volume * intensity, 2.0f);
            ce::Audio.Play(m_sound, p);
            m_baseFov = ce::Camera.Fov();
            m_start = now;
            m_peak = intensity;
            if (m_flash)
                m_light = ce::Amper.AddPointLight(ce::Camera.Position(), {0.9f, 0.95f, 1.0f}, 0.0f, 30.0f);
            std::lock_guard lk(m_mutex);
            m_active = true;
        }
        if (!m_active)
            return;
        const float t = static_cast<float>(now - m_start) / kLength;
        if (t >= 1.0f) {
            Finish();
            return;
        }
        const float k = (1.0f - t) * (1.0f - t);  // быстрый спад
        if (m_light.Valid())
            m_light.SetPosition(ce::Camera.Position()).SetIntensity(60.0f * m_peak * k);
        if (m_fovKick)
            ce::Camera.SetFov(m_baseFov - 22.0f * m_peak * k);  // «наезд»
    }

    void Finish() {
        if (m_light.Valid())
            m_light.Remove();
        m_light = ce::Light{};
        if (m_active && m_fovKick)
            ce::Camera.SetFov(m_baseFov);
        std::lock_guard lk(m_mutex);
        m_active = false;
    }

    float m_cooldown = 8, m_volume = 1;
    bool m_flash = true, m_fovKick = true;
    ce::Sound m_sound;
    ce::Light m_light;
    mutable std::mutex m_mutex;
    double m_readyAt = 0;
    bool m_pending = false;
    float m_intensity = 1;
    // только из задачи Effects:
    bool m_active = false;
    double m_start = 0;
    float m_peak = 1, m_baseFov = 60;
};

} // namespace lib
