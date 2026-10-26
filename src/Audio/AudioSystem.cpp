#include "Audio/AudioSystem.h"
#include "Core.h"
#include "World/WorldImpl.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define MA_NO_GENERATION  // генераторы шума/синуса не нужны
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <stdexcept>

namespace ce::core {

namespace {

ma_sound_group* G(void* p) { return static_cast<ma_sound_group*>(p); }

ma_uint64 Ms(float seconds) { return seconds > 0 ? static_cast<ma_uint64>(seconds * 1000.0f) : 0; }

} // namespace

AudioSystem::AudioSystem(const AudioSettings& s) : m_engine(std::make_unique<ma_engine>()) {
    m_master = s.master;
    m_groupVolume = s.groups;
    if (!s.enabled) {
        m_desc = "выключен (engine.json)";
        return;
    }
    // Звуковая карта не нашлась — miniaudio берёт «null»-бэкенд (тишина): игра продолжает работать
    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, m_engine.get()) != MA_SUCCESS) {
        m_desc = "недоступен (нет устройства вывода)";
        return;
    }
    m_ok = true;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        auto* group = new ma_sound_group;
        if (ma_sound_group_init(m_engine.get(), 0, nullptr, group) != MA_SUCCESS) {
            delete group;
            continue;
        }
        ma_sound_group_set_volume(group, m_groupVolume[g]);
        m_groups[g] = group;
    }
    ma_engine_set_volume(m_engine.get(), m_master);
    ma_engine_listener_set_world_up(m_engine.get(), 0, 0, 1, 0);

    ma_device* dev = ma_engine_get_device(m_engine.get());
    m_desc = std::string(ma_get_backend_name(dev->pContext->backend)) + ", " + std::to_string(ma_engine_get_sample_rate(m_engine.get())) +
             " Гц, " + std::to_string(ma_engine_get_channels(m_engine.get())) + " кан.";
    if (dev->pContext->backend == ma_backend_null)
        m_desc += " (устройства нет — тишина)";
}

AudioSystem::~AudioSystem() {
    if (!m_ok)
        return;
    for (uint32_t i = 0; i < m_voices.size(); ++i)
        if (m_voices[i].alive)
            Release(i);
    for (auto& sd : m_sounds)
        if (sd->anchor) {
            ma_sound_uninit(sd->anchor);
            delete sd->anchor;
        }
    for (void* g : m_groups)
        if (g) {
            ma_sound_group_uninit(G(g));
            delete G(g);
        }
    ma_engine_uninit(m_engine.get());
}

// ---------------- звуки ----------------

uint32_t AudioSystem::Load(const std::filesystem::path& path, bool stream) {
    std::lock_guard lk(m_mutex);
    const std::u8string u8 = path.lexically_normal().generic_u8string();
    const std::string key = std::string(u8.begin(), u8.end()) + (stream ? "|stream" : "");
    if (auto it = m_byPath.find(key); it != m_byPath.end())
        return it->second;

    auto sd = std::make_unique<SoundData>();
    sd->path = path;
    sd->stream = stream;
    if (m_ok) {
        // Сначала проверяем файл отдельным декодером: в miniaudio 0.11.22–0.11.25 при ошибке загрузки
        // в resource manager обращение к уже освобождённому узлу (use-after-free, видно под ASan)
        ma_decoder probe;
#ifdef _WIN32
        ma_result pr = ma_decoder_init_file_w(path.wstring().c_str(), nullptr, &probe);
#else
        ma_result pr = ma_decoder_init_file(path.c_str(), nullptr, &probe);
#endif
        if (pr != MA_SUCCESS)
            throw std::runtime_error("Sound::Load: " + std::string(reinterpret_cast<const char*>(path.u8string().c_str())) +
                                     ": " + ma_result_description(pr) + " (поддерживаются WAV, MP3, FLAC)");
        ma_decoder_uninit(&probe);

        sd->anchor = new ma_sound;
        const ma_uint32 flags = (stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE) | MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
        const ma_result r = ma_sound_init_from_file_w(m_engine.get(), path.wstring().c_str(), flags, nullptr, nullptr, sd->anchor);
#else
        const ma_result r = ma_sound_init_from_file(m_engine.get(), path.c_str(), flags, nullptr, nullptr, sd->anchor);
#endif
        if (r != MA_SUCCESS) {
            delete sd->anchor;
            throw std::runtime_error("Sound::Load: " + std::string(reinterpret_cast<const char*>(path.u8string().c_str())) +
                                     ": " + ma_result_description(r) + " (поддерживаются WAV, MP3, FLAC)");
        }
        ma_sound_get_length_in_seconds(sd->anchor, &sd->duration);
    } else {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            throw std::runtime_error("Sound::Load: файл не найден: " +
                                     std::string(reinterpret_cast<const char*>(path.u8string().c_str())));
    }
    const auto id = static_cast<uint32_t>(m_sounds.size());
    m_sounds.push_back(std::move(sd));
    m_byPath.emplace(key, id);
    return id;
}

