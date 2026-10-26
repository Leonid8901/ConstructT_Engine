#pragma once
// Сцена: сущности, меши, материалы, камера.
// Все записи из задач потокобезопасны: внутри стадий они откладываются и применяются
// в точке синхронизации кадра. Чтение отдаёт состояние на начало стадии.
#include "CE/Math.h"
#include "CE/Shapes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ce {

namespace core { struct Access; }

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv{};                    // текстурные координаты (0,0 — левый верх, как в glTF)
    Vec4 tangent{1, 0, 0, 1};     // xyz — касательная, w = ±1 (направление битангенса); Mesh::Create считает сам
};

// Текстура (PNG, JPEG, TGA, BMP, HDR→8 бит). Дескриптор: индекс в общем массиве текстур GPU.
class Texture {
public:
    Texture() = default;
    // srgb = true для цвета (baseColor, emissive), false для данных (нормали, roughness/metallic)
    static Texture Load(const std::string& path, bool srgb = true);
    static Texture Create(const uint8_t* rgba8, uint32_t width, uint32_t height, bool srgb = true);
    bool Valid() const { return m_id != UINT32_MAX; }
    uint32_t Id() const { return m_id; }

private:
    friend struct core::Access;
    uint32_t m_id = UINT32_MAX;
};

// PBR-материал (metallic/roughness, как в glTF). Текстуры необязательны: без текстуры берётся число,
// с текстурой — число умножается на текстуру.
struct Material {
    Vec3 color{0.8f, 0.8f, 0.8f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 emissive{};
    Texture baseColorMap{};          // RGB — цвет, A — прозрачность для alphaCutoff
    Texture metallicRoughnessMap{};  // G — roughness, B — metallic (как в glTF)
    Texture normalMap{};             // карта нормалей в касательном пространстве
    Texture emissiveMap{};
    Texture occlusionMap{};          // R — затенение окружения (ambient occlusion)
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.0f;      // > 0: пиксели с альфой ниже порога отбрасываются (листва, решётки)
    bool doubleSided = false;      // рисовать обе стороны треугольников
    Vec2 uvScale{1, 1};            // повтор текстур (для Mesh::Plane UV уже в метрах)
};

// Примитивы единичного размера: куб 1x1x1, сфера диаметром 1, плоскость size x size.
class Mesh {
public:
    Mesh() = default;
    static Mesh Cube();
    static Mesh Sphere(uint32_t segments = 32);
    static Mesh Plane(float size = 1.0f);
    static Mesh Capsule(float radius = 0.5f, float halfHeight = 0.5f, uint32_t segments = 24);
    // Касательные (Vertex::tangent) считаются по UV автоматически, если у всех вершин они по умолчанию
    static Mesh Create(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    static Mesh LoadOBJ(const std::string& path);

    bool Valid() const { return m_id != UINT32_MAX; }
    uint32_t Id() const { return m_id; }

private:
    friend struct core::Access;
    uint32_t m_id = UINT32_MAX;
};

// Модель glTF 2.0 (.gltf + .bin + картинки или один .glb): меши, PBR-материалы, текстуры, иерархия
// узлов. Иерархия запекается в «части» (меш + материал + локальная матрица); сущность с моделью рисует
// все части. Загрузка синхронная — тяжёлые модели удобно грузить из фоновой задачи (UniX Once Extreme).
class Model {
public:
    Model() = default;
    static Model Load(const std::string& path);

    bool Valid() const { return m_id != UINT32_MAX; }
    uint32_t Id() const { return m_id; }
    uint32_t PartCount() const;
    Vec3 BoundsMin() const;  // в координатах модели, поза по умолчанию
    Vec3 BoundsMax() const;

    // Анимации (скелетные и узлов) из glTF
    uint32_t AnimationCount() const;
    std::string AnimationName(uint32_t index) const;
    float AnimationDuration(uint32_t index) const;  // секунды

private:
    friend struct core::Access;
    uint32_t m_id = UINT32_MAX;
};

// Что делать с сущностью, вылетевшей за область мира (см. WorldApi::SetBounds / SetKillY).
enum class OutOfBounds : uint8_t {
    Default,  // только для сущности: действие по умолчанию из World.SetOutOfBoundsAction (для движущихся тел)
    None,     // не трогать
    Destroy,  // удалить сущность
    Respawn,  // вернуть в точку возврата: положение и поворот, нулевые скорости
};

class Entity {
public:
    Entity() = default;

