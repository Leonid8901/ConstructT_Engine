#pragma once
// Звуковая подсистема поверх miniaudio (ma_engine): микшер, 3D-панорама и затухание, ресемплинг.
// Голоса (ma_sound) живут в пуле со слотами + поколениями, как сущности мира; ma_sound не двигается в памяти.
// Все публичные методы под одним мьютексом: звать можно из любых задач UniX.
#include "CE/Audio.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ma_engine;
struct ma_sound;

namespace ce::core {

class WorldImpl;

struct AudioSettings {
    bool enabled = true;
    float master = 1.0f;
    std::array<float, static_cast<size_t>(AudioGroup::Count)> groups{1, 1, 1, 1};
};

class AudioSystem {
public:
    explicit AudioSystem(const AudioSettings& settings);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Available() const { return m_ok; }
    std::string Description() const { return m_desc; }

    uint32_t Load(const std::filesystem::path& path, bool stream);  // бросает std::runtime_error
    uint32_t Create(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate);
    float Duration(uint32_t sound);

    bool Play(uint32_t sound, const SoundParams& p, uint32_t& index, uint32_t& gen);
    bool Playing(uint32_t index, uint32_t gen);
    void SetVolume(uint32_t index, uint32_t gen, float v);
    void SetPitch(uint32_t index, uint32_t gen, float v);
    void SetPosition(uint32_t index, uint32_t gen, Vec3 p);
    void Stop(uint32_t index, uint32_t gen, float fadeSeconds);
    void StopAll(float fadeSeconds);

    void PlayMusic(uint32_t sound, float fade, bool loop, float volume);
    void StopMusic(float fade);

    void SetMaster(float v);
    float Master();
    void SetGroup(AudioGroup g, float v);
    float Group(AudioGroup g);
    uint32_t PlayingCount();

    // Главный поток, раз в кадр: слушатель = камера, звуки за сущностями, уборка доигравших
    void Update(Vec3 camPos, Vec3 forward, Vec3 up, WorldImpl& world);

private:
    struct SoundData {
        std::filesystem::path path;  // пусто — сэмплы в памяти
        bool stream = false;
        std::vector<float> samples;
        uint32_t frames = 0, channels = 0, rate = 0;
        ma_sound* anchor = nullptr;  // держит декодированные данные в кэше resource manager
        float duration = 0;
    };
    struct VoiceSlot {
        ma_sound* sound = nullptr;
        void* buffer = nullptr;  // ma_audio_buffer для звуков из памяти: свой курсор чтения на голос
        uint32_t gen = 0;
        bool alive = false;
        bool music = false;
        bool follow = false;
        bool followSeen = false;  // сущность уже была живой (созданная в той же стадии появится после Flush)
        uint8_t followWait = 0;
        uint32_t entity = 0, entityGen = 0;
    };

    VoiceSlot* Find(uint32_t index, uint32_t gen);
    void Release(uint32_t index);
    bool PlayLocked(uint32_t sound, const SoundParams& p, uint32_t& index, uint32_t& gen, bool music);
    void StopLocked(VoiceSlot& v, float fadeSeconds);

    std::mutex m_mutex;
    bool m_ok = false;
    std::string m_desc;
    std::unique_ptr<ma_engine> m_engine;
    std::array<void*, static_cast<size_t>(AudioGroup::Count)> m_groups{};  // ma_sound_group*
    std::array<float, static_cast<size_t>(AudioGroup::Count)> m_groupVolume{1, 1, 1, 1};
    float m_master = 1.0f;

    std::vector<std::unique_ptr<SoundData>> m_sounds;
    std::unordered_map<std::string, uint32_t> m_byPath;
    std::vector<VoiceSlot> m_voices;
    std::vector<uint32_t> m_free;
    uint32_t m_musicIndex = UINT32_MAX, m_musicGen = 0;
    bool m_limitLogged = false;

    static constexpr uint32_t kMaxVoices = 256;
};

} // namespace ce::core