uint32_t AudioSystem::Create(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate) {
    if (!samples || frames == 0 || channels == 0 || channels > 8 || sampleRate == 0)
        throw std::invalid_argument("Sound::Create: пустые данные или неверные каналы/частота");
    std::lock_guard lk(m_mutex);
    auto sd = std::make_unique<SoundData>();
    sd->samples.assign(samples, samples + size_t(frames) * channels);
    sd->frames = frames;
    sd->channels = channels;
    sd->rate = sampleRate;
    sd->duration = static_cast<float>(frames) / static_cast<float>(sampleRate);
    const auto id = static_cast<uint32_t>(m_sounds.size());
    m_sounds.push_back(std::move(sd));
    return id;
}

float AudioSystem::Duration(uint32_t sound) {
    std::lock_guard lk(m_mutex);
    return sound < m_sounds.size() ? m_sounds[sound]->duration : 0.0f;
}

// ---------------- голоса ----------------

AudioSystem::VoiceSlot* AudioSystem::Find(uint32_t index, uint32_t gen) {
    if (index >= m_voices.size())
        return nullptr;
    VoiceSlot& v = m_voices[index];
    return v.alive && v.gen == gen ? &v : nullptr;
}

void AudioSystem::Release(uint32_t index) {
    VoiceSlot& v = m_voices[index];
    ma_sound_uninit(v.sound);
    delete v.sound;
    if (v.buffer) {
        ma_audio_buffer_uninit(static_cast<ma_audio_buffer*>(v.buffer));
        delete static_cast<ma_audio_buffer*>(v.buffer);
    }
    const uint32_t gen = v.gen + 1;  // старые дескрипторы Voice становятся невалидными
    v = VoiceSlot{};
    v.gen = gen;
    m_free.push_back(index);
}

bool AudioSystem::PlayLocked(uint32_t sound, const SoundParams& p, uint32_t& index, uint32_t& gen, bool music) {
    if (!m_ok || sound >= m_sounds.size())
        return false;
    if (m_voices.size() - m_free.size() >= kMaxVoices) {
        if (!m_limitLogged)
            Log("Audio: одновременно играет " + std::to_string(kMaxVoices) + " звуков — новые пропускаются");
        m_limitLogged = true;
        return false;
    }
    const SoundData& sd = *m_sounds[sound];
    ma_sound_group* group = G(m_groups[static_cast<size_t>(p.group)]);
    const bool follow = Access::Index(p.follow) != UINT32_MAX;
    const bool spatial = p.spatial || follow;
    ma_uint32 flags = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;

    auto* snd = new ma_sound;
    ma_audio_buffer* buf = nullptr;
    ma_result r;
    if (sd.path.empty()) {
        // Сэмплы общие для всех голосов этого звука, у каждого голоса — свой курсор
        buf = new ma_audio_buffer;
        ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, sd.channels, sd.frames, sd.samples.data(), nullptr);
        bc.sampleRate = sd.rate;
        r = ma_audio_buffer_init(&bc, buf);
        if (r == MA_SUCCESS)
            r = ma_sound_init_from_data_source(m_engine.get(), buf, flags, group, snd);
    } else {
        flags |= sd.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;  // DECODE: данные берутся из кэша
#ifdef _WIN32
        r = ma_sound_init_from_file_w(m_engine.get(), sd.path.wstring().c_str(), flags, group, nullptr, snd);
#else
        r = ma_sound_init_from_file(m_engine.get(), sd.path.c_str(), flags, group, nullptr, snd);
#endif
    }
    if (r != MA_SUCCESS) {
        if (buf) {
            ma_audio_buffer_uninit(buf);
            delete buf;
        }
        delete snd;
        return false;
    }
    ma_sound_set_volume(snd, std::max(0.0f, p.volume));
    ma_sound_set_pitch(snd, std::max(0.01f, p.pitch));
    ma_sound_set_looping(snd, p.loop ? MA_TRUE : MA_FALSE);
    if (spatial) {
        ma_sound_set_attenuation_model(snd, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(snd, std::max(0.01f, p.minDistance));
        ma_sound_set_max_distance(snd, std::max(p.minDistance, p.maxDistance));
        ma_sound_set_doppler_factor(snd, 0.0f);  // без эффекта Доплера: скорости у нас не передаются
        Vec3 pos = p.position;
        if (follow && p.follow.Valid())  // сущность из этой же стадии ещё не создана — позицию даст Update
            pos = p.follow.Position();
        ma_sound_set_position(snd, pos.x, pos.y, pos.z);
    }
    if (p.fadeIn > 0)
        ma_sound_set_fade_in_milliseconds(snd, 0.0f, 1.0f, Ms(p.fadeIn));
    ma_sound_start(snd);

    if (m_free.empty()) {
        m_free.push_back(static_cast<uint32_t>(m_voices.size()));
        m_voices.emplace_back();
    }
    index = m_free.back();
    m_free.pop_back();
    VoiceSlot& v = m_voices[index];
    v.sound = snd;
    v.buffer = buf;
    v.alive = true;
    v.music = music;
    v.follow = follow;
    v.followSeen = false;
    v.followWait = 0;
    v.entity = Access::Index(p.follow);
    v.entityGen = Access::Gen(p.follow);
    gen = v.gen;
    return true;
}

