/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace AK {

class JsonArray {
    template<typename Callback>
    using CallbackErrorType = decltype(declval<Callback>()(declval<JsonValue const&>()).release_error());

public:
    JsonArray() = default;
    ~JsonArray() = default;

    explicit JsonArray(size_t initial_size)
    {
        resize(initial_size);
    }

    JsonArray(JsonArray const& other)
        : m_values(other.m_values)
    {
    }

    JsonArray(JsonArray&& other)
        : m_values(move(other.m_values))
    {
    }

    template<IterableContainerOf<JsonValue> ContainerT>
    JsonArray(ContainerT&& source)
    {
        for (auto& value : source)
            m_values.append(move(value));
    }

    template<IterableContainerOf<JsonValue> ContainerT>
    JsonArray(ContainerT const& source)
    {
        for (auto const& value : source)
            m_values.append(value);
    }

    JsonArray& operator=(JsonArray const& other)
    {
        if (this != &other)
            m_values = other.m_values;
        return *this;
    }

    JsonArray& operator=(JsonArray&& other)
    {
        if (this != &other)
            m_values = move(other.m_values);
        return *this;
    }

    [[nodiscard]] size_t size() const { return m_values.size(); }
    [[nodiscard]] bool is_empty() const { return m_values.is_empty(); }

    [[nodiscard]] JsonValue const& at(size_t index) const { return m_values.at(index); }
    [[nodiscard]] JsonValue const& operator[](size_t index) const { return at(index); }

    [[nodiscard]] JsonValue take(size_t index) { return m_values.take(index); }

    void must_append(JsonValue value) { m_values.append(move(value)); }

    void clear() { m_values.clear(); }
    ErrorOr<void> append(JsonValue value) { return m_values.try_append(move(value)); }
    void set(size_t index, JsonValue value) { m_values.at(index) = move(value); }

    String serialized() const;
    void serialize(StringBuilder&) const;

    template<typename Callback>
    void for_each(Callback callback)
    {
        for (auto& value : m_values)
            callback(value);
    }

    template<typename Callback>
    void for_each(Callback callback) const
    {
        for (auto const& value : m_values)
            callback(value);
    }

    template<FallibleFunction<JsonValue&> Callback>
    ErrorOr<void, CallbackErrorType<Callback>> try_for_each(Callback&& callback)
    {
        for (auto& value : m_values)
            TRY(callback(value));
        return {};
    }

    template<FallibleFunction<JsonValue const&> Callback>
    ErrorOr<void, CallbackErrorType<Callback>> try_for_each(Callback&& callback) const
    {
        for (auto const& value : m_values)
            TRY(callback(value));
        return {};
    }

    [[nodiscard]] Vector<JsonValue>& values() { return m_values; }
    [[nodiscard]] Vector<JsonValue> const& values() const { return m_values; }

    void ensure_capacity(size_t capacity) { m_values.ensure_capacity(capacity); }
    void resize(size_t size) { m_values.resize(size); }

private:
    Vector<JsonValue> m_values;
};

}

#if USING_AK_GLOBALLY
using AK::JsonArray;
#endif
