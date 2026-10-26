#pragma once
// Звук (miniaudio внутри): эффекты в 3D, музыка, группы громкости.
// Слушатель — камера: громкость падает с расстоянием, панорама — по направлению взгляда.
// Все методы потокобезопасны: можно звать из любых задач UniX.
// Нет звуковой карты — miniaudio выбирает «пустой» вывод: всё работает, но тихо.
// Audio.Available() == false — только если звук выключен (engine.json / EngineConfig::audio) или не запустился.
#include "CE/Math.h"
#include "CE/World.h"

#include <cstdint>
#include <string>

namespace ce {

namespace core { struct Access; }

enum class AudioGroup : uint8_t { Effects, Music, Voice, UI, Count };

// Звуковой файл или буфер сэмплов. Повторный Load того же пути возвращает тот же звук.
class Sound {
public:
    Sound() = default;
    // WAV, MP3, FLAC. stream = true — декодировать на лету (длинная музыка), иначе целиком в память (эффекты).
    static Sound Load(const std::string& path, bool stream = false);
    // Свои сэмплы: float -1..1, каналы чередуются (L R L R …). Данные копируются.
    static Sound Create(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate);

    bool Valid() const { return m_id != UINT32_MAX; }
    float Duration() const;  // секунды

private:
    friend struct core::Access;
    uint32_t m_id = UINT32_MAX;
};

struct SoundParams {
    float volume = 1.0f;
    float pitch = 1.0f;          // 2 — на октаву выше и вдвое быстрее
    bool loop = false;
    AudioGroup group = AudioGroup::Effects;
    bool spatial = false;        // 3D: позиция или сущность ниже
    Vec3 position{};
    Entity follow{};             // если задана — звук едет за сущностью (и останавливается, когда её удалят)
    float minDistance = 1.0f;    // ближе — полная громкость
    float maxDistance = 60.0f;   // дальше громкость не падает (затухание обратно пропорциональное)
    float fadeIn = 0.0f;         // секунды
};

// Играющий экземпляр звука. Дескриптор: после окончания звука становится невалидным.
class Voice {
public:
    Voice() = default;
    bool Playing() const;
    Voice& SetVolume(float volume);
    Voice& SetPitch(float pitch);
    Voice& SetPosition(Vec3 position);  // для 3D-звука без follow
    void Stop(float fadeSeconds = 0.0f);

private:
    friend struct core::Access;
    uint32_t m_index = UINT32_MAX;
    uint32_t m_gen = 0;
};

struct AudioApi {
    bool Available() const;  // false — звук выключен или не запустился (все вызовы ничего не делают)

    Voice Play(Sound sound, const SoundParams& params = {}) const;
    Voice PlayAt(Sound sound, Vec3 position, float volume = 1.0f) const;              // 3D в точке
    Voice PlayOn(Sound sound, Entity entity, float volume = 1.0f, bool loop = false) const;  // 3D за сущностью

    // Музыка: одна дорожка, новая плавно сменяет старую (crossfade)
    void PlayMusic(Sound music, float fadeSeconds = 1.0f, bool loop = true, float volume = 1.0f) const;
    void StopMusic(float fadeSeconds = 1.0f) const;

    void SetMasterVolume(float volume) const;  // 0..1 (больше 1 — усиление)
    float MasterVolume() const;
    void SetGroupVolume(AudioGroup group, float volume) const;
    float GroupVolume(AudioGroup group) const;
    void StopAll(float fadeSeconds = 0.0f) const;  // кроме музыки

    uint32_t PlayingCount() const;
};
inline constexpr AudioApi Audio{};

} // namespace ce
