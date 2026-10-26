#include "Amper/MeshRegistry.h"
#include "Core.h"
#include "UniX/GpuLanes.h"
#include "Vulkan/VkCheck.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <map>
#include <tuple>
#include <sstream>
#include <stdexcept>

namespace ce::core {

MeshRegistry::MeshRegistry(Gpu& gpu, GpuLanes& lanes)
    : m_gpu(gpu), m_lanes(lanes), m_meshes(std::make_unique<MeshGpu[]>(kMaxMeshes)) {}

MeshRegistry::~MeshRegistry() {
    uint32_t n = Count();
    for (uint32_t i = 0; i < n; ++i) {
        MeshGpu& m = m_meshes[i];
        if (m.blas)
            m_gpu.Rt().destroyAs(m_gpu.Device(), m.blas, nullptr);
        m_gpu.Destroy(m.blasBuffer);
        m_gpu.Destroy(m.vertices);
        m_gpu.Destroy(m.indices);
        m_gpu.Destroy(m.skin);
    }
}

uint32_t MeshRegistry::Create(const Vertex* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount,
                              const SkinVertex* skin) {
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0)
        throw std::invalid_argument("Mesh::Create: пустой меш или число индексов не кратно 3");
    if (vertexCount > UINT32_MAX || indexCount > UINT32_MAX)
        throw std::invalid_argument("Mesh::Create: больше 2^32 вершин/индексов");
    // Индекс за пределами вершин — чтение чужой памяти на GPU (в лучшем случае мусор, в худшем device lost)
    for (size_t k = 0; k < indexCount; ++k)
        if (indices[k] >= vertexCount)
            throw std::invalid_argument("Mesh::Create: индекс " + std::to_string(indices[k]) + " >= числа вершин " +
                                        std::to_string(vertexCount));

    uint32_t id = m_count.load();
    // слот резервируется атомарно; меш станет видимым рендеру только после ready=true
    while (true) {
        if (id >= kMaxMeshes)
            throw std::runtime_error("Mesh limit reached");
        if (m_count.compare_exchange_weak(id, id + 1))
            break;
    }
    MeshGpu& mesh = m_meshes[id];

    const VkDeviceSize vbSize = vertexCount * sizeof(Vertex);
    const VkDeviceSize ibSize = indexCount * sizeof(uint32_t);
    // Адрес и storage — для вычислительного скиннинга (читает исходные вершины), build input — для BLAS
    VkBufferUsageFlags extra = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (m_gpu.RayTracing())
        extra |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    mesh.vertices = m_gpu.CreateBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                       MemUsage::GpuOnly, true);
    mesh.indices = m_gpu.CreateBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                      MemUsage::GpuOnly, true);
    mesh.vertexCount = static_cast<uint32_t>(vertexCount);
    mesh.indexCount = static_cast<uint32_t>(indexCount);

    // Ограничивающая сфера в локальных координатах (центр AABB) — для отсечения в Amper
    Vec3 lo = vertices[0].position, hi = vertices[0].position;
    for (size_t k = 1; k < vertexCount; ++k) {
        lo = Min(lo, vertices[k].position);
        hi = Max(hi, vertices[k].position);
    }
    const Vec3 center = (lo + hi) * 0.5f;
    float r2 = 0;
    for (size_t k = 0; k < vertexCount; ++k)
        r2 = std::max(r2, LengthSq(vertices[k].position - center));
    mesh.boundCenter = center;
    mesh.boundRadius = std::sqrt(r2);

    const VkDeviceSize skSize = skin ? vertexCount * sizeof(SkinVertex) : 0;
    if (skin)
        mesh.skin = m_gpu.CreateBuffer(skSize,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       MemUsage::GpuOnly, true);

    Buffer staging = m_gpu.CreateBuffer(vbSize + ibSize + skSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    std::memcpy(staging.mapped, vertices, vbSize);
    std::memcpy(static_cast<char*>(staging.mapped) + vbSize, indices, ibSize);
    if (skin)
        std::memcpy(static_cast<char*>(staging.mapped) + vbSize + ibSize, skin, skSize);

    m_lanes.Immediate(GpuLane::Transfer, [&](VkCommandBuffer cmd) {
        VkBufferCopy v{0, 0, vbSize};
        VkBufferCopy i{vbSize, 0, ibSize};
        vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &v);
        vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &i);
        if (skin) {
            VkBufferCopy k{vbSize + ibSize, 0, skSize};
            vkCmdCopyBuffer(cmd, staging.buffer, mesh.skin.buffer, 1, &k);
        }
    });
    m_gpu.Destroy(staging);

    if (m_gpu.RayTracing())
        BuildBlas(mesh);

    mesh.ready.store(true, std::memory_order_release);
    return id;
}

