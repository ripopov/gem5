/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __RTL_COSIM_RUNTIME_JSON_HH__
#define __RTL_COSIM_RUNTIME_JSON_HH__

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace gem5::rtl_cosim::json
{

class Value
{
  public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    explicit Value(bool value) : _value(value) {}
    explicit Value(std::string value) : _value(std::move(value)) {}
    explicit Value(Array value) : _value(std::move(value)) {}
    explicit Value(Object value) : _value(std::move(value)) {}

    static Value number(std::string spelling);

    bool isNull() const noexcept;
    bool isBool() const noexcept;
    bool isNumber() const noexcept;
    bool isString() const noexcept;
    bool isArray() const noexcept;
    bool isObject() const noexcept;
    bool asBool() const;
    const std::string &asNumber() const;
    const std::string &asString() const;
    const Array &asArray() const;
    const Object &asObject() const;
    const Value *find(std::string_view name) const noexcept;
    std::string serialize() const;

  private:
    struct Number
    {
        std::string spelling;
    };
    using Storage =
        std::variant<std::monostate, bool, Number, std::string, Array, Object>;
    explicit Value(Number value) : _value(std::move(value)) {}
    Storage _value;
};

bool parse(std::string_view input, Value &value, std::string &error);
bool unsignedInteger(const Value &value, std::uint64_t &result,
                     std::string &error);

} // namespace gem5::rtl_cosim::json

#endif // __RTL_COSIM_RUNTIME_JSON_HH__