bool AudioSystem::Play(uint32_t sound, const SoundParams& p, uint32_t& index, uint32_t& gen) {
    std::lock_guard lk(m_mutex);
    return PlayLocked(sound, p, index, gen, false);
}

bool AudioSystem::Playing(uint32_t index, uint32_t gen) {
    std::lock_guard lk(m_mutex);
    VoiceSlot* v = Find(index, gen);
    return v && ma_sound_is_playing(v->sound);
}

void AudioSystem::SetVolume(uint32_t index, uint32_t gen, float vol) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* v = Find(index, gen))
        ma_sound_set_volume(v->sound, std::max(0.0f, vol));
}

void AudioSystem::SetPitch(uint32_t index, uint32_t gen, float pitch) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* v = Find(index, gen))
        ma_sound_set_pitch(v->sound, std::max(0.01f, pitch));
}

void AudioSystem::SetPosition(uint32_t index, uint32_t gen, Vec3 p) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* v = Find(index, gen))
        ma_sound_set_position(v->sound, p.x, p.y, p.z);
}

void AudioSystem::StopLocked(VoiceSlot& v, float fadeSeconds) {
    if (fadeSeconds > 0)
        ma_sound_stop_with_fade_in_milliseconds(v.sound, Ms(fadeSeconds));
    else
        ma_sound_stop(v.sound);
    v.follow = false;  // уборка в Update: когда голос замолчит
}

void AudioSystem::Stop(uint32_t index, uint32_t gen, float fadeSeconds) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* v = Find(index, gen))
        StopLocked(*v, fadeSeconds);
}

void AudioSystem::StopAll(float fadeSeconds) {
    std::lock_guard lk(m_mutex);
    for (VoiceSlot& v : m_voices)
        if (v.alive && !v.music)
            StopLocked(v, fadeSeconds);
}

void AudioSystem::PlayMusic(uint32_t sound, float fade, bool loop, float volume) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* old = Find(m_musicIndex, m_musicGen))
        StopLocked(*old, fade);  // старая дорожка затихает, пока новая нарастает
    SoundParams p;
    p.volume = volume;
    p.loop = loop;
    p.group = AudioGroup::Music;
    p.fadeIn = fade;
    if (!PlayLocked(sound, p, m_musicIndex, m_musicGen, true))
        m_musicIndex = UINT32_MAX;
}

void AudioSystem::StopMusic(float fade) {
    std::lock_guard lk(m_mutex);
    if (VoiceSlot* v = Find(m_musicIndex, m_musicGen))
        StopLocked(*v, fade);
    m_musicIndex = UINT32_MAX;
}

void AudioSystem::SetMaster(float v) {
    std::lock_guard lk(m_mutex);
    m_master = std::max(0.0f, v);
    if (m_ok)
        ma_engine_set_volume(m_engine.get(), m_master);
}

float AudioSystem::Master() {
    std::lock_guard lk(m_mutex);
    return m_master;
}

void AudioSystem::SetGroup(AudioGroup g, float v) {
    const auto i = static_cast<size_t>(g);
    if (i >= m_groups.size())
        return;
    std::lock_guard lk(m_mutex);
    m_groupVolume[i] = std::max(0.0f, v);
    if (m_groups[i])
        ma_sound_group_set_volume(G(m_groups[i]), m_groupVolume[i]);
}

float AudioSystem::Group(AudioGroup g) {
    const auto i = static_cast<size_t>(g);
    std::lock_guard lk(m_mutex);
    return i < m_groupVolume.size() ? m_groupVolume[i] : 0.0f;
}

uint32_t AudioSystem::PlayingCount() {
    std::lock_guard lk(m_mutex);
    uint32_t n = 0;
    for (const VoiceSlot& v : m_voices)
        if (v.alive && ma_sound_is_playing(v.sound))
            ++n;
    return n;
}

void AudioSystem::Update(Vec3 camPos, Vec3 forward, Vec3 up, WorldImpl& world) {
    std::lock_guard lk(m_mutex);
    if (!m_ok)
        return;
    ma_engine_listener_set_position(m_engine.get(), 0, camPos.x, camPos.y, camPos.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);

    for (uint32_t i = 0; i < m_voices.size(); ++i) {
        VoiceSlot& v = m_voices[i];
        if (!v.alive)
            continue;
        if (v.follow) {
            if (const EntityData* d = world.TryGet(v.entity, v.entityGen)) {
                ma_sound_set_position(v.sound, d->pos.x, d->pos.y, d->pos.z);
                v.followSeen = true;
            } else if (v.followSeen || ++v.followWait > 2) {
                StopLocked(v, 0.05f);  // сущность удалили (или так и не появилась) — короткое затухание
            }
        }
        // Доиграл (или остановлен и затих) — освобождаем. Только что созданный голос уже играет.
        if (!ma_sound_is_playing(v.sound))
            Release(i);
    }
}

} // namespace ce::core
