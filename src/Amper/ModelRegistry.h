#pragma once
// Реестр моделей glTF 2.0. Модель = набор частей (меш + материал + матрица узла в координатах модели).
// Меши и текстуры загружаются в общие реестры (MeshRegistry, TextureRegistry); модель хранит их индексы.
// Слоты фиксированы: рендер читает готовую модель без блокировок (указатель публикуется атомарно).
#include "CE/World.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce::core {

class MeshRegistry;
class TextureRegistry;

struct ModelPart {
    uint32_t mesh = UINT32_MAX;
    Material material;
    Mat4 local;           // матрица узла относительно корня модели (поза по умолчанию)
    uint32_t node = 0;
    int32_t skin = -1;    // >= 0: вершины ведут кости скина (локальная матрица не используется)
};

// Узел glTF: поза по умолчанию. Анимации меняют translation/rotation/scale.
struct ModelNode {
    int32_t parent = -1;
    Vec3 t{};
    Quat r{};
    Vec3 s{1, 1, 1};
    bool hasMatrix = false;  // узел задан матрицей (по спецификации такие не анимируются)
    Mat4 matrix;
};

struct ModelSkin {
    std::vector<uint32_t> joints;    // индексы узлов
    std::vector<Mat4> inverseBind;   // по кости
};

enum class AnimPath : uint8_t { Translation, Rotation, Scale };
enum class AnimInterp : uint8_t { Linear, Step, CubicSpline };

struct AnimChannel {
    uint32_t node = 0;
    AnimPath path = AnimPath::Translation;
    AnimInterp interp = AnimInterp::Linear;
    std::vector<float> times;   // по возрастанию
    std::vector<float> values;  // 3 или 4 float на ключ (CUBICSPLINE: in-tangent, value, out-tangent)
};

struct AnimClip {
    std::string name;
    float duration = 0;
    std::vector<AnimChannel> channels;
};

struct ModelData {
    std::string path;
    std::vector<ModelPart> parts;
    Vec3 boundsMin{}, boundsMax{};
    std::vector<ModelNode> nodes;
    std::vector<uint32_t> order;  // узлы в порядке «родитель раньше детей»
    std::vector<ModelSkin> skins;
    std::vector<AnimClip> clips;
    bool animated() const { return !skins.empty() || !clips.empty(); }
    int32_t FindClip(const std::string& name) const {
        for (size_t i = 0; i < clips.size(); ++i)
            if (clips[i].name == name)
                return static_cast<int32_t>(i);
        return -1;
    }
};

class ModelRegistry {
public:
    static constexpr uint32_t kMaxModels = 4096;

    ModelRegistry(MeshRegistry& meshes, TextureRegistry& textures);
    ~ModelRegistry();

    uint32_t Load(const std::string& path);  // повторный вызов с тем же путём — та же модель
    const ModelData* Get(uint32_t id) const {
        return id < kMaxModels ? m_models[id].load(std::memory_order_acquire) : nullptr;
    }

private:
    MeshRegistry& m_meshes;
    TextureRegistry& m_textures;
    std::unique_ptr<std::atomic<ModelData*>[]> m_models;
    std::atomic<uint32_t> m_count{0};
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, uint32_t> m_cache;
};

} // namespace ce::core
