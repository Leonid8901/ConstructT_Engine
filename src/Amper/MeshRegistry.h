#pragma once
// Реестр мешей: загрузка через transfer-лейн UniX, BLAS для ray tracing — через compute-лейн.
// Слоты фиксированы и не двигаются: рендер читает их без блокировок (флаг ready).
#include "CE/World.h"
#include "Vulkan/Gpu.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce::core {

class GpuLanes;

// Привязка вершины к костям (совпадает со struct SkinVertex в shaders/skin.comp)
struct SkinVertex {
    uint32_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {1, 0, 0, 0};
};

struct MeshGpu {
    Buffer vertices;
    Buffer indices;
    Buffer skin;             // SkinVertex на вершину; только у мешей со скелетом
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    Vec3 boundCenter{};      // ограничивающая сфера в локальных координатах меша
    float boundRadius = 0;
    Buffer blasBuffer;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkDeviceAddress blasAddress = 0;
    std::atomic<bool> ready{false};
};

class MeshRegistry {
public:
    static constexpr uint32_t kMaxMeshes = 4096;

    MeshRegistry(Gpu& gpu, GpuLanes& lanes);
    ~MeshRegistry();

    // skin — по SkinVertex на вершину (или nullptr). Меш со скином рисуется через вычислительный скиннинг.
    uint32_t Create(const Vertex* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount,
                    const SkinVertex* skin = nullptr);
    uint32_t Primitive(const std::string& key, const std::function<void(std::vector<Vertex>&, std::vector<uint32_t>&)>& gen);

    uint32_t Count() const { return m_count.load(std::memory_order_acquire); }
    const MeshGpu* Get(uint32_t id) const {
        if (id >= Count()) return nullptr;
        const MeshGpu& m = m_meshes[id];
        return m.ready.load(std::memory_order_acquire) ? &m : nullptr;
    }

    static void GenCube(std::vector<Vertex>& v, std::vector<uint32_t>& i);
    static void GenSphere(std::vector<Vertex>& v, std::vector<uint32_t>& i, uint32_t segments);
    static void GenPlane(std::vector<Vertex>& v, std::vector<uint32_t>& i, float size);
    static void GenCapsule(std::vector<Vertex>& v, std::vector<uint32_t>& i, float radius, float halfHeight, uint32_t segments);
    static bool LoadObj(const std::string& path, std::vector<Vertex>& v, std::vector<uint32_t>& i);
    static void ComputeTangents(std::vector<Vertex>& v, const std::vector<uint32_t>& i);

private:
    void BuildBlas(MeshGpu& mesh);

    Gpu& m_gpu;
    GpuLanes& m_lanes;
    std::unique_ptr<MeshGpu[]> m_meshes;
    std::atomic<uint32_t> m_count{0};
    std::mutex m_primitiveMutex;
    std::unordered_map<std::string, uint32_t> m_primitives;
};

} // namespace ce::core
