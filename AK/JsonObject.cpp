/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/StringBuilder.h>

namespace AK {

JsonObject::JsonObject() = default;
JsonObject::~JsonObject() = default;

JsonObject::JsonObject(JsonObject const& other)
    : m_members(other.m_members.clone().release_value_but_fixme_should_propagate_errors())
{
}

JsonObject::JsonObject(JsonObject&& other)
    : m_members(move(other.m_members))
{
}

JsonObject& JsonObject::operator=(JsonObject const& other)
{
    if (this != &other)
        m_members = other.m_members.clone().release_value_but_fixme_should_propagate_errors();
    return *this;
}

JsonObject& JsonObject::operator=(JsonObject&& other)
{
    if (this != &other)
        m_members = move(other.m_members);
    return *this;
}

size_t JsonObject::size() const
{
    return m_members.size();
}

bool JsonObject::is_empty() const
{
    return m_members.is_empty();
}

Optional<JsonValue&> JsonObject::get(StringView key)
{
    auto it = m_members.find(key);
    if (it == m_members.end())
        return {};
    return it->value;
}

Optional<JsonValue const&> JsonObject::get(StringView key) const
{
    auto it = m_members.find(key);
    if (it == m_members.end())
        return {};
    return it->value;
}

Optional<i8> JsonObject::get_i8(StringView key) const
{
    return get_integer<i8>(key);
}

Optional<u8> JsonObject::get_u8(StringView key) const
{
    return get_integer<u8>(key);
}

Optional<i16> JsonObject::get_i16(StringView key) const
{
    return get_integer<i16>(key);
}

Optional<u16> JsonObject::get_u16(StringView key) const
{
    return get_integer<u16>(key);
}

Optional<i32> JsonObject::get_i32(StringView key) const
{
    return get_integer<i32>(key);
}

Optional<u32> JsonObject::get_u32(StringView key) const
{
    return get_integer<u32>(key);
}

Optional<i64> JsonObject::get_i64(StringView key) const
{
    return get_integer<i64>(key);
}

Optional<u64> JsonObject::get_u64(StringView key) const
{
    return get_integer<u64>(key);
}

Optional<FlatPtr> JsonObject::get_addr(StringView key) const
{
    return get_integer<FlatPtr>(key);
}

Optional<bool> JsonObject::get_bool(StringView key) const
{
    auto maybe_value = get(key);
    if (maybe_value.has_value() && maybe_value->is_bool())
        return maybe_value->as_bool();
    return {};
}

Optional<String const&> JsonObject::get_string(StringView key) const
{
    if (auto value = get(key); value.has_value() && value->is_string())
        return value->as_string();
    return {};
}

Optional<JsonObject&> JsonObject::get_object(StringView key)
{
    auto maybe_value = get(key);
    if (maybe_value.has_value() && maybe_value->is_object())
        return maybe_value->as_object();
    return {};
}

Optional<JsonObject const&> JsonObject::get_object(StringView key) const
{
    auto maybe_value = get(key);
    if (maybe_value.has_value() && maybe_value->is_object())
        return maybe_value->as_object();
    return {};
}

Optional<JsonArray&> JsonObject::get_array(StringView key)
{
    auto maybe_value = get(key);
    if (maybe_value.has_value() && maybe_value->is_array())
        return maybe_value->as_array();
    return {};
}

Optional<JsonArray const&> JsonObject::get_array(StringView key) const
{
    auto maybe_value = get(key);
    if (maybe_value.has_value() && maybe_value->is_array())
        return maybe_value->as_array();
    return {};
}

Optional<double> JsonObject::get_double_with_precision_loss(StringView key) const
{
    auto maybe_value = get(key);
    if (maybe_value.has_value())
        return maybe_value->get_double_with_precision_loss();
    return {};
}

Optional<float> JsonObject::get_float_with_precision_loss(StringView key) const
{
    auto maybe_value = get(key);
    if (maybe_value.has_value())
        return maybe_value->get_float_with_precision_loss();
    return {};
}

bool JsonObject::has(StringView key) const
{
    return m_members.contains(key);
}

bool JsonObject::has_null(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_null();
}

bool JsonObject::has_bool(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_bool();
}

bool JsonObject::has_string(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_string();
}

bool JsonObject::has_i8(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<i8>();
}

bool JsonObject::has_u8(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<u8>();
}

bool JsonObject::has_i16(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<i16>();
}

bool JsonObject::has_u16(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<u16>();
}

bool JsonObject::has_i32(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<i32>();
}

bool JsonObject::has_u32(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<u32>();
}

bool JsonObject::has_i64(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<i64>();
}

bool JsonObject::has_u64(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_integer<u64>();
}

bool JsonObject::has_number(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_number();
}

bool JsonObject::has_array(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_array();
}

bool JsonObject::has_object(StringView key) const
{
    auto value = get(key);
    return value.has_value() && value->is_object();
}

void JsonObject::set(String key, JsonValue value)
{
    m_members.set(move(key), move(value));
}

void JsonObject::set(StringView key, JsonValue value)
{
    set(MUST(String::from_utf8(key)), move(value));
}

bool JsonObject::remove(StringView key)
{
    return m_members.remove(key);
}

String JsonObject::serialized() const
{
    StringBuilder builder;
    serialize(builder);

    return MUST(builder.to_string());
}

void JsonObject::serialize(StringBuilder& builder) const
{
    auto serializer = MUST(JsonObjectSerializer<>::try_create(builder));
    for_each_member([&](auto& key, auto& value) {
        MUST(serializer.add(key, value));
    });
    MUST(serializer.finish());
}

}
