#pragma once
// Минимальный JSON для engine.json: объекты, массивы, строки (с \uXXXX), числа, true/false/null.
// Без зависимостей; на ошибке разбора Parse возвращает false и текст ошибки с номером строки.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ce::core {

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static bool Parse(const std::string& text, Json& out, std::string& error);

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsObject() const { return m_type == Type::Object; }

    bool AsBool() const { return m_bool; }
    double AsNumber() const { return m_number; }
    const std::string& AsString() const { return m_string; }
    const std::vector<Json>& Items() const { return m_array; }

    // Поле объекта; для отсутствующего поля (или не-объекта) — пустое значение Null
    const Json& operator[](const std::string& key) const;
    bool Has(const std::string& key) const;

private:
    friend class JsonParser;
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<Json> m_array;
    std::map<std::string, Json> m_object;
};

} // namespace ce::core
