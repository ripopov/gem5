/* Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl/runtime/json.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace gem5::rtl_cosim::json
{

Value
Value::number(std::string spelling)
{
    return Value(Number{std::move(spelling)});
}

bool
Value::isNull() const noexcept
{
    return _value.index() == 0;
}
bool
Value::isBool() const noexcept
{
    return _value.index() == 1;
}
bool
Value::isNumber() const noexcept
{
    return _value.index() == 2;
}
bool
Value::isString() const noexcept
{
    return _value.index() == 3;
}
bool
Value::isArray() const noexcept
{
    return _value.index() == 4;
}
bool
Value::isObject() const noexcept
{
    return _value.index() == 5;
}
bool
Value::asBool() const
{
    return std::get<bool>(_value);
}
const std::string &
Value::asNumber() const
{
    return std::get<Number>(_value).spelling;
}
const std::string &
Value::asString() const
{
    return std::get<std::string>(_value);
}
const Value::Array &
Value::asArray() const
{
    return std::get<Array>(_value);
}
const Value::Object &
Value::asObject() const
{
    return std::get<Object>(_value);
}

const Value *
Value::find(std::string_view name) const noexcept
{
    if (!isObject()) {
        return nullptr;
    }
    const auto it = asObject().find(name);
    return it == asObject().end() ? nullptr : &it->second;
}

namespace
{

void
serializeString(const std::string &input, std::string &output)
{
    static constexpr char Hex[] = "0123456789abcdef";
    output += '"';
    for (char character : input) {
        const auto ch = static_cast<unsigned char>(character);
        switch (ch) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output += Hex[ch >> 4];
                    output += Hex[ch & 0xf];
                } else {
                    output += static_cast<char>(ch);
                }
        }
    }
    output += '"';
}

} // anonymous namespace

std::string
Value::serialize() const
{
    std::string output;
    if (isNull()) {
        return "null";
    } else if (isBool()) {
        return asBool() ? "true" : "false";
    } else if (isNumber()) {
        return asNumber();
    } else if (isString()) {
        serializeString(asString(), output);
    } else if (isArray()) {
        output += '[';
        bool first = true;
        for (const Value &item : asArray()) {
            if (!first) {
                output += ',';
            }
            first = false;
            output += item.serialize();
        }
        output += ']';
    } else {
        output += '{';
        bool first = true;
        for (const auto &[name, item] : asObject()) {
            if (!first) {
                output += ',';
            }
            first = false;
            serializeString(name, output);
            output += ':';
            output += item.serialize();
        }
        output += '}';
    }
    return output;
}

namespace
{

class Parser
{
  public:
    Parser(std::string_view input, std::string &error)
        : _input(input), _error(error)
    {}

    bool
    parse(Value &value)
    {
        whitespace();
        if (!parseValue(value)) {
            return false;
        }
        whitespace();
        if (_position != _input.size()) {
            return fail("unexpected characters after the JSON value");
        }
        return true;
    }

  private:
    bool
    parseValue(Value &value)
    {
        if (_position == _input.size()) {
            return fail("expected a JSON value");
        }
        switch (_input[_position]) {
            case 'n':
                if (!literal("null")) {
                    return false;
                }
                value = Value();
                return true;
            case 't':
                if (!literal("true")) {
                    return false;
                }
                value = Value(true);
                return true;
            case 'f':
                if (!literal("false")) {
                    return false;
                }
                value = Value(false);
                return true;
            case '"': {
                std::string string;
                if (!parseString(string)) {
                    return false;
                }
                value = Value(std::move(string));
                return true;
            }
            case '[':
                return parseArray(value);
            case '{':
                return parseObject(value);
            default:
                return parseNumber(value);
        }
    }

    bool
    parseArray(Value &value)
    {
        ++_position;
        whitespace();
        Value::Array array;
        if (consume(']')) {
            value = Value(std::move(array));
            return true;
        }
        while (true) {
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            array.emplace_back(std::move(item));
            whitespace();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or ']' in array");
            }
            whitespace();
        }
        value = Value(std::move(array));
        return true;
    }

    bool
    parseObject(Value &value)
    {
        ++_position;
        whitespace();
        Value::Object object;
        if (consume('}')) {
            value = Value(std::move(object));
            return true;
        }
        while (true) {
            std::string name;
            if (!parseString(name)) {
                return false;
            }
            whitespace();
            if (!consume(':')) {
                return fail("expected ':' after object member name");
            }
            whitespace();
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            if (!object.emplace(name, std::move(item)).second) {
                return fail("duplicate object member '" + name + "'");
            }
            whitespace();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or '}' in object");
            }
            whitespace();
        }
        value = Value(std::move(object));
        return true;
    }

    static int
    hexDigit(char ch)
    {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    }

    bool
    parseString(std::string &value)
    {
        if (!consume('"')) {
            return fail("expected a JSON string");
        }
        while (_position < _input.size()) {
            char ch = _input[_position++];
            if (ch == '"') {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return fail("unescaped control character in string");
            }
            if (ch != '\\') {
                value += ch;
                continue;
            }
            if (_position == _input.size()) {
                return fail("unterminated string escape");
            }
            ch = _input[_position++];
            switch (ch) {
                case '"':
                    value += '"';
                    break;
                case '\\':
                    value += '\\';
                    break;
                case '/':
                    value += '/';
                    break;
                case 'b':
                    value += '\b';
                    break;
                case 'f':
                    value += '\f';
                    break;
                case 'n':
                    value += '\n';
                    break;
                case 'r':
                    value += '\r';
                    break;
                case 't':
                    value += '\t';
                    break;
                case 'u': {
                    if (_position + 4 > _input.size()) {
                        return fail("short Unicode escape");
                    }
                    unsigned code = 0;
                    for (unsigned i = 0; i < 4; ++i) {
                        const int digit = hexDigit(_input[_position++]);
                        if (digit < 0) {
                            return fail("invalid Unicode escape");
                        }
                        code = (code << 4) | static_cast<unsigned>(digit);
                    }
                    if (code <= 0x7f) {
                        value += static_cast<char>(code);
                    } else if (code <= 0x7ff) {
                        value += static_cast<char>(0xc0 | (code >> 6));
                        value += static_cast<char>(0x80 | (code & 0x3f));
                    } else if (code < 0xd800 || code > 0xdfff) {
                        value += static_cast<char>(0xe0 | (code >> 12));
                        value +=
                            static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                        value += static_cast<char>(0x80 | (code & 0x3f));
                    } else {
                        return fail(
                            "UTF-16 surrogate escapes are unsupported");
                    }
                    break;
                }
                default:
                    return fail("invalid string escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool
    parseNumber(Value &value)
    {
        const std::size_t start = _position;
        if (consume('-')) {}
        if (consume('0')) {
            if (_position < _input.size() &&
                std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                return fail("leading zero in number");
            }
        } else {
            if (_position == _input.size() ||
                !std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                return fail("expected a JSON value");
            }
            while (
                _position < _input.size() &&
                std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                ++_position;
            }
        }
        if (consume('.')) {
            if (_position == _input.size() ||
                !std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                return fail("missing digits after decimal point");
            }
            while (
                _position < _input.size() &&
                std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                ++_position;
            }
        }
        if (_position < _input.size() &&
            (_input[_position] == 'e' || _input[_position] == 'E')) {
            ++_position;
            if (_position < _input.size() &&
                (_input[_position] == '+' || _input[_position] == '-')) {
                ++_position;
            }
            if (_position == _input.size() ||
                !std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                return fail("missing exponent digits");
            }
            while (
                _position < _input.size() &&
                std::isdigit(static_cast<unsigned char>(_input[_position]))) {
                ++_position;
            }
        }
        value = Value::number(
            std::string(_input.substr(start, _position - start)));
        return true;
    }

    bool
    literal(std::string_view text)
    {
        if (_input.substr(_position, text.size()) != text) {
            return fail("invalid JSON literal");
        }
        _position += text.size();
        return true;
    }

    bool
    consume(char ch)
    {
        if (_position < _input.size() && _input[_position] == ch) {
            ++_position;
            return true;
        }
        return false;
    }

    void
    whitespace()
    {
        while (_position < _input.size() &&
               std::isspace(static_cast<unsigned char>(_input[_position]))) {
            ++_position;
        }
    }

    bool
    fail(const std::string &message)
    {
        const std::size_t line =
            1 + static_cast<std::size_t>(std::count(
                    _input.begin(),
                    _input.begin() + static_cast<std::ptrdiff_t>(_position),
                    '\n'));
        const std::size_t lineStart = _input.rfind('\n', _position);
        const std::size_t column =
            _position -
            (lineStart == std::string_view::npos ? 0 : lineStart + 1) + 1;
        _error = message + " at line " + std::to_string(line) + ", column " +
                 std::to_string(column);
        return false;
    }

    std::string_view _input;
    std::string &_error;
    std::size_t _position = 0;
};

} // anonymous namespace

bool
parse(std::string_view input, Value &value, std::string &error)
{
    error.clear();
    return Parser(input, error).parse(value);
}

bool
unsignedInteger(const Value &value, std::uint64_t &result, std::string &error)
{
    if (!value.isNumber()) {
        error = "expected an unsigned integer";
        return false;
    }
    const std::string &number = value.asNumber();
    if (number.empty() || number.front() == '-' ||
        number.find_first_of(".eE") != std::string::npos) {
        error = "expected an unsigned integer";
        return false;
    }
    const auto [end, code] =
        std::from_chars(number.data(), number.data() + number.size(), result);
    if (code != std::errc() || end != number.data() + number.size()) {
        error = "unsigned integer is out of range";
        return false;
    }
    return true;
}

} // namespace gem5::rtl_cosim::json
