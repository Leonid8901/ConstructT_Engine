#include "UniX/GpuLanes.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"

namespace ce::core {

GpuLanes::GpuLanes(const VulkanContext& ctx) : m_device(ctx.Device()) {
    const QueueFamilies& qf = ctx.Families();
    m_graphicsFamily = qf.graphics;

    for (uint32_t family : ctx.UniqueFamilies()) {
        auto q = std::make_unique<QueueSlot>();
        q->family = family;
        vkGetDeviceQueue(m_device, family, 0, &q->queue);
        m_queues.push_back(std::move(q));

        auto p = std::make_unique<PoolSlot>();
        p->family = family;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family;
        VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &p->pool));
        m_pools.push_back(std::move(p));
    }

    auto find = [&](uint32_t family) {
        for (size_t i = 0; i < m_queues.size(); ++i)
            if (m_queues[i]->family == family)
                return i;
        return size_t{0};
    };
    uint32_t families[] = {qf.graphics, qf.compute, qf.transfer, qf.present};
    for (size_t i = 0; i < static_cast<size_t>(GpuLane::Count); ++i) {
        size_t idx = find(families[i]);
        m_lane[i] = m_queues[idx].get();
        m_lanePool[i] = m_pools[idx].get();
    }

    auto name = [&](GpuLane l) {
        return std::to_string(Family(l)) + (Dedicated(l) ? " (dedicated)" : " (shared)");
    };
    m_desc = "graphics=" + std::to_string(qf.graphics) + " compute=" + name(GpuLane::Compute) +
             " transfer=" + name(GpuLane::Transfer);
}

GpuLanes::~GpuLanes() {
    for (auto& q : m_queues) {
        std::lock_guard lk(q->mutex);
        vkQueueWaitIdle(q->queue);
    }
    for (auto& p : m_pools)
        vkDestroyCommandPool(m_device, p->pool, nullptr);
}

bool GpuLanes::Dedicated(GpuLane lane) const {
    return lane == GpuLane::Graphics || Family(lane) != m_graphicsFamily;
}

VkResult GpuLanes::Submit(GpuLane lane, const VkSubmitInfo2& info, VkFence fence) {
    QueueSlot* q = m_lane[static_cast<size_t>(lane)];
    m_submits[static_cast<size_t>(lane)].fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(q->mutex);
    return vkQueueSubmit2(q->queue, 1, &info, fence);
}

VkResult GpuLanes::Present(const VkPresentInfoKHR& info) {
    QueueSlot* q = m_lane[static_cast<size_t>(GpuLane::Present)];
    std::lock_guard lk(q->mutex);
    return vkQueuePresentKHR(q->queue, &info);
}

void GpuLanes::Immediate(GpuLane lane, const std::function<void(VkCommandBuffer)>& record) {
    PoolSlot* pool = m_lanePool[static_cast<size_t>(lane)];
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        std::lock_guard lk(pool->mutex);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        record(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
    }

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(m_device, &fi, nullptr, &fence));

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(Submit(lane, si, fence));
    VK_CHECK(vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(m_device, fence, nullptr);

    std::lock_guard lk(pool->mutex);
    vkFreeCommandBuffers(m_device, pool->pool, 1, &cmd);
}

} // namespace ce::core
