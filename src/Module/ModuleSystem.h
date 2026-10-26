#pragma once
// Реестр модулей игры: порядок загрузки, зависимости, выгрузка. Все вызовы — из главного потока вне стадий
// (OnStart, OnShutdown, обработчики триггеров и таймеры); внутри из-за зависимостей допустима рекурсия.
#include "CE/Module.h"
#include "Platform/Json.h"

#include <mutex>
#include <stdexcept>
#include <typeindex>

namespace ce::core {

// Нужный модуль выключен игроком: модуль, который его затребовал, тоже не загружается (не ошибка).
struct ModuleDisabled : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ModuleSystem {
public:
    explicit ModuleSystem(Json config) : m_config(std::move(config)) {}
    ~ModuleSystem();

    Module* Add(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make, const Module* requester);
    Module* Get(const std::type_info& type);
    bool Remove(const std::type_info& type);
    Module* Find(const std::string& name);
    uint32_t Count();
    std::vector<std::string> Names();
    void UnloadAll();

    const Json& Section(const std::string& name) const { return m_config[name]; }

private:
    struct Entry {
        std::type_index type;
        std::unique_ptr<Module> module;
        std::string name;
        std::vector<std::type_index> deps;  // модули, которые этот затребовал через Require
        bool loading = true;
    };
    Entry* FindEntry(const std::type_info& type);
    Entry* FindEntry(const Module* module);
    void Unload(size_t index);

    Json m_config;
    std::recursive_mutex m_mutex;
    std::vector<std::unique_ptr<Entry>> m_entries;  // загруженные, в порядке загрузки (зависимость всегда раньше зависимого)
    std::vector<Entry*> m_loading;                  // у которых сейчас идёт OnLoad (для циклов и поиска по Module*)
};

} // namespace ce::core