    bool Valid() const;
    void Destroy();

    Vec3 Position() const;
    Quat Rotation() const;
    Vec3 Scale() const;
    Vec3 Forward() const;
    Entity& SetPosition(Vec3 p);
    Entity& SetRotation(Quat q);
    Entity& SetScale(Vec3 s);
    Entity& SetScale(float s) { return SetScale(Vec3(s)); }

    Entity& SetMesh(Mesh mesh);
    Entity& SetModel(Model model);  // вместо меша; материалы берутся из модели, Material сущности не используется

    // Анимации модели. fadeSeconds — плавный переход с того, что играло (0 — мгновенно).
    // Имя ищется, когда команда применяется, поэтому SetModel(m).PlayAnimation("Walk") в одной цепочке работает.
    Entity& PlayAnimation(const std::string& name, bool loop = true, float fadeSeconds = 0.2f);
    Entity& PlayAnimation(uint32_t index, bool loop = true, float fadeSeconds = 0.2f);
    Entity& StopAnimation(float fadeSeconds = 0.2f);  // плавно вернуться в позу по умолчанию
    Entity& SetAnimationSpeed(float speed);           // 1 — как в файле, 0 — пауза, < 0 — назад
    float AnimationTime() const;     // секунды с начала текущей анимации (на начало стадии)
    bool AnimationPlaying() const;   // false — ничего не играет или однократная анимация закончилась
    Entity& SetMaterial(const Material& m);
    Entity& SetColor(Vec3 c);
    Entity& SetVisible(bool v);

    // Поведение за границами мира. Без вызова сущность с динамическим/кинематическим телом получает действие
    // по умолчанию из World.SetOutOfBoundsAction, остальные сущности не трогаются.
    // Respawn без явной точки возвращает в положение, где сущность была при AddBody (или при включении Respawn).
    Entity& SetOutOfBounds(OutOfBounds action);
    Entity& SetRespawnPoint(Vec3 position, Quat rotation = Quat{});

    Entity& AddBody(const BodyDesc& desc);
    Entity& RemoveBody();
    bool HasBody() const;
    Vec3 Velocity() const;
    Entity& SetVelocity(Vec3 v);
    Entity& SetAngularVelocity(Vec3 w);
    Entity& AddForce(Vec3 f);
    Entity& AddImpulse(Vec3 j);

    // Для сущности-триггера (BodyDesc::trigger): кто вошёл в зону / вышел из неё. Вызывается в главном потоке
    // после физики; повторный вызов добавляет ещё один обработчик. Подробнее — docs/Triggers.md.
    Entity& OnTriggerEnter(std::function<void(Entity other)> fn);
    Entity& OnTriggerExit(std::function<void(Entity other)> fn);

    uint32_t Index() const { return m_index; }
    bool operator==(const Entity& o) const { return m_index == o.m_index && m_gen == o.m_gen; }
    bool operator!=(const Entity& o) const { return !(*this == o); }

private:
    friend struct core::Access;
    uint32_t m_index = UINT32_MAX;
    uint32_t m_gen = 0;
};

struct WorldApi {
    Entity Spawn() const;
    uint32_t Count() const;

    // Область мира. Сущность, центр которой вышел за неё, получает действие OutOfBounds
    // (проверка раз в кадр после физики). По умолчанию область выключена.
    void SetBounds(Vec3 min, Vec3 max) const;
    // Только нижняя граница по Y («KillZ» из Unreal; у нас Y вверх). Остальные границы не меняются.
    void SetKillY(float y) const;
    void ClearBounds() const;
    // Действие по умолчанию для сущностей с движущимися телами; по умолчанию Destroy. Допустимы None/Destroy/Respawn.
    void SetOutOfBoundsAction(OutOfBounds action) const;
};
inline constexpr WorldApi World{};

struct CameraApi {
    void SetPosition(Vec3 p) const;
    void LookAt(Vec3 target) const;
    void SetAngles(float yaw, float pitch) const;  // радианы; yaw=0 смотрит в -Z
    void SetFov(float degrees) const;
    float Fov() const;                             // градусы по вертикали, по умолчанию 60
    Vec3 Position() const;
    Vec3 Forward() const;
    Vec3 Right() const;
    float Yaw() const;
    float Pitch() const;
};
inline constexpr CameraApi Camera{};

} // namespace ce
