#include "Amper/ModelRegistry.h"
#include "Amper/MeshRegistry.h"
#include "Amper/TextureRegistry.h"
#include "Core.h"

#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace ce::core {

namespace {

std::filesystem::path U8Path(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }

bool ReadFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const auto size = static_cast<size_t>(f.tellg());
    out.resize(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(f) || size == 0;
}

// cgltf открывает внешние .bin через fopen (на Windows — ANSI, кириллица ломается): читаем сами
cgltf_result FileRead(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                      void** data) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(U8Path(path), bytes))
        return cgltf_result_file_not_found;
    void* mem = std::malloc(bytes.empty() ? 1 : bytes.size());
    if (!mem)
        return cgltf_result_out_of_memory;
    std::memcpy(mem, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = mem;
    return cgltf_result_success;
}
void FileRelease(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

const char* ResultText(cgltf_result r) {
    switch (r) {
    case cgltf_result_data_too_short: return "файл обрезан";
    case cgltf_result_unknown_format: return "неизвестный формат";
    case cgltf_result_invalid_json: return "ошибка JSON";
    case cgltf_result_invalid_gltf: return "некорректный glTF";
    case cgltf_result_file_not_found: return "файл не найден";
    case cgltf_result_io_error: return "ошибка чтения";
    case cgltf_result_out_of_memory: return "не хватило памяти";
    case cgltf_result_legacy_gltf: return "glTF 1.0 не поддерживается (нужен 2.0)";
    default: return "ошибка";
    }
}

Mat4 ToMat4(const float* m) {
    Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

// Загрузка одной модели: живёт на время вызова Load
class Loader {
public:
    Loader(const std::string& path, MeshRegistry& meshes, TextureRegistry& textures)
        : m_path(path), m_dir(U8Path(path).parent_path()), m_meshes(meshes), m_textures(textures) {}

    ~Loader() {
        if (m_data)
            cgltf_free(m_data);
    }

    void Run(ModelData& out) {
        std::vector<uint8_t> file;
        if (!ReadFile(U8Path(m_path), file))
            throw std::runtime_error("Model::Load: нет файла " + m_path);
        cgltf_options opt{};
        opt.file.read = FileRead;
        opt.file.release = FileRelease;
        cgltf_result r = cgltf_parse(&opt, file.data(), file.size(), &m_data);
        if (r == cgltf_result_success)
            r = cgltf_load_buffers(&opt, m_data, m_path.c_str());
        if (r == cgltf_result_success)
            r = cgltf_validate(m_data);
        if (r != cgltf_result_success)
            throw std::runtime_error("Model::Load: " + m_path + ": " + ResultText(r));
        m_opt = opt;

        out.path = m_path;
        LoadNodes(out);
        LoadSkins(out);
        LoadAnimations(out);
        const cgltf_scene* scene = m_data->scene ? m_data->scene : (m_data->scenes_count ? &m_data->scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
                Visit(scene->nodes[i], Mat4{}, out);
        } else {
            for (cgltf_size i = 0; i < m_data->nodes_count; ++i)
                if (!m_data->nodes[i].parent)
                    Visit(&m_data->nodes[i], Mat4{}, out);
        }
        if (out.parts.empty())
            throw std::runtime_error("Model::Load: " + m_path + ": нет треугольных мешей");
        if (m_skippedPrims)
            Log("Model " + m_path + ": пропущено " + std::to_string(m_skippedPrims) + " примитивов (не треугольники)");
        if (m_blend)
            Log("Model " + m_path + ": alphaMode BLEND не поддерживается — рисуется как MASK с порогом 0.5");
        if (m_otherUv)
            Log("Model " + m_path + ": текстуры с TEXCOORD_1+ берут TEXCOORD_0");

        out.boundsMin = Vec3(3.0e38f);
        out.boundsMax = Vec3(-3.0e38f);
        for (const ModelPart& p : out.parts) {
            const auto& b = m_meshBounds[p.mesh];
            for (int c = 0; c < 8; ++c) {
                const Vec3 corner{(c & 1) ? b.second.x : b.first.x, (c & 2) ? b.second.y : b.first.y,
                                  (c & 4) ? b.second.z : b.first.z};
                const Vec3 w = p.local.TransformPoint(corner);  // у частей со скином local = единичная
                out.boundsMin = Min(out.boundsMin, w);
                out.boundsMax = Max(out.boundsMax, w);
            }
        }
    }

private:
    uint32_t NodeIndex(const cgltf_node* n) const { return static_cast<uint32_t>(n - m_data->nodes); }

    // Все узлы файла (кости могут лежать вне сцены) + порядок обхода «родитель раньше детей»
    void LoadNodes(ModelData& out) {
        out.nodes.resize(m_data->nodes_count);
        for (cgltf_size i = 0; i < m_data->nodes_count; ++i) {
            const cgltf_node& n = m_data->nodes[i];
            ModelNode& dst = out.nodes[i];
            dst.parent = n.parent ? static_cast<int32_t>(NodeIndex(n.parent)) : -1;
            if (n.has_matrix) {
                dst.hasMatrix = true;
                dst.matrix = ToMat4(n.matrix);
            }
            if (n.has_translation) dst.t = {n.translation[0], n.translation[1], n.translation[2]};
            if (n.has_rotation) dst.r = Normalize(Quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]});
            if (n.has_scale) dst.s = {n.scale[0], n.scale[1], n.scale[2]};
        }
        std::vector<uint8_t> visited(out.nodes.size(), 0);
        std::vector<uint32_t> stack;
        for (uint32_t i = 0; i < out.nodes.size(); ++i) {
            if (out.nodes[i].parent >= 0)
                continue;
            stack.push_back(i);
            while (!stack.empty()) {
                const uint32_t k = stack.back();
                stack.pop_back();
                if (visited[k])
                    continue;  // защита от циклов в битом файле
                visited[k] = 1;
                out.order.push_back(k);
                const cgltf_node& n = m_data->nodes[k];
                for (cgltf_size c = 0; c < n.children_count; ++c)
                    stack.push_back(NodeIndex(n.children[c]));
            }
        }
    }

    void LoadSkins(ModelData& out) {
        out.skins.resize(m_data->skins_count);
        for (cgltf_size s = 0; s < m_data->skins_count; ++s) {
            const cgltf_skin& skin = m_data->skins[s];
            ModelSkin& dst = out.skins[s];
            dst.joints.resize(skin.joints_count);
            dst.inverseBind.resize(skin.joints_count);
            for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                dst.joints[j] = NodeIndex(skin.joints[j]);
                if (skin.inverse_bind_matrices) {
                    float m[16];
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, m, 16);
                    dst.inverseBind[j] = ToMat4(m);
                }
            }
        }
    }

    void LoadAnimations(ModelData& out) {
        for (cgltf_size a = 0; a < m_data->animations_count; ++a) {
            const cgltf_animation& anim = m_data->animations[a];
            AnimClip clip;
            clip.name = anim.name ? anim.name : "animation" + std::to_string(a);
            for (cgltf_size c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& ch = anim.channels[c];
                if (!ch.target_node || !ch.sampler)
                    continue;
                AnimChannel dst;
                switch (ch.target_path) {
                case cgltf_animation_path_type_translation: dst.path = AnimPath::Translation; break;
                case cgltf_animation_path_type_rotation: dst.path = AnimPath::Rotation; break;
                case cgltf_animation_path_type_scale: dst.path = AnimPath::Scale; break;
                default: continue;  // морфинг (weights) не поддерживается
                }
                dst.node = NodeIndex(ch.target_node);
                const cgltf_animation_sampler& sm = *ch.sampler;
                dst.interp = sm.interpolation == cgltf_interpolation_type_step          ? AnimInterp::Step
                             : sm.interpolation == cgltf_interpolation_type_cubic_spline ? AnimInterp::CubicSpline
                                                                                         : AnimInterp::Linear;
                const size_t keys = sm.input->count;
                dst.times.resize(keys);
                cgltf_accessor_unpack_floats(sm.input, dst.times.data(), keys);
                const size_t comps = dst.path == AnimPath::Rotation ? 4 : 3;
                const size_t perKey = dst.interp == AnimInterp::CubicSpline ? 3 : 1;
                if (keys == 0 || sm.output->count != keys * perKey)
                    continue;  // битый канал
                dst.values.resize(sm.output->count * comps);
                for (size_t k = 0; k < sm.output->count; ++k)
                    cgltf_accessor_read_float(sm.output, k, &dst.values[k * comps], comps);
                clip.duration = std::max(clip.duration, dst.times.back());
                clip.channels.push_back(std::move(dst));
            }
            out.clips.push_back(std::move(clip));
        }
    }

    void Visit(const cgltf_node* node, const Mat4& parent, ModelData& out) {
        float local[16];
        cgltf_node_transform_local(node, local);
        const Mat4 world = parent * ToMat4(local);
        if (node->mesh) {
            const cgltf_mesh* mesh = node->mesh;
            for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
                const cgltf_primitive& prim = mesh->primitives[p];
                if (prim.type != cgltf_primitive_type_triangles) {
                    ++m_skippedPrims;
                    continue;
                }
                ModelPart part;
                const bool skinned = node->skin && HasSkinAttributes(prim);
                part.skin = skinned ? static_cast<int32_t>(node->skin - m_data->skins) : -1;
                part.mesh = PrimitiveMesh(&prim, skinned ? node->skin : nullptr);
                part.material = ConvertMaterial(prim.material);
                part.local = skinned ? Mat4{} : world;  // вершины скина уже в координатах модели (через кости)
                part.node = NodeIndex(node);
                out.parts.push_back(part);
            }
        }
        for (cgltf_size i = 0; i < node->children_count; ++i)
            Visit(node->children[i], world, out);
    }

    static bool HasSkinAttributes(const cgltf_primitive& prim) {
        bool j = false, w = false;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            if (prim.attributes[a].index != 0)
                continue;
            j |= prim.attributes[a].type == cgltf_attribute_type_joints;
            w |= prim.attributes[a].type == cgltf_attribute_type_weights;
        }
        return j && w;
    }

    uint32_t PrimitiveMesh(const cgltf_primitive* prim, const cgltf_skin* skin) {
        auto it = m_primCache.find(prim);
        if (it != m_primCache.end())
            return it->second;

        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
            const cgltf_attribute& at = prim->attributes[a];
            if (at.type == cgltf_attribute_type_position) pos = at.data;
            else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
            else if (at.type == cgltf_attribute_type_tangent) tan = at.data;
            else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
            else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
        }
        if (!pos)
            throw std::runtime_error("Model::Load: " + m_path + ": примитив без POSITION");

        const size_t n = pos->count;
        std::vector<Vertex> v(n);
        for (size_t i = 0; i < n; ++i) {
            float f[4] = {};
            cgltf_accessor_read_float(pos, i, f, 3);
            v[i].position = {f[0], f[1], f[2]};
            if (nrm) {
                cgltf_accessor_read_float(nrm, i, f, 3);
                v[i].normal = Normalize(Vec3{f[0], f[1], f[2]});
            }
            if (uv) {
                cgltf_accessor_read_float(uv, i, f, 2);
                v[i].uv = {f[0], f[1]};
            }
            if (tan) {
                cgltf_accessor_read_float(tan, i, f, 4);
                v[i].tangent = {Normalize(Vec3{f[0], f[1], f[2]}), f[3] < 0 ? -1.0f : 1.0f};
            }
        }
        std::vector<SkinVertex> sk;
        if (skin && jnt && wgt) {
            sk.resize(n);
            const uint32_t jointCount = static_cast<uint32_t>(skin->joints_count);
            for (size_t i = 0; i < n; ++i) {
                cgltf_uint j[4] = {};
                float w[4] = {};
                cgltf_accessor_read_uint(jnt, i, j, 4);
                cgltf_accessor_read_float(wgt, i, w, 4);
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    if (j[k] >= jointCount || !(w[k] > 0)) {  // кость вне скина или NaN — вес в ноль
                        j[k] = 0;
                        w[k] = 0;
                    }
                    sum += w[k];
                }
                for (int k = 0; k < 4; ++k) {
                    sk[i].joints[k] = j[k];
                    sk[i].weights[k] = sum > 0 ? w[k] / sum : (k == 0 ? 1.0f : 0.0f);
                }
            }
        }
        std::vector<uint32_t> idx;
        if (prim->indices) {
            idx.resize(prim->indices->count);
            for (size_t i = 0; i < idx.size(); ++i)
                idx[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i));
        } else {
            idx.resize(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = static_cast<uint32_t>(i);
        }
        idx.resize(idx.size() / 3 * 3);

        if (!nrm) {  // по спецификации — плоские нормали: каждая вершина треугольника своя
            std::vector<Vertex> flat;
            std::vector<SkinVertex> flatSkin;
            flat.reserve(idx.size());
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                Vertex a = v[idx[i]], b = v[idx[i + 1]], c = v[idx[i + 2]];
                const Vec3 fn = Normalize(Cross(b.position - a.position, c.position - a.position));
                a.normal = b.normal = c.normal = fn;
                flat.push_back(a);
                flat.push_back(b);
                flat.push_back(c);
                if (!sk.empty())
                    for (size_t k = 0; k < 3; ++k)
                        flatSkin.push_back(sk[idx[i + k]]);
            }
            v.swap(flat);
            sk.swap(flatSkin);
            for (size_t i = 0; i < idx.size(); ++i)
                idx[i] = static_cast<uint32_t>(i);
            tan = nullptr;
        }
        if (!tan)
            MeshRegistry::ComputeTangents(v, idx);

        Vec3 lo = v[0].position, hi = v[0].position;
        for (const Vertex& vx : v) {
            lo = Min(lo, vx.position);
            hi = Max(hi, vx.position);
        }
        for (uint32_t i : idx)
            if (i >= v.size())
                throw std::runtime_error("Model::Load: " + m_path + ": индекс вершины вне диапазона");
        const uint32_t id = m_meshes.Create(v.data(), v.size(), idx.data(), idx.size(), sk.empty() ? nullptr : sk.data());
        m_meshBounds[id] = {lo, hi};
        m_primCache.emplace(prim, id);
        return id;
    }

    Texture TextureFor(const cgltf_texture_view& view, bool srgb) {
        if (!view.texture || !view.texture->image)
            return {};
        if (view.texcoord != 0)
            m_otherUv = true;
        const cgltf_image* img = view.texture->image;
        const auto key = std::make_pair(img, srgb);
        auto it = m_texCache.find(key);
        if (it != m_texCache.end())
            return Access::MakeTexture(it->second);

        uint32_t id = TextureRegistry::kNone;
        const std::string name = m_path + (img->name ? std::string(" / ") + img->name : "");
        try {
            if (img->buffer_view) {  // встроено в .glb
                const uint8_t* bytes = cgltf_buffer_view_data(img->buffer_view);
                id = m_textures.CreateFromMemory(bytes, img->buffer_view->size, srgb, name);
            } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {  // base64 в JSON
                const char* comma = std::strchr(img->uri, ',');
                if (!comma)
                    throw std::runtime_error("неверный data URI");
                const size_t len = std::strlen(comma + 1);
                size_t size = len / 4 * 3;
                if (len >= 1 && comma[len] == '=') --size;
                if (len >= 2 && comma[len - 1] == '=') --size;
                void* decoded = nullptr;
                if (cgltf_load_buffer_base64(&m_opt, size, comma + 1, &decoded) != cgltf_result_success)
                    throw std::runtime_error("не удалось декодировать base64");
                try {
                    id = m_textures.CreateFromMemory(static_cast<const uint8_t*>(decoded), size, srgb, name);
                } catch (...) {
                    std::free(decoded);
                    throw;
                }
                std::free(decoded);
            } else if (img->uri) {  // файл рядом с .gltf
                std::string uri = img->uri;
                uri.resize(cgltf_decode_uri(uri.data()));
                const std::filesystem::path file = m_dir / U8Path(uri);
                const auto u8 = file.u8string();
                id = m_textures.Load(std::string(u8.begin(), u8.end()), srgb);
            }
        } catch (const std::exception& e) {
            // Битая или отсутствующая картинка не роняет модель: материал без этой текстуры
            Log(std::string("Model ") + m_path + ": текстура пропущена — " + e.what());
            id = TextureRegistry::kNone;
        }
        m_texCache.emplace(key, id);
        return id == TextureRegistry::kNone ? Texture{} : Access::MakeTexture(id);
    }

    Material ConvertMaterial(const cgltf_material* m) {
        Material out;
        if (!m) {  // материал по умолчанию из спецификации
            out.color = {1, 1, 1};
            out.roughness = 1;
            out.metallic = 1;
            return out;
        }
        if (m->has_pbr_metallic_roughness) {
            const auto& pbr = m->pbr_metallic_roughness;
            out.color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]};
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            out.baseColorMap = TextureFor(pbr.base_color_texture, true);
            out.metallicRoughnessMap = TextureFor(pbr.metallic_roughness_texture, false);
        } else {
            out.color = {1, 1, 1};
            out.roughness = 1;
            out.metallic = 0;
        }
        out.normalMap = TextureFor(m->normal_texture, false);
        out.normalScale = m->normal_texture.texture ? m->normal_texture.scale : 1.0f;
        out.occlusionMap = TextureFor(m->occlusion_texture, false);
        out.occlusionStrength = m->occlusion_texture.texture ? m->occlusion_texture.scale : 1.0f;
        const float strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
        out.emissive = Vec3{m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]} * strength;
        out.emissiveMap = TextureFor(m->emissive_texture, true);  // фактор умножает текстуру (0 — выключено)
        out.doubleSided = m->double_sided;
        if (m->alpha_mode == cgltf_alpha_mode_mask) {
            out.alphaCutoff = std::max(m->alpha_cutoff, 1e-3f);
        } else if (m->alpha_mode == cgltf_alpha_mode_blend) {
            out.alphaCutoff = 0.5f;
            m_blend = true;
        }
        return out;
    }

    std::string m_path;
    std::filesystem::path m_dir;
    MeshRegistry& m_meshes;
    TextureRegistry& m_textures;
    cgltf_data* m_data = nullptr;
    cgltf_options m_opt{};
    std::map<const cgltf_primitive*, uint32_t> m_primCache;
    std::map<uint32_t, std::pair<Vec3, Vec3>> m_meshBounds;
    std::map<std::pair<const cgltf_image*, bool>, uint32_t> m_texCache;
    uint32_t m_skippedPrims = 0;
    bool m_blend = false, m_otherUv = false;
};

} // namespace

