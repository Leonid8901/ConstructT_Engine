#include "Module/ModuleSystem.h"

#include "Core.h"

#include <algorithm>

namespace ce::core {

namespace {
const char* NameOf(const Module& m) {
    const char* n = m.Name();
    return n ? n : "";
}
} // namespace

ModuleSystem::~ModuleSystem() {
    try {
        UnloadAll();
    } catch (...) {
    }
}

ModuleSystem::Entry* ModuleSystem::FindEntry(const std::type_info& type) {
    for (auto& e : m_entries)
        if (e->type == std::type_index(type))
            return e.get();
    for (Entry* e : m_loading)
        if (e->type == std::type_index(type))
            return e;
    return nullptr;
}

ModuleSystem::Entry* ModuleSystem::FindEntry(const Module* module) {
    for (auto& e : m_entries)
        if (e->module.get() == module)
            return e.get();
    for (Entry* e : m_loading)
        if (e->module.get() == module)
            return e;
    return nullptr;
}

Module* ModuleSystem::Add(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make,
                          const Module* requester) {
    std::lock_guard lk(m_mutex);
    auto noteDependency = [&] {
        if (!requester)
            return;
        if (Entry* r = FindEntry(requester)) {
            const std::type_index t(type);
            if (std::find(r->deps.begin(), r->deps.end(), t) == r->deps.end())
                r->deps.push_back(t);
        }
    };

    if (Entry* existing = FindEntry(type)) {
        if (existing->loading)
            throw std::runtime_error(std::string("Модули: циклическая зависимость вокруг «") + existing->name + "»");
        noteDependency();
        return existing->module.get();
    }

    std::unique_ptr<Module> module = make();
    if (!module)
        throw std::runtime_error("Модули: фабрика вернула пустой модуль");
    const std::string name = NameOf(*module);
    if (name.empty())
        throw std::runtime_error("Модули: у модуля нет имени (Name())");
    if (Find(name))
        throw std::runtime_error("Модули: имя «" + name + "» уже занято другим модулем");

    const Json& section = m_config[name];
    if (section["enabled"].IsBool() && !section["enabled"].AsBool()) {
        Log("Модули: «" + name + "» выключен в engine.json");
        return nullptr;
    }

    auto entry = std::make_unique<Entry>(Entry{std::type_index(type), std::move(module), name, {}, true});
    Entry* raw = entry.get();
    m_loading.push_back(raw);
    noteDependency();

    auto forget = [&] {
        m_loading.erase(std::find(m_loading.begin(), m_loading.end(), raw));
        for (TaskHandle& h : raw->module->m_tasks)
            h.Kill();
        raw->module->m_tasks.clear();
    };
    try {
        raw->module->OnLoad();
    } catch (const ModuleDisabled& e) {
        forget();
        Log("Модули: «" + name + "» не загружен: " + e.what());
        return nullptr;
    } catch (const std::exception& e) {
        forget();
        throw std::runtime_error("Модули: «" + name + "»: " + e.what());
    } catch (...) {
        forget();
        throw;
    }
    m_loading.erase(std::find(m_loading.begin(), m_loading.end(), raw));
    raw->loading = false;
    raw->module->m_loaded = true;
    m_entries.push_back(std::move(entry));
    Log("Модули: «" + name + "» загружен");
    return raw->module.get();
}

Module* ModuleSystem::Get(const std::type_info& type) {
    std::lock_guard lk(m_mutex);
    for (auto& e : m_entries)
        if (e->type == std::type_index(type))
            return e->module.get();
    return nullptr;
}

Module* ModuleSystem::Find(const std::string& name) {
    std::lock_guard lk(m_mutex);
    for (auto& e : m_entries)
        if (e->name == name)
            return e->module.get();
    for (Entry* e : m_loading)
        if (e->name == name)
            return e->module.get();
    return nullptr;
}

uint32_t ModuleSystem::Count() {
    std::lock_guard lk(m_mutex);
    return static_cast<uint32_t>(m_entries.size());
}

std::vector<std::string> ModuleSystem::Names() {
    std::lock_guard lk(m_mutex);
    std::vector<std::string> out;
    for (auto& e : m_entries)
        out.push_back(e->name);
    return out;
}

void ModuleSystem::Unload(size_t index) {
    std::unique_ptr<Entry> e = std::move(m_entries[index]);
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    try {
        e->module->OnUnload();
    } catch (const std::exception& ex) {
        Log("Модули: «" + e->name + "».OnUnload: " + ex.what());
    } catch (...) {
        Log("Модули: «" + e->name + "».OnUnload бросил исключение");
    }
    for (TaskHandle& h : e->module->m_tasks)
        h.Kill();
    e->module->m_tasks.clear();
    e->module->m_loaded = false;
    Log("Модули: «" + e->name + "» выгружен");
}

bool ModuleSystem::Remove(const std::type_info& type) {
    std::lock_guard lk(m_mutex);
    if (!m_loading.empty() && FindEntry(type) && FindEntry(type)->loading)
        throw std::runtime_error("Модули: нельзя выгрузить модуль во время его OnLoad");
    size_t index = m_entries.size();
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i]->type == std::type_index(type))
            index = i;
    if (index == m_entries.size())
        return false;
    // зависимые (прямо и через цепочку) — они в списке позже; выгружаем с конца
    std::vector<std::type_index> gone{m_entries[index]->type};
    std::vector<size_t> order{index};
    for (size_t i = index + 1; i < m_entries.size(); ++i) {
        for (const auto& d : m_entries[i]->deps) {
            if (std::find(gone.begin(), gone.end(), d) != gone.end()) {
                gone.push_back(m_entries[i]->type);
                order.push_back(i);
                break;
            }
        }
    }
    for (size_t k = order.size(); k-- > 0;)
        Unload(order[k]);
    return true;
}

void ModuleSystem::UnloadAll() {
    std::lock_guard lk(m_mutex);
    while (!m_entries.empty())
        Unload(m_entries.size() - 1);
}

} // namespace ce::core
