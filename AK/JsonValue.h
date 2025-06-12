/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>

namespace AK {

class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    using Storage = Variant<
        Empty,
        bool,
        i64,
        u64,
        double,
        String,
        NonnullOwnPtr<JsonArray>,
        NonnullOwnPtr<JsonObject>>;

    static ErrorOr<JsonValue> from_string(StringView);

    JsonValue();
    ~JsonValue();

    JsonValue(JsonValue const&);
    JsonValue(JsonValue&&);

    JsonValue& operator=(JsonValue const&);
    JsonValue& operator=(JsonValue&&);

    JsonValue(int);
    JsonValue(unsigned);
    JsonValue(long);
    JsonValue(long unsigned);
    JsonValue(long long);
    JsonValue(long long unsigned);
    JsonValue(double);

    JsonValue(String);
    JsonValue(StringView);

    template<typename T>
    requires(SameAs<RemoveCVReference<T>, bool>)
    JsonValue(T value)
        : m_value { static_cast<bool>(value) }
    {
    }

    JsonValue(JsonArray const&);
    JsonValue(JsonObject const&);

    JsonValue(JsonArray&&);
    JsonValue(JsonObject&&);

    JsonValue& operator=(JsonArray const&);
    JsonValue& operator=(JsonObject const&);

    JsonValue& operator=(JsonArray&&);
    JsonValue& operator=(JsonObject&&);

    String serialized() const;
    void serialize(StringBuilder&) const;

    Optional<int> get_int() const { return get_integer<int>(); }
    Optional<i32> get_i32() const { return get_integer<i32>(); }
    Optional<i64> get_i64() const { return get_integer<i64>(); }

    Optional<unsigned> get_uint() const { return get_integer<unsigned>(); }
    Optional<u32> get_u32() const { return get_integer<u32>(); }
    Optional<u64> get_u64() const { return get_integer<u64>(); }
    Optional<float> get_float_with_precision_loss() const { return get_number_with_precision_loss<float>(); }
    Optional<double> get_double_with_precision_loss() const { return get_number_with_precision_loss<double>(); }

    Optional<FlatPtr> get_addr() const
    {
        // Note: This makes the lambda dependent on the template parameter, which is necessary
        //       for the `if constexpr` to not evaluate both branches.
        auto fn = [&]<typename T>() -> Optional<T> {
            if constexpr (IsSame<T, u64>) {
                return get_u64();
            } else {
                return get_u32();
            }
        };

        return fn.operator()<FlatPtr>();
    }

    Optional<bool> get_bool() const
    {
        if (!is_bool())
            return {};
        return as_bool();
    }

    bool as_bool() const
    {
        return m_value.get<bool>();
    }

    String const& as_string() const
    {
        return m_value.get<String>();
    }

    JsonObject& as_object()
    {
        return *m_value.get<NonnullOwnPtr<JsonObject>>();
    }

    JsonObject const& as_object() const
    {
        return *m_value.get<NonnullOwnPtr<JsonObject>>();
    }

    JsonArray& as_array()
    {
        return *m_value.get<NonnullOwnPtr<JsonArray>>();
    }

    JsonArray const& as_array() const
    {
        return *m_value.get<NonnullOwnPtr<JsonArray>>();
    }

    Variant<u64, i64, double> as_number() const
    {
        return m_value.downcast<u64, i64, double>();
    }

    Type type() const
    {
        return m_value.visit(
            [](Empty const&) { return Type::Null; },
            [](bool const&) { return Type::Bool; },
            [](Arithmetic auto const&) { return Type::Number; },
            [](String const&) { return Type::String; },
            [](NonnullOwnPtr<JsonArray> const&) { return Type::Array; },
            [](NonnullOwnPtr<JsonObject> const&) { return Type::Object; });
    }

    bool is_null() const { return m_value.has<Empty>(); }
    bool is_bool() const { return m_value.has<bool>(); }
    bool is_string() const { return m_value.has<String>(); }
    bool is_array() const { return m_value.has<NonnullOwnPtr<JsonArray>>(); }
    bool is_object() const { return m_value.has<NonnullOwnPtr<JsonObject>>(); }
    bool is_number() const
    {
        return m_value.visit(
            [](bool const&) { return false; },
            []<Arithmetic U>(U const&) { return true; },
            [](auto const&) { return false; });
    }

    template<typename T>
    Optional<T> get_number_with_precision_loss() const
    {
        return m_value.visit(
            [](bool const&) { return Optional<T> {}; },
            []<Arithmetic U>(U const& value) { return Optional<T> { static_cast<T>(value) }; },
            [](auto const&) { return Optional<T> {}; });
    }

    template<Integral T>
    bool is_integer() const
    {
        return get_integer<T>().has_value();
    }

    template<Integral T>
    T as_integer() const
    {
        return get_integer<T>().value();
    }

    template<Integral T>
    Optional<T> get_integer() const
    {
        return m_value.visit(
            [](bool const&) { return Optional<T> {}; },
            []<Arithmetic U>(U const& value) -> Optional<T> {
                if constexpr (Integral<U>) {
                    if (!is_within_range<T>(value))
                        return {};
                    return static_cast<T>(value);
                } else {
                    // FIXME: Make is_within_range work with floating point numbers.
                    if (static_cast<U>(static_cast<T>(value)) != value)
                        return {};
                    return static_cast<T>(value);
                }
            },
            [](auto const&) { return Optional<T> {}; });
    }

    bool equals(JsonValue const& other) const;

private:
    Storage m_value;
};

template<>
struct Formatter<JsonValue> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JsonValue const& value)
    {
        return Formatter<StringView>::format(builder, value.serialized());
    }
};

}

#if USING_AK_GLOBALLY
using AK::JsonValue;
#endif
