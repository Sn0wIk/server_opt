#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace opt {

// A JSON writer only. External JSON is never parsed with this type.
class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool value) : value_(value) {}
    template<typename T, std::enable_if_t<std::is_integral_v<T> &&
                                         !std::is_same_v<T, bool>, int> = 0>
    Json(T value) : value_(static_cast<int64_t>(value)) {}
    Json(double value) : value_(value) {
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite JSON number");
    }
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    Json& operator[](const std::string& key) { return std::get<Object>(value_)[key]; }
    const Json& at(const std::string& key) const { return std::get<Object>(value_).at(key); }
    void push(Json value) { std::get<Array>(value_).push_back(std::move(value)); }

    std::string dump() const {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        write(stream, 0);
        return stream.str();
    }

private:
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value_;

    static void quote(std::ostream& out, const std::string& value) {
        static constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (unsigned char ch : value) {
            if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
            else if (ch < 0x20) out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
            else out << static_cast<char>(ch);
        }
        out << '"';
    }
    void write(std::ostream& out, int level) const {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) out << "null";
            else if constexpr (std::is_same_v<T, bool>) out << (value ? "true" : "false");
            else if constexpr (std::is_same_v<T, std::string>) quote(out, value);
            else if constexpr (std::is_same_v<T, Array> || std::is_same_v<T, Object>) {
                constexpr bool object = std::is_same_v<T, Object>;
                out << (object ? '{' : '[');
                size_t index = 0;
                for (const auto& item : value) {
                    out << (index++ ? ",\n" : "\n") << std::string((level + 1) * 2, ' ');
                    if constexpr (object) {
                        quote(out, item.first);
                        out << ": ";
                        item.second.write(out, level + 1);
                    } else item.write(out, level + 1);
                }
                if (!value.empty()) out << '\n' << std::string(level * 2, ' ');
                out << (object ? '}' : ']');
            } else out << std::setprecision(12) << value;
        }, value_);
    }
};

} // namespace opt
