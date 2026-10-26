#pragma once
// GPU-сторона UniX: лейны очередей. Каждый лейн отображается на реальную VkQueue:
// выделенная семья, если она есть у GPU, иначе фолбэк на более общую (transfer -> compute -> graphics).
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ce::core {

class VulkanContext;

enum class GpuLane : uint8_t { Graphics, Compute, Transfer, Present, Count };

class GpuLanes {
public:
    explicit GpuLanes(const VulkanContext& ctx);
    ~GpuLanes();

    GpuLanes(const GpuLanes&) = delete;
    GpuLanes& operator=(const GpuLanes&) = delete;

    uint32_t Family(GpuLane lane) const { return m_lane[static_cast<size_t>(lane)]->family; }
    bool Dedicated(GpuLane lane) const;

    // Потокобезопасно: VkQueue требует внешней синхронизации, лейн держит мьютекс очереди.
    VkResult Submit(GpuLane lane, const VkSubmitInfo2& info, VkFence fence);
    VkResult Present(const VkPresentInfoKHR& info);

    // Записать команды, отправить в лейн и дождаться выполнения (загрузки, сборка BLAS).
    void Immediate(GpuLane lane, const std::function<void(VkCommandBuffer)>& record);

    const std::string& Description() const { return m_desc; }
    uint64_t Submissions(GpuLane lane) const { return m_submits[static_cast<size_t>(lane)].load(); }

private:
    struct QueueSlot {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        std::mutex mutex;
    };
    struct PoolSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        uint32_t family = 0;
        std::mutex mutex;
    };

    VkDevice m_device;
    std::vector<std::unique_ptr<QueueSlot>> m_queues;
    std::vector<std::unique_ptr<PoolSlot>> m_pools;
    std::array<QueueSlot*, static_cast<size_t>(GpuLane::Count)> m_lane{};
    std::array<PoolSlot*, static_cast<size_t>(GpuLane::Count)> m_lanePool{};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(GpuLane::Count)> m_submits{};
    uint32_t m_graphicsFamily = 0;
    std::string m_desc;
};

} // namespace ce::core
