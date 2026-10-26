#pragma once
// Модули: свои библиотеки поверх движка («скример», «поиск игрока», «инвентарь»). Модуль — класс со своим
// жизненным циклом, задачами UniX и настройками в engine.json; игра подключает его одной строкой и
// пользуется его методами. Движок про содержимое модуля ничего не знает: модуль собирается вместе с игрой
// (обычно это один заголовок или пара файлов) и пишется на том же API (World, Grave, Nav, Timer, ...).
//
//   class Jumpscare : public Module {
//   public:
//       const char* Name() const override { return "Jumpscare"; }
//       void OnLoad() override { cooldown = Settings().Number("cooldown", 8.0f); }
//       void Trigger() { ... }
//   };
//
//   void OnStart() override {
//       if (auto* scare = Modules.Add<Jumpscare>())   // nullptr — игрок выключил модуль в engine.json
//           scare->Trigger();
//   }
//
// Модули загружаются в порядке Add, выгружаются в обратном (при Modules.Remove, и все при выходе из игры).
// Модуль, который нужен другому (Require<T>()), загружается раньше него и выгружается позже. Подробнее —
// docs/Modules.md.
#include "CE/UniX.h"

#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace ce {

namespace core {
struct Access;
class ModuleSystem;
} // namespace core

// Раздел "modules": { "ИмяМодуля": { ... } } из engine.json. Нет раздела или поля — возвращается fallback.
class ModuleSettings {
public:
    bool Has(const std::string& key) const;
    float Number(const std::string& key, float fallback) const;
    bool Bool(const std::string& key, bool fallback) const;
    std::string String(const std::string& key, const std::string& fallback) const;

private:
    friend struct core::Access;
    const void* m_json = nullptr;
};

class Module {
public:
    virtual ~Module();
    virtual const char* Name() const = 0;  // уникальное; по нему ищется раздел engine.json и пишутся логи
    virtual void OnLoad() {}               // движок и зависимости уже готовы; бросили исключение — модуль не загружен
    virtual void OnUnload() {}             // задачи модуля к этому моменту ещё живы; после возврата они убираются
    bool Loaded() const { return m_loaded; }

protected:
    ModuleSettings Settings() const;
    // Задача UniX, которая живёт, пока жив модуль (имя получает префикс "Модуль.").
    TaskHandle NewTask(std::string name, Cost cost, Priority priority, TaskFn fn);
    // Другой модуль, без которого этот не работает: загружается, если ещё нет. Если он выключен в engine.json,
    // этот модуль тоже не загружается (Add вернёт nullptr). Зависимость от себя и циклы — ошибка.
    template <class T>
    T& Require() {
        return *static_cast<T*>(RequireImpl(typeid(T), [] { return std::unique_ptr<Module>(std::make_unique<T>()); }));
    }

private:
    friend class core::ModuleSystem;
    Module* RequireImpl(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make);
    std::vector<TaskHandle> m_tasks;
    bool m_loaded = false;
};

struct ModulesApi {
    // Создать модуль и загрузить его (OnLoad вызывается сразу, в вызывающем потоке). Аргументы уходят в конструктор.
    // Уже загруженный модуль того же типа возвращается как есть (аргументы игнорируются).
    // nullptr — модуль выключен в engine.json ("modules": { "Имя": { "enabled": false } }) или нужный ему выключен.
    template <class T, class... Args>
    T* Add(Args&&... args) const {
        return static_cast<T*>(AddImpl(typeid(T), [&] {
            return std::unique_ptr<Module>(std::make_unique<T>(std::forward<Args>(args)...));
        }));
    }
    template <class T>
    T* Get() const { return static_cast<T*>(GetImpl(typeid(T))); }
    template <class T>
    bool Remove() const { return RemoveImpl(typeid(T)); }  // вместе с модулями, которые от него зависят

    Module* Find(const std::string& name) const;
    uint32_t Count() const;
    std::vector<std::string> Names() const;  // в порядке загрузки

private:
    Module* AddImpl(const std::type_info& type, const std::function<std::unique_ptr<Module>()>& make) const;
    Module* GetImpl(const std::type_info& type) const;
    bool RemoveImpl(const std::type_info& type) const;
};
inline constexpr ModulesApi Modules{};

} // namespace ce
