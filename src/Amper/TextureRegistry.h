#pragma once
// Реестр текстур: один bindless-массив на все текстуры (set = 1, binding = 0).
// Индекс текстуры лежит в данных инстанса, шейдер берёт textures[nonuniformEXT(i)].
// Загрузка синхронная через графический лейн UniX (копирование + генерация мипов блитами),
// безопасна из любого потока; дескриптор пишется после загрузки (update-after-bind).
#include "Vulkan/Gpu.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce::core {

class VulkanContext;
class GpuLanes;

class TextureRegistry {
public:
    static constexpr uint32_t kNone = UINT32_MAX;

    TextureRegistry(const VulkanContext& ctx, Gpu& gpu, GpuLanes& lanes);
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // RGBA8, строки сверху вниз. Бросает std::runtime_error, если места в массиве нет.
    uint32_t Create(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb);
    // PNG/JPEG/TGA/BMP/PSD/HDR из памяти (stb_image)
    uint32_t CreateFromMemory(const uint8_t* data, size_t size, bool srgb, const std::string& nameForErrors);
    // Файл; повторная загрузка того же пути с тем же srgb возвращает ту же текстуру
    uint32_t Load(const std::string& path, bool srgb);

    VkDescriptorSetLayout Layout() const { return m_layout; }
    VkDescriptorSet Set() const { return m_set; }
    uint32_t Count() const { return m_count.load(std::memory_order_acquire); }
    uint32_t Capacity() const { return m_capacity; }

private:
    struct Slot {
        Image image;
    };

    const VulkanContext& m_ctx;
    Gpu& m_gpu;
    GpuLanes& m_lanes;
    VkDevice m_device;
    uint32_t m_capacity = 0;
    std::unique_ptr<Slot[]> m_slots;
    std::atomic<uint32_t> m_count{0};

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    std::mutex m_writeMutex;  // запись в дескрипторный набор
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, uint32_t> m_cache;
};

} // namespace ce::core
