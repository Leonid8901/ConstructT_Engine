#include "Core.h"
#include "Module/ModuleSystem.h"

namespace ce {

using core::Core;

namespace {
const core::Json* J(const void* p) { return static_cast<const core::Json*>(p); }
} // namespace

bool ModuleSettings::Has(const std::string& key) const { return m_json && J(m_json)->Has(key); }

float ModuleSettings::Number(const std::string& key, float fallback) const {
    if (!m_json)
        return fallback;
    const core::Json& v = (*J(m_json))[key];
    return v.IsNumber() ? static_cast<float>(v.AsNumber()) : fallback;
}

bool ModuleSettings::Bool(const std::string& key, bool fallback) const {
    if (!m_json)
        return fallback;
    const core::Json& v = (*J(m_json))[key];
    return v.IsBool() ? v.AsBool() : fallback;
}

std::string ModuleSettings::String(const std::string& key, const std::string& fallback) const {
    if (!m_json)
        return fallback;
    const core::Json& v = (*J(m_json))[key];
    return v.IsString() ? v.AsString() : fallback;
}

Module::~Module() = default;

ModuleSettings Module::Settings() const {
    return core::Access::MakeSettings(&Core().modules->Section(Name()));
}

TaskHandle Module::NewTask(std::string name, Cost cost, Priority priority, TaskFn fn) {
    TaskHandle h = UniX.CreateTask.NewTask(std::string(Name()) + "." + name, cost, priority, std::move(fn));
    m_tasks.push_back(h);
    return h;
}

Module* Module::RequireImpl(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make) {
    Module* m = Core().modules->Add(type, make, this);
    if (!m)
        throw core::ModuleDisabled(std::string("нужный ему модуль выключен в engine.json"));
    return m;
}

Module* ModulesApi::AddImpl(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make) const {
    return Core().modules->Add(type, make, nullptr);
}
Module* ModulesApi::GetImpl(const std::type_info& type) const { return Core().modules->Get(type); }
bool ModulesApi::RemoveImpl(const std::type_info& type) const { return Core().modules->Remove(type); }
Module* ModulesApi::Find(const std::string& name) const { return Core().modules->Find(name); }
uint32_t ModulesApi::Count() const { return Core().modules->Count(); }
std::vector<std::string> ModulesApi::Names() const { return Core().modules->Names(); }

} // namespace ce
