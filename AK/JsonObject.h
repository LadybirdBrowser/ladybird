/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/String.h>

namespace AK {

class JsonObject {
    template<typename Callback>
    using CallbackErrorType = decltype(declval<Callback>()(declval<String const&>(), declval<JsonValue const&>()).release_error());

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

    Optional<String const&> get_string(StringView key) const;

    Optional<JsonObject&> get_object(StringView key);
    Optional<JsonObject const&> get_object(StringView key) const;

    Optional<JsonArray&> get_array(StringView key);
    Optional<JsonArray const&> get_array(StringView key) const;

    Optional<double> get_double_with_precision_loss(StringView key) const;
    Optional<float> get_float_with_precision_loss(StringView key) const;

    void set(String key, JsonValue value);
    void set(StringView key, JsonValue value);

    template<typename Callback>
    void for_each_member(Callback callback) const
    {
        for (auto const& member : m_members)
            callback(member.key, member.value);
    }

    template<FallibleFunction<String const&, JsonValue const&> Callback>
    ErrorOr<void, CallbackErrorType<Callback>> try_for_each_member(Callback&& callback) const
    {
        for (auto const& member : m_members)
            TRY(callback(member.key, member.value));
        return {};
    }

    bool remove(StringView key);

    String serialized() const;
    void serialize(StringBuilder&) const;

private:
    OrderedHashMap<String, JsonValue> m_members;
};

}

#if USING_AK_GLOBALLY
using AK::JsonObject;
#endif
