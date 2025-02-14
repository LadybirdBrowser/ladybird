/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/JsonValue.h>

namespace AK {

class JsonObject {
    template<typename Callback>
    using CallbackErrorType = decltype(declval<Callback>()(declval<ByteString const&>(), declval<JsonValue const&>()).release_error());

public:
    JsonObject();
    ~JsonObject();

    JsonObject(JsonObject const& other);
    JsonObject(JsonObject&& other);

    JsonObject& operator=(JsonObject const& other);
    JsonObject& operator=(JsonObject&& other);

    [[nodiscard]] size_t size() const;
    [[nodiscard]] bool is_empty() const;

    [[nodiscard]] bool has(StringView key) const;

    [[nodiscard]] bool has_null(StringView key) const;
    [[nodiscard]] bool has_bool(StringView key) const;
    [[nodiscard]] bool has_string(StringView key) const;
    [[nodiscard]] bool has_i8(StringView key) const;
    [[nodiscard]] bool has_u8(StringView key) const;
    [[nodiscard]] bool has_i16(StringView key) const;
    [[nodiscard]] bool has_u16(StringView key) const;
    [[nodiscard]] bool has_i32(StringView key) const;
    [[nodiscard]] bool has_u32(StringView key) const;
    [[nodiscard]] bool has_i64(StringView key) const;
    [[nodiscard]] bool has_u64(StringView key) const;
    [[nodiscard]] bool has_number(StringView key) const;
    [[nodiscard]] bool has_array(StringView key) const;
    [[nodiscard]] bool has_object(StringView key) const;

    Optional<JsonValue&> get(StringView key);
    Optional<JsonValue const&> get(StringView key) const;

    template<Integral T>
    Optional<T> get_integer(StringView key) const
    {
        auto maybe_value = get(key);
        if (maybe_value.has_value() && maybe_value->is_integer<T>())
            return maybe_value->as_integer<T>();
        return {};
    }

    Optional<i8> get_i8(StringView key) const;
    Optional<u8> get_u8(StringView key) const;
    Optional<i16> get_i16(StringView key) const;
    Optional<u16> get_u16(StringView key) const;
    Optional<i32> get_i32(StringView key) const;
    Optional<u32> get_u32(StringView key) const;
    Optional<i64> get_i64(StringView key) const;
    Optional<u64> get_u64(StringView key) const;
    Optional<FlatPtr> get_addr(StringView key) const;
    Optional<bool> get_bool(StringView key) const;

    Optional<ByteString> get_byte_string(StringView key) const;

    Optional<JsonObject&> get_object(StringView key);
    Optional<JsonObject const&> get_object(StringView key) const;

    Optional<JsonArray&> get_array(StringView key);
    Optional<JsonArray const&> get_array(StringView key) const;

    Optional<double> get_double_with_precision_loss(StringView key) const;
    Optional<float> get_float_with_precision_loss(StringView key) const;

    void set(ByteString const& key, JsonValue value);

    template<typename Callback>
    void for_each_member(Callback callback) const
    {
        for (auto const& member : m_members)
            callback(member.key, member.value);
    }

    template<FallibleFunction<ByteString const&, JsonValue const&> Callback>
    ErrorOr<void, CallbackErrorType<Callback>> try_for_each_member(Callback&& callback) const
    {
        for (auto const& member : m_members)
            TRY(callback(member.key, member.value));
        return {};
    }

    bool remove(StringView key);

    template<typename Builder>
    typename Builder::OutputType serialized() const;

    template<typename Builder>
    void serialize(Builder&) const;

    [[nodiscard]] ByteString to_byte_string() const;

private:
    OrderedHashMap<ByteString, JsonValue> m_members;
};

template<typename Builder>
inline void JsonObject::serialize(Builder& builder) const
{
    auto serializer = MUST(JsonObjectSerializer<>::try_create(builder));
    for_each_member([&](auto& key, auto& value) {
        MUST(serializer.add(key, value));
    });
    MUST(serializer.finish());
}

template<typename Builder>
inline typename Builder::OutputType JsonObject::serialized() const
{
    Builder builder;
    serialize(builder);
    return builder.to_byte_string();
}

template<typename Builder>
inline void JsonValue::serialize(Builder& builder) const
{
    m_value.visit(
        [&](Empty const&) { builder.append("null"sv); },
        [&](bool const& value) { builder.append(value ? "true"sv : "false"sv); },
        [&](Arithmetic auto const& value) { builder.appendff("{}", value); },
        [&](ByteString const& value) {
            builder.append('\"');
            builder.append_escaped_for_json(value.bytes());
            builder.append('\"');
        },
        [&](auto const& array_or_object) {
            array_or_object->serialize(builder);
        });
}

template<typename Builder>
inline typename Builder::OutputType JsonValue::serialized() const
{
    Builder builder;
    serialize(builder);
    return builder.to_byte_string();
}

}

#if USING_AK_GLOBALLY
using AK::JsonObject;
#endif
