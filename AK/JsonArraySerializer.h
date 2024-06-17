/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/Try.h>

namespace AK {

template<typename Builder>
inline constexpr bool IsLegacyBuilder = requires(Builder builder) { builder.try_append('\0'); };

template<typename Builder = void>
class JsonObjectSerializer;

template<typename Builder = void>
class JsonArraySerializer {
public:
    static ErrorOr<JsonArraySerializer> try_create(Builder& builder)
    {
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(builder.try_append('['));
        else
            TRY(builder.append('['));
        return JsonArraySerializer { builder };
    }

    JsonArraySerializer(JsonArraySerializer&& other)
        : m_builder(other.m_builder)
        , m_empty(other.m_empty)
        , m_finished(exchange(other.m_finished, true))
    {
    }

    JsonArraySerializer(JsonArraySerializer const&) = delete;

    ErrorOr<void> add(JsonValue const& value)
    {
        TRY(begin_item());
        value.serialize(m_builder);
        return {};
    }

    ErrorOr<void> add(StringView value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>) {
            TRY(m_builder.try_append('"'));
            TRY(m_builder.try_append_escaped_for_json(value));
            TRY(m_builder.try_append('"'));
        } else {
            TRY(m_builder.append('"'));
            TRY(m_builder.append_escaped_for_json(value));
            TRY(m_builder.append('"'));
        }
        return {};
    }

    ErrorOr<void> add(ByteString const& value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>) {
            TRY(m_builder.try_append('"'));
            TRY(m_builder.try_append_escaped_for_json(value));
            TRY(m_builder.try_append('"'));
        } else {
            TRY(m_builder.append('"'));
            TRY(m_builder.append_escaped_for_json(value));
            TRY(m_builder.append('"'));
        }
        return {};
    }

    ErrorOr<void> add(char const* value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>) {
            TRY(m_builder.try_append('"'));
            TRY(m_builder.try_append_escaped_for_json(value));
            TRY(m_builder.try_append('"'));
        } else {
            TRY(m_builder.append('"'));
            TRY(m_builder.append_escaped_for_json(value));
            TRY(m_builder.append('"'));
        }
        return {};
    }

    ErrorOr<void> add(bool value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_append(value ? "true"sv : "false"sv));
        else
            TRY(m_builder.append(value ? "true"sv : "false"sv));
        return {};
    }

    ErrorOr<void> add(int value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(unsigned value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(long value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(long unsigned value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(long long value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(long long unsigned value)
    {
        TRY(begin_item());
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<JsonArraySerializer<Builder>> add_array()
    {
        TRY(begin_item());
        return JsonArraySerializer::try_create(m_builder);
    }

    // Implemented in JsonObjectSerializer.h
    ErrorOr<JsonObjectSerializer<Builder>> add_object();

    ErrorOr<void> finish()
    {
        VERIFY(!m_finished);
        m_finished = true;
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_append(']'));
        else
            TRY(m_builder.append(']'));
        return {};
    }

private:
    explicit JsonArraySerializer(Builder& builder)
        : m_builder(builder)
    {
    }

    ErrorOr<void> begin_item()
    {
        VERIFY(!m_finished);
        if (!m_empty) {
            if constexpr (IsLegacyBuilder<Builder>)
                TRY(m_builder.try_append(','));
            else
                TRY(m_builder.append(','));
        }
        m_empty = false;
        return {};
    }

    Builder& m_builder;
    bool m_empty { true };
    bool m_finished { false };
};

// Template magic to allow for JsonArraySerializer<>::try_create(...) - Blame CxByte
template<>
struct JsonArraySerializer<void> {
    template<typename Builder>
    static ErrorOr<JsonArraySerializer<Builder>> try_create(Builder& builder)
    {
        return JsonArraySerializer<Builder>::try_create(builder);
    }
};

}

#if USING_AK_GLOBALLY
using AK::JsonArraySerializer;
#endif
