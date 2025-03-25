/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/StringOrSymbol.h>

namespace JS {

class PropertyKey {
    AK_MAKE_DEFAULT_COPYABLE(PropertyKey);
    AK_MAKE_DEFAULT_MOVABLE(PropertyKey);

public:
    enum class StringMayBeNumber {
        Yes,
        No,
    };

    static ThrowCompletionOr<PropertyKey> from_value(VM& vm, Value value)
    {
        VERIFY(!value.is_empty());
        if (value.is_symbol())
            return PropertyKey { value.as_symbol() };
        if (value.is_integral_number() && value.as_double() >= 0 && value.as_double() < NumericLimits<u32>::max())
            return static_cast<u32>(value.as_double());
        return TRY(value.to_string(vm));
    }

    PropertyKey() = delete;

    template<Integral T>
    PropertyKey(T index)
        : m_data(index)
    {
        // FIXME: Replace this with requires(IsUnsigned<T>)?
        //        Needs changes in various places using `int` (but not actually being in the negative range)
        VERIFY(index >= 0);
        if constexpr (NumericLimits<T>::max() >= NumericLimits<u32>::max()) {
            if (index >= NumericLimits<u32>::max()) {
                m_data = FlyString { String::number(index) };
                return;
            }
        }
    }

    PropertyKey(FlyString string, StringMayBeNumber string_may_be_number = StringMayBeNumber::Yes)
        : m_data { try_coerce_into_number(move(string), string_may_be_number) }
    {
    }

    PropertyKey(String const& string)
        : PropertyKey(FlyString(string))
    {
    }

    PropertyKey(GC::Ref<Symbol> symbol)
        : m_data { symbol }
    {
    }

    PropertyKey(StringOrSymbol const& string_or_symbol)
        : m_data {
            string_or_symbol.is_string()
                ? Variant<FlyString, GC::Root<Symbol>, u32> { string_or_symbol.as_string() }
                : Variant<FlyString, GC::Root<Symbol>, u32> { const_cast<Symbol*>(string_or_symbol.as_symbol()) }
        }
    {
    }

    bool is_number() const { return m_data.has<u32>(); }
    bool is_string() const { return m_data.has<FlyString>(); }
    bool is_symbol() const { return m_data.has<GC::Root<Symbol>>(); }

    u32 as_number() const { return m_data.get<u32>(); }
    FlyString const& as_string() const { return m_data.get<FlyString>(); }
    Symbol const* as_symbol() const { return m_data.get<GC::Root<Symbol>>(); }

    String to_string() const
    {
        VERIFY(!is_symbol());
        if (is_string())
            return as_string().to_string();
        return String::number(as_number());
    }

    StringOrSymbol to_string_or_symbol() const
    {
        VERIFY(!is_number());
        if (is_string())
            return StringOrSymbol(as_string());
        return StringOrSymbol(as_symbol());
    }

    bool operator==(PropertyKey const&) const = default;

private:
    friend Traits<JS::PropertyKey>;

    static Variant<FlyString, u32> try_coerce_into_number(FlyString string, StringMayBeNumber string_may_be_number)
    {
        if (string_may_be_number != StringMayBeNumber::Yes)
            return string;
        auto view = string.bytes_as_string_view();
        if (view.is_empty())
            return string;
        if (view[0] == '0' && view.length() > 1)
            return string;
        auto property_index = view.to_number<u32>(TrimWhitespace::No);
        if (!property_index.has_value() || property_index.value() >= NumericLimits<u32>::max())
            return string;
        return property_index.release_value();
    }

    Variant<FlyString, u32, GC::Root<Symbol>> m_data;
};

}

namespace AK {

template<>
struct Traits<JS::PropertyKey> : public DefaultTraits<JS::PropertyKey> {
    static unsigned hash(JS::PropertyKey const& name)
    {
        return name.m_data.visit(
            [](FlyString const& string) { return string.hash(); },
            [](GC::Root<JS::Symbol> const& symbol) { return ptr_hash(symbol.ptr()); },
            [](u32 const& number) { return int_hash(number); });
    }

    static bool equals(JS::PropertyKey const& a, JS::PropertyKey const& b)
    {
        return a.m_data == b.m_data;
    }
};

template<>
struct Formatter<JS::PropertyKey> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::PropertyKey const& property_key)
    {
        if (property_key.is_number())
            return builder.put_u64(property_key.as_number());
        return builder.put_string(property_key.to_string_or_symbol().to_display_string());
    }
};

}