void MeshRegistry::BuildBlas(MeshGpu& mesh) {
    const RtFunctions& rt = m_gpu.Rt();
    VkDevice dev = m_gpu.Device();

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& tri = geom.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = mesh.vertices.address;
    tri.vertexStride = sizeof(Vertex);
    tri.maxVertex = mesh.vertexCount - 1;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = mesh.indices.address;

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    const uint32_t primitives = mesh.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.buildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);

    mesh.blasBuffer = m_gpu.CreateBuffer(sizes.accelerationStructureSize,
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         MemUsage::GpuOnly, true);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = mesh.blasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(rt.createAs(dev, &ci, nullptr, &mesh.blas));

    Buffer scratch = m_gpu.CreateBuffer(sizes.buildScratchSize,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        MemUsage::GpuOnly, false, m_gpu.ScratchAlignment());
    build.dstAccelerationStructure = mesh.blas;
    build.scratchData.deviceAddress = scratch.address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitives;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    m_lanes.Immediate(GpuLane::Compute, [&](VkCommandBuffer cmd) {
        // данные пришли из transfer-очереди: делаем их видимыми для сборки
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        rt.cmdBuild(cmd, 1, &build, &ranges);
    });
    m_gpu.Destroy(scratch);

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = mesh.blas;
    mesh.blasAddress = rt.asAddress(dev, &ai);
}

uint32_t MeshRegistry::Primitive(const std::string& key,
                                 const std::function<void(std::vector<Vertex>&, std::vector<uint32_t>&)>& gen) {
    std::lock_guard lk(m_primitiveMutex);
    auto it = m_primitives.find(key);
    if (it != m_primitives.end())
        return it->second;
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    gen(v, i);
    ComputeTangents(v, i);
    uint32_t id = Create(v.data(), v.size(), i.data(), i.size());
    m_primitives.emplace(key, id);
    return id;
}

// ---------------- генерация примитивов (CCW снаружи, нормали наружу) ----------------

void MeshRegistry::GenCube(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    struct Face { Vec3 n, u, w; };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const Face& f : faces) {
        uint32_t base = static_cast<uint32_t>(v.size());
        const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : s)
            v.push_back({(f.n + f.u * c[0] + f.w * c[1]) * 0.5f, f.n, {(c[0] + 1) * 0.5f, 1 - (c[1] + 1) * 0.5f}});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

