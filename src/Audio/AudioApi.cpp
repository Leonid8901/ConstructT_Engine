// Публичный фасад звука: Sound, Voice, Audio
#include "Audio/AudioSystem.h"
#include "Core.h"

#include <filesystem>

namespace ce {

using core::Access;
using core::Core;

namespace {
core::AudioSystem& A() { return *Core().audio; }
}

// ---------------- Sound ----------------

Sound Sound::Load(const std::string& path, bool stream) {
    // Путь в UTF-8 -> path: на Windows ANSI сломал бы кириллицу
    const std::filesystem::path p(reinterpret_cast<const char8_t*>(path.c_str()));
    return Access::MakeSound(A().Load(p, stream));
}

Sound Sound::Create(const float* samples, uint32_t frames, uint32_t channels, uint32_t sampleRate) {
    return Access::MakeSound(A().Create(samples, frames, channels, sampleRate));
}

float Sound::Duration() const { return Valid() ? A().Duration(m_id) : 0.0f; }

// ---------------- Voice ----------------

bool Voice::Playing() const { return m_index != UINT32_MAX && A().Playing(m_index, m_gen); }

Voice& Voice::SetVolume(float volume) {
    A().SetVolume(m_index, m_gen, volume);
    return *this;
}

Voice& Voice::SetPitch(float pitch) {
    A().SetPitch(m_index, m_gen, pitch);
    return *this;
}

Voice& Voice::SetPosition(Vec3 position) {
    A().SetPosition(m_index, m_gen, position);
    return *this;
}

void Voice::Stop(float fadeSeconds) { A().Stop(m_index, m_gen, fadeSeconds); }

// ---------------- Audio ----------------

bool AudioApi::Available() const { return A().Available(); }

Voice AudioApi::Play(Sound sound, const SoundParams& params) const {
    uint32_t index = UINT32_MAX, gen = 0;
    if (!sound.Valid() || !A().Play(Access::Id(sound), params, index, gen))
        return {};
    return Access::MakeVoice(index, gen);
}

Voice AudioApi::PlayAt(Sound sound, Vec3 position, float volume) const {
    SoundParams p;
    p.volume = volume;
    p.spatial = true;
    p.position = position;
    return Play(sound, p);
}

Voice AudioApi::PlayOn(Sound sound, Entity entity, float volume, bool loop) const {
    SoundParams p;
    p.volume = volume;
    p.loop = loop;
    p.spatial = true;
    p.follow = entity;
    return Play(sound, p);
}

void AudioApi::PlayMusic(Sound music, float fadeSeconds, bool loop, float volume) const {
    if (music.Valid())
        A().PlayMusic(Access::Id(music), fadeSeconds, loop, volume);
}

void AudioApi::StopMusic(float fadeSeconds) const { A().StopMusic(fadeSeconds); }
void AudioApi::SetMasterVolume(float volume) const { A().SetMaster(volume); }
float AudioApi::MasterVolume() const { return A().Master(); }
void AudioApi::SetGroupVolume(AudioGroup group, float volume) const { A().SetGroup(group, volume); }
float AudioApi::GroupVolume(AudioGroup group) const { return A().Group(group); }
void AudioApi::StopAll(float fadeSeconds) const { A().StopAll(fadeSeconds); }
uint32_t AudioApi::PlayingCount() const { return A().PlayingCount(); }

} // namespace ce
