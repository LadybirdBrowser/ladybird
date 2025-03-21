/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>

namespace AK {

static ErrorOr<JsonValue::Storage> clone(JsonValue::Storage const& other)
{
    return other.visit(
        [](NonnullOwnPtr<JsonArray> const& value) -> ErrorOr<JsonValue::Storage> {
            return TRY(try_make<JsonArray>(*value));
        },
        [](NonnullOwnPtr<JsonObject> const& value) -> ErrorOr<JsonValue::Storage> {
            return TRY(try_make<JsonObject>(*value));
        },
        [](auto const& value) -> ErrorOr<JsonValue::Storage> { return JsonValue::Storage(value); });
}

JsonValue::JsonValue() = default;
JsonValue::~JsonValue() = default;
JsonValue::JsonValue(JsonValue&&) = default;
JsonValue& JsonValue::operator=(JsonValue&&) = default;

JsonValue::JsonValue(JsonValue const& other)
    : m_value(MUST(clone(other.m_value)))
{
}

JsonValue& JsonValue::operator=(JsonValue const& other)
{
    if (this != &other)
        m_value = MUST(clone(other.m_value));
    return *this;
}

JsonValue& JsonValue::operator=(JsonArray const& other)
{
    return *this = JsonValue(other);
}

JsonValue& JsonValue::operator=(JsonArray&& other)
{
    return *this = JsonValue(other);
}

JsonValue& JsonValue::operator=(JsonObject const& other)
{
    return *this = JsonValue(other);
}

JsonValue& JsonValue::operator=(JsonObject&& other)
{
    return *this = JsonValue(other);
}

bool JsonValue::equals(JsonValue const& other) const
{
    if (is_null() && other.is_null())
        return true;

    if (is_bool() && other.is_bool() && as_bool() == other.as_bool())
        return true;

    if (is_string() && other.is_string() && as_string() == other.as_string())
        return true;

    if (is_number() && other.is_number()) {
        auto normalize = [](Variant<u64, i64, double> representation, bool& is_negative) {
            return representation.visit(
                [&](u64& value) -> Variant<u64, double> {
                    is_negative = false;
                    return value;
                },
                [&](i64& value) -> Variant<u64, double> {
                    is_negative = value < 0;
                    return static_cast<u64>(abs(value));
                },
                [&](double& value) -> Variant<u64, double> {
                    is_negative = value < 0;
                    value = abs(value);
                    if (static_cast<double>(static_cast<u64>(value)) == value)
                        return static_cast<u64>(value);
                    return value;
                });
        };
        bool is_this_negative;
        auto normalized_this = normalize(as_number(), is_this_negative);
        bool is_that_negative;
        auto normalized_that = normalize(other.as_number(), is_that_negative);
        return is_this_negative == is_that_negative && normalized_this == normalized_that;
    }

    if (is_array() && other.is_array() && as_array().size() == other.as_array().size()) {
        bool result = true;
        for (size_t i = 0; i < as_array().size(); ++i) {
            result &= as_array().at(i).equals(other.as_array().at(i));
        }
        return result;
    }

    if (is_object() && other.is_object() && as_object().size() == other.as_object().size()) {
        bool result = true;
        as_object().for_each_member([&](auto& key, auto& value) {
            auto other_value = other.as_object().get(key);
            if (other_value.has_value())
                result &= value.equals(*other_value);
            else
                result = false;
        });
        return result;
    }

    return false;
}

JsonValue::JsonValue(int value)
    : m_value(i64 { value })
{
}

JsonValue::JsonValue(unsigned value)
    : m_value(i64 { value })
{
}

JsonValue::JsonValue(long value)
    : m_value(i64 { value })
{
}

JsonValue::JsonValue(unsigned long value)
    : m_value(u64 { value })
{
}

JsonValue::JsonValue(long long value)
    : m_value(i64 { value })
{
}

JsonValue::JsonValue(long long unsigned value)
    : m_value(u64 { value })
{
}

JsonValue::JsonValue(double value)
    : m_value(value)
{
}

JsonValue::JsonValue(String value)
    : m_value(move(value))
{
}

JsonValue::JsonValue(StringView value)
    : m_value(MUST(String::from_utf8(value)))
{
}

JsonValue::JsonValue(JsonObject const& value)
    : m_value(make<JsonObject>(value))
{
}

JsonValue::JsonValue(JsonArray const& value)
    : m_value(make<JsonArray>(value))
{
}

JsonValue::JsonValue(JsonObject&& value)
    : m_value(make<JsonObject>(move(value)))
{
}

JsonValue::JsonValue(JsonArray&& value)
    : m_value(make<JsonArray>(move(value)))
{
}

ErrorOr<JsonValue> JsonValue::from_string(StringView input)
{
    return JsonParser::parse(input);
}

String JsonValue::serialized() const
{
    StringBuilder builder;
    serialize(builder);

    return MUST(builder.to_string());
}

void JsonValue::serialize(StringBuilder& builder) const
{
    m_value.visit(
        [&](Empty const&) { builder.append("null"sv); },
        [&](bool const& value) { builder.append(value ? "true"sv : "false"sv); },
        [&](Arithmetic auto const& value) { builder.appendff("{}", value); },
        [&](String const& value) {
            builder.append('\"');
            builder.append_escaped_for_json(value.bytes());
            builder.append('\"');
        },
        [&](auto const& array_or_object) {
            array_or_object->serialize(builder);
        });
}

}