namespace {

// Сетка «широта/долгота»; offsetY сдвигает верхнюю/нижнюю половину (для капсулы)
void GenLatLong(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float radius, float halfHeight,
                uint32_t segments, uint32_t rings) {
    const uint32_t half = rings / 2;
    std::vector<float> thetas;
    for (uint32_t r = 0; r <= half; ++r)
        thetas.push_back(kPi * 0.5f * r / half);
    if (halfHeight > 0)
        thetas.push_back(kPi * 0.5f);  // дубль экватора для цилиндрической части
    for (uint32_t r = 1; r <= half; ++r)
        thetas.push_back(kPi * 0.5f + kPi * 0.5f * r / half);

    const uint32_t rowCount = static_cast<uint32_t>(thetas.size());
    for (uint32_t r = 0; r < rowCount; ++r) {
        float th = thetas[r];
        bool top = halfHeight > 0 ? (r <= half) : (th <= kPi * 0.5f);
        for (uint32_t s = 0; s <= segments; ++s) {
            float ph = 2 * kPi * s / segments;
            Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            Vec3 p = n * radius;
            p.y += top ? halfHeight : -halfHeight;
            v.push_back({p, n, {static_cast<float>(s) / static_cast<float>(segments), th / kPi}});
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r + 1 < rowCount; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t a = r * stride + s, b = (r + 1) * stride + s, c = b + 1, d = a + 1;
            idx.insert(idx.end(), {a, d, c, a, c, b});
        }
}

} // namespace

void MeshRegistry::GenSphere(std::vector<Vertex>& v, std::vector<uint32_t>& idx, uint32_t segments) {
    segments = std::max(segments, 8u);
    GenLatLong(v, idx, 0.5f, 0.0f, segments, (segments / 2) & ~1u);
}

void MeshRegistry::GenCapsule(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float radius, float halfHeight,
                              uint32_t segments) {
    segments = std::max(segments, 8u);
    GenLatLong(v, idx, radius, halfHeight, segments, (segments / 2) & ~1u);
}

void MeshRegistry::GenPlane(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float size) {
    float h = size * 0.5f;
    Vec3 n{0, 1, 0};
    // UV в метрах: текстура повторяется каждый метр (масштаб — Material::uvScale)
    v = {{{-h, 0, -h}, n, {0, 0}}, {{-h, 0, h}, n, {0, size}}, {{h, 0, h}, n, {size, size}}, {{h, 0, -h}, n, {size, 0}}};
    idx = {0, 1, 2, 0, 2, 3};
}

// OBJ: v, vt, vn, f (треугольники и n-угольники веером; v, v/vt, v//vn, v/vt/vn; отрицательные индексы).
// Грань без нормалей получает плоскую нормаль. vt переворачивается по V (в OBJ начало снизу, у нас сверху).
bool MeshRegistry::LoadObj(const std::string& path, std::vector<Vertex>& out, std::vector<uint32_t>& idx) {
    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())));
    if (!file)
        return false;
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uvs;
    std::map<std::tuple<int, int, int>, uint32_t> cache;
    std::string line;
    std::vector<std::tuple<int, int, int>> face;
    auto toIndex = [](const std::string& s, size_t count) -> int {
        if (s.empty())
            return -1;
        int i = std::stoi(s);
        return i < 0 ? static_cast<int>(count) + i : i - 1;  // 1-based; -1 = последний
    };
    uint32_t lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        std::istringstream ss(line);
        ss.imbue(std::locale::classic());
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            Vec3 p; ss >> p.x >> p.y >> p.z; pos.push_back(p);
        } else if (tag == "vt") {
            Vec2 t; ss >> t.x >> t.y; uvs.push_back({t.x, 1.0f - t.y});
        } else if (tag == "vn") {
            Vec3 n; ss >> n.x >> n.y >> n.z; nrm.push_back(Normalize(n));
        } else if (tag == "f") {
            face.clear();
            std::string tok;
            while (ss >> tok) {
                const size_t s1 = tok.find('/');
                const size_t s2 = s1 == std::string::npos ? std::string::npos : tok.find('/', s1 + 1);
                const int vi = toIndex(tok.substr(0, s1), pos.size());
                const int ti = s1 == std::string::npos ? -1 : toIndex(tok.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1), uvs.size());
                const int ni = s2 == std::string::npos ? -1 : toIndex(tok.substr(s2 + 1), nrm.size());
                if (vi < 0 || vi >= static_cast<int>(pos.size()) || ti >= static_cast<int>(uvs.size()) ||
                    ni >= static_cast<int>(nrm.size()))
                    throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": индекс вне диапазона");
                face.emplace_back(vi, ti, ni);
            }
            bool flat = false;
            for (auto& f : face)
                flat |= std::get<2>(f) < 0;
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                const auto tri = {face[0], face[k], face[k + 1]};
                if (flat) {  // плоская нормаль; вершины этой грани не делятся с соседями
                    const Vec3 a = pos[std::get<0>(face[0])], b = pos[std::get<0>(face[k])], c = pos[std::get<0>(face[k + 1])];
                    const Vec3 n = Normalize(Cross(b - a, c - a));
                    for (auto key : tri) {
                        const int ti = std::get<1>(key);
                        out.push_back({pos[std::get<0>(key)], n, ti >= 0 ? uvs[ti] : Vec2{}});
                        idx.push_back(static_cast<uint32_t>(out.size() - 1));
                    }
                    continue;
                }
                for (auto key : tri) {
                    auto it = cache.find(key);
                    if (it == cache.end()) {
                        const int ti = std::get<1>(key);
                        out.push_back({pos[std::get<0>(key)], nrm[std::get<2>(key)], ti >= 0 ? uvs[ti] : Vec2{}});
                        it = cache.emplace(key, static_cast<uint32_t>(out.size() - 1)).first;
                    }
                    idx.push_back(it->second);
                }
            }
        }
    }
    return !idx.empty();
}

// Касательные по UV (накопление по треугольникам + ортогонализация Грама — Шмидта).
// Без UV или с вырожденными UV — любой перпендикуляр к нормали.
void MeshRegistry::ComputeTangents(std::vector<Vertex>& v, const std::vector<uint32_t>& idx) {
    std::vector<Vec3> tan(v.size()), bit(v.size());
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        const Vec3 e1 = v[b].position - v[a].position, e2 = v[c].position - v[a].position;
        const Vec2 d1 = v[b].uv - v[a].uv, d2 = v[c].uv - v[a].uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f)
            continue;
        const float r = 1.0f / det;
        const Vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {a, b, c}) {
            tan[k] += t;
            bit[k] += bt;
        }
    }
    for (size_t k = 0; k < v.size(); ++k) {
        const Vec3 n = v[k].normal;
        Vec3 t = tan[k] - n * Dot(n, tan[k]);
        if (LengthSq(t) < 1e-12f)  // нет UV: любой перпендикуляр
            t = Cross(n, std::fabs(n.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
        t = Normalize(t);
        const float w = Dot(Cross(n, t), bit[k]) < 0.0f ? -1.0f : 1.0f;
        v[k].tangent = {t, w};
    }
}

} // namespace ce::core