ModelRegistry::ModelRegistry(MeshRegistry& meshes, TextureRegistry& textures)
    : m_meshes(meshes), m_textures(textures), m_models(std::make_unique<std::atomic<ModelData*>[]>(kMaxModels)) {
    for (uint32_t i = 0; i < kMaxModels; ++i)
        m_models[i].store(nullptr);
}

ModelRegistry::~ModelRegistry() {
    for (uint32_t i = 0; i < kMaxModels; ++i)
        delete m_models[i].load();
}

uint32_t ModelRegistry::Load(const std::string& path) {
    {
        std::lock_guard lk(m_cacheMutex);
        auto it = m_cache.find(path);
        if (it != m_cache.end())
            return it->second;
    }
    auto data = std::make_unique<ModelData>();
    Loader loader(path, m_meshes, m_textures);
    loader.Run(*data);

    std::lock_guard lk(m_cacheMutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end())  // параллельно загрузили тот же файл: оставляем первую (лишние меши остаются в реестре)
        return it->second;
    const uint32_t id = m_count.fetch_add(1);
    if (id >= kMaxModels)
        throw std::runtime_error("Model: лимит моделей (" + std::to_string(kMaxModels) + ")");
    m_models[id].store(data.release(), std::memory_order_release);
    m_cache.emplace(path, id);
    Log("Model " + path + ": частей " + std::to_string(Get(id)->parts.size()));
    return id;
}

} // namespace ce::core
