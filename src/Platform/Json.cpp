#include "Platform/Json.h"

#include <charconv>

namespace ce::core {

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}

    bool Run(Json& out, std::string& error) {
        SkipWs();
        if (!Value(out, 0))
            return Fail(error);
        SkipWs();
        if (m_pos != m_s.size()) {
            m_error = "лишние символы после значения";
            return Fail(error);
        }
        return true;
    }

private:
    bool Fail(std::string& error) {
        uint32_t line = 1;
        for (size_t i = 0; i < m_pos && i < m_s.size(); ++i)
            line += m_s[i] == '\n';
        error = "строка " + std::to_string(line) + ": " + (m_error.empty() ? "ошибка разбора" : m_error);
        return false;
    }

    void SkipWs() {
        while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r'))
            ++m_pos;
    }

    bool Literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (m_s.compare(m_pos, n, word) != 0)
            return false;
        m_pos += n;
        return true;
    }

    bool Value(Json& v, int depth) {
        if (depth > 64) {
            m_error = "слишком глубокая вложенность";
            return false;
        }
        if (m_pos >= m_s.size()) {
            m_error = "неожиданный конец файла";
            return false;
        }
        const char c = m_s[m_pos];
        if (c == '{') return Object(v, depth);
        if (c == '[') return Array(v, depth);
        if (c == '"') {
            v.m_type = Json::Type::String;
            return String(v.m_string);
        }
        if (c == 't' && Literal("true")) { v.m_type = Json::Type::Bool; v.m_bool = true; return true; }
        if (c == 'f' && Literal("false")) { v.m_type = Json::Type::Bool; v.m_bool = false; return true; }
        if (c == 'n' && Literal("null")) { v.m_type = Json::Type::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(v);
        m_error = std::string("неожиданный символ '") + c + "'";
        return false;
    }

    bool Number(Json& v) {
        // from_chars не зависит от локали (strtod с русской локалью ждал бы запятую)
        const char* begin = m_s.data() + m_pos;
        const char* last = m_s.data() + m_s.size();
        double d = 0;
        auto [end, ec] = std::from_chars(begin, last, d);
        if (ec != std::errc() || end == begin) {
            m_error = "неверное число";
            return false;
        }
        m_pos += static_cast<size_t>(end - begin);
        v.m_type = Json::Type::Number;
        v.m_number = d;
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t& out) {
        if (m_pos + 4 > m_s.size())
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_s[m_pos++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') out |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') out |= static_cast<uint32_t>(h - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool String(std::string& out) {
        ++m_pos;  // "
        out.clear();
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_s.size())
                break;
            const char e = m_s[m_pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp)) {
                    m_error = "неверная \\u-последовательность";
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && m_pos + 6 <= m_s.size() && m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
                    m_pos += 2;
                    uint32_t lo;
                    if (!Hex4(lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                m_error = "неверная escape-последовательность";
                return false;
            }
        }
        m_error = "незакрытая строка";
        return false;
    }

    bool Array(Json& v, int depth) {
        ++m_pos;  // [
        v.m_type = Json::Type::Array;
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            Json item;
            SkipWs();
            if (!Value(item, depth + 1))
                return false;
            v.m_array.push_back(std::move(item));
            SkipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return true; }
            m_error = "ожидалась ',' или ']'";
            return false;
        }
    }

    bool Object(Json& v, int depth) {
        ++m_pos;  // {
        v.m_type = Json::Type::Object;
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != '"') {
                m_error = "ожидалось имя поля в кавычках";
                return false;
            }
            std::string key;
            if (!String(key))
                return false;
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') {
                m_error = "ожидалось ':' после \"" + key + "\"";
                return false;
            }
            ++m_pos;
            SkipWs();
            Json item;
            if (!Value(item, depth + 1))
                return false;
            v.m_object[key] = std::move(item);
            SkipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return true; }
            m_error = "ожидалась ',' или '}'";
            return false;
        }
    }

    const std::string& m_s;
    size_t m_pos = 0;
    std::string m_error;
};

bool Json::Parse(const std::string& text, Json& out, std::string& error) {
    out = Json{};
    JsonParser p(text);
    return p.Run(out, error);
}

const Json& Json::operator[](const std::string& key) const {
    static const Json kNull;
    if (m_type != Type::Object)
        return kNull;
    auto it = m_object.find(key);
    return it == m_object.end() ? kNull : it->second;
}

bool Json::Has(const std::string& key) const { return m_type == Type::Object && m_object.count(key) != 0; }

} // namespace ce::core
