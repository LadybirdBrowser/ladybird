/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/FlyString.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/StringOrSymbol.h>

namespace JS {

class PropertyKey {
public:
    enum class Type : u8 {
        Invalid,
        Number,
        String,
        Symbol,
    };

    enum class StringMayBeNumber {
        Yes,
        No,
    };

    static ThrowCompletionOr<PropertyKey> from_value(VM& vm, Value value)
    {
        if (value.is_empty())
            return PropertyKey {};
        if (value.is_symbol())
            return PropertyKey { value.as_symbol() };
        if (value.is_integral_number() && value.as_double() >= 0 && value.as_double() < NumericLimits<u32>::max())
            return static_cast<u32>(value.as_double());
        return TRY(value.to_byte_string(vm));
    }

    PropertyKey() = default;

    template<Integral T>
    PropertyKey(T index)
    {
        // FIXME: Replace this with requires(IsUnsigned<T>)?
        //        Needs changes in various places using `int` (but not actually being in the negative range)
        VERIFY(index >= 0);
        if constexpr (NumericLimits<T>::max() >= NumericLimits<u32>::max()) {
            if (index >= NumericLimits<u32>::max()) {
                m_string = ByteString::number(index);
                m_type = Type::String;
                m_string_may_be_number = false;
                return;
            }
        }

        m_type = Type::Number;
        m_number = index;
    }

    PropertyKey(char const* chars)
        : m_type(Type::String)
        , m_string(DeprecatedFlyString(chars))
    {
    }

    PropertyKey(ByteString const& string)
        : m_type(Type::String)
        , m_string(DeprecatedFlyString(string))
    {
    }

    PropertyKey(FlyString const& string)
        : m_type(Type::String)
        , m_string(string.to_deprecated_fly_string())
    {
    }

    PropertyKey(DeprecatedFlyString string, StringMayBeNumber string_may_be_number = StringMayBeNumber::Yes)
        : m_string_may_be_number(string_may_be_number == StringMayBeNumber::Yes)
        , m_type(Type::String)
        , m_string(move(string))
    {
    }

    PropertyKey(NonnullGCPtr<Symbol> symbol)
        : m_type(Type::Symbol)
        , m_symbol(symbol)
    {
    }

    PropertyKey(StringOrSymbol const& string_or_symbol)
    {
        if (string_or_symbol.is_string()) {
            m_string = string_or_symbol.as_string();
            m_type = Type::String;
        } else if (string_or_symbol.is_symbol()) {
            m_symbol = const_cast<Symbol*>(string_or_symbol.as_symbol());
            m_type = Type::Symbol;
        }
    }

    ALWAYS_INLINE Type type() const { return m_type; }

    bool is_valid() const { return m_type != Type::Invalid; }
    bool is_number() const
    {
        if (m_type == Type::Number)
            return true;
        if (m_type != Type::String || !m_string_may_be_number)
            return false;

        return const_cast<PropertyKey*>(this)->try_coerce_into_number();
    }
    bool is_string() const
    {
        if (m_type != Type::String)
            return false;
        if (!m_string_may_be_number)
            return true;

        return !const_cast<PropertyKey*>(this)->try_coerce_into_number();
    }
    bool is_symbol() const { return m_type == Type::Symbol; }

    bool try_coerce_into_number()
    {
        VERIFY(m_string_may_be_number);
        if (m_string.is_empty()) {
            m_string_may_be_number = false;
            return false;
        }

        if (char first = m_string.characters()[0]; first < '0' || first > '9') {
            m_string_may_be_number = false;
            return false;
        } else if (m_string.length() > 1 && first == '0') {
            m_string_may_be_number = false;
            return false;
        }

        auto property_index = m_string.to_number<unsigned>(TrimWhitespace::No);
        if (!property_index.has_value() || property_index.value() == NumericLimits<u32>::max()) {
            m_string_may_be_number = false;
            return false;
        }
        m_type = Type::Number;
        m_number = *property_index;
        return true;
    }

    u32 as_number() const
    {
        VERIFY(is_number());
        return m_number;
    }

    DeprecatedFlyString const& as_string() const
    {
        VERIFY(is_string());
        return m_string;
    }

    Symbol const* as_symbol() const
    {
        VERIFY(is_symbol());
        return m_symbol;
    }

    ByteString to_string() const
    {
        VERIFY(is_valid());
        VERIFY(!is_symbol());
        if (is_string())
            return as_string();
        return ByteString::number(as_number());
    }

    StringOrSymbol to_string_or_symbol() const
    {
        VERIFY(is_valid());
        VERIFY(!is_number());
        if (is_string())
            return StringOrSymbol(as_string());
        return StringOrSymbol(as_symbol());
    }

private:
    bool m_string_may_be_number { true };
    Type m_type { Type::Invalid };
    u32 m_number { 0 };
    DeprecatedFlyString m_string;
    Handle<Symbol> m_symbol;
};

}

namespace AK {

template<>
struct Traits<JS::PropertyKey> : public DefaultTraits<JS::PropertyKey> {
    static unsigned hash(JS::PropertyKey const& name)
    {
        VERIFY(name.is_valid());
        if (name.is_string())
            return name.as_string().hash();
        if (name.is_number())
            return int_hash(name.as_number());
        return ptr_hash(name.as_symbol());
    }

    static bool equals(JS::PropertyKey const& a, JS::PropertyKey const& b)
    {
        if (a.type() != b.type())
            return false;

        switch (a.type()) {
        case JS::PropertyKey::Type::Number:
            return a.as_number() == b.as_number();
        case JS::PropertyKey::Type::String:
            return a.as_string() == b.as_string();
        case JS::PropertyKey::Type::Symbol:
            return a.as_symbol() == b.as_symbol();
        default:
            VERIFY_NOT_REACHED();
        }
    }
};

template<>
struct Formatter<JS::PropertyKey> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::PropertyKey const& property_key)
    {
        if (!property_key.is_valid())
            return builder.put_string("<invalid PropertyKey>"sv);
        if (property_key.is_number())
            return builder.put_u64(property_key.as_number());
        return builder.put_string(property_key.to_string_or_symbol().to_display_string());
    }
};

}
