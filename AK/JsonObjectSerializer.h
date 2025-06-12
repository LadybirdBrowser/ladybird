/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonArraySerializer.h>
#include <AK/JsonValue.h>
#include <AK/Try.h>

namespace AK {

template<typename Builder>
class JsonObjectSerializer {
public:
    static ErrorOr<JsonObjectSerializer> try_create(Builder& builder)
    {
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(builder.try_append('{'));
        else
            TRY(builder.append('{'));
        return JsonObjectSerializer { builder };
    }

    JsonObjectSerializer(JsonObjectSerializer&& other)
        : m_builder(other.m_builder)
        , m_empty(other.m_empty)
        , m_finished(exchange(other.m_finished, true))
    {
    }

    JsonObjectSerializer(JsonObjectSerializer const&) = delete;

    ErrorOr<void> add(StringView key, JsonValue const& value)
    {
        TRY(begin_item(key));
        value.serialize(m_builder);
        return {};
    }

    ErrorOr<void> add(StringView key, StringView value)
    {
        TRY(begin_item(key));
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

    ErrorOr<void> add(StringView key, String const& value)
    {
        TRY(begin_item(key));
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

    ErrorOr<void> add(StringView key, char const* value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>) {
            TRY(m_builder.try_append('"'));
            TRY(m_builder.try_append_escaped_for_json({ value, __builtin_strlen(value) }));
            TRY(m_builder.try_append('"'));
        } else {
            TRY(m_builder.append('"'));
            TRY(m_builder.append_escaped_for_json({ value, __builtin_strlen(value) }));
            TRY(m_builder.append('"'));
        }
        return {};
    }

    ErrorOr<void> add(StringView key, bool value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_append(value ? "true"sv : "false"sv));
        else
            TRY(m_builder.append(value ? "true"sv : "false"sv));
        return {};
    }

    ErrorOr<void> add(StringView key, int value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, unsigned value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, long value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, long unsigned value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, long long value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, long long unsigned value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, float value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<void> add(StringView key, double value)
    {
        TRY(begin_item(key));
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_appendff("{}", value));
        else
            TRY(m_builder.appendff("{}", value));
        return {};
    }

    ErrorOr<JsonArraySerializer<Builder>> add_array(StringView key)
    {
        TRY(begin_item(key));
        return JsonArraySerializer<Builder>::try_create(m_builder);
    }

    ErrorOr<JsonObjectSerializer<Builder>> add_object(StringView key)
    {
        TRY(begin_item(key));
        return JsonObjectSerializer::try_create(m_builder);
    }

    ErrorOr<void> finish()
    {
        VERIFY(!m_finished);
        m_finished = true;
        if constexpr (IsLegacyBuilder<Builder>)
            TRY(m_builder.try_append('}'));
        else
            TRY(m_builder.append('}'));
        return {};
    }

private:
    explicit JsonObjectSerializer(Builder& builder)
        : m_builder(builder)
    {
    }

    ErrorOr<void> begin_item(StringView key)
    {
        VERIFY(!m_finished);
        if (!m_empty) {
            if constexpr (IsLegacyBuilder<Builder>)
                TRY(m_builder.try_append(','));
            else
                TRY(m_builder.append(','));
        }
        m_empty = false;

        if constexpr (IsLegacyBuilder<Builder>) {
            TRY(m_builder.try_append('"'));
            TRY(m_builder.try_append_escaped_for_json(key));
            TRY(m_builder.try_append("\":"sv));
        } else {
            TRY(m_builder.append('"'));
            TRY(m_builder.append_escaped_for_json(key));
            TRY(m_builder.append("\":"sv));
        }
        return {};
    }

    Builder& m_builder;
    bool m_empty { true };
    bool m_finished { false };
};

// Template magic to allow for JsonObjectSerializer<>::try_create(...) - Blame CxByte
template<>
struct JsonObjectSerializer<void> {
    template<typename Builder>
    static ErrorOr<JsonObjectSerializer<Builder>> try_create(Builder& builder)
    {
        return JsonObjectSerializer<Builder>::try_create(builder);
    }
};

template<typename Builder>
ErrorOr<JsonObjectSerializer<Builder>> JsonArraySerializer<Builder>::add_object()
{
    TRY(begin_item());
    return JsonObjectSerializer<Builder>::try_create(m_builder);
}

}

#if USING_AK_GLOBALLY
using AK::JsonObjectSerializer;
#endif
