/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Symbol.h>

namespace JS {

class PropertyKey {
public:
    enum class StringMayBeNumber {
        Yes,
        No,
    };

    static ThrowCompletionOr<PropertyKey> from_value(VM& vm, Value value)
    {
        VERIFY(!value.is_special_empty_value());
        if (value.is_symbol())
            return PropertyKey { value.as_symbol() };
        if (value.is_integral_number() && value.as_double() >= 0 && value.as_double() < NumericLimits<u32>::max())
            return static_cast<u32>(value.as_double());
        return TRY(value.to_string(vm));
    }

    static constexpr uintptr_t NORMAL_STRING_FLAG = 0;
    static constexpr uintptr_t SHORT_STRING_FLAG = 1;
    static constexpr uintptr_t SYMBOL_FLAG = 2;
    static constexpr uintptr_t NUMBER_FLAG = 3;

    bool is_string() const { return (m_bits & 3) == NORMAL_STRING_FLAG || (m_bits & 3) == SHORT_STRING_FLAG; }
    bool is_number() const { return (m_bits & 3) == NUMBER_FLAG; }
    bool is_symbol() const { return (m_bits & 3) == SYMBOL_FLAG; }

    PropertyKey() = delete;

    PropertyKey(PropertyKey const& other)
    {
        if (other.is_string())
            new (&m_string) FlyString(other.m_string);
        else
            m_bits = other.m_bits;
    }

    PropertyKey(PropertyKey&& other) noexcept
    {
        if (other.is_string())
            new (&m_string) FlyString(move(other.m_string));
        else
            m_bits = exchange(other.m_bits, 0);
    }

    template<Integral T>
    PropertyKey(T index)
    {
        // FIXME: Replace this with requires(IsUnsigned<T>)?
        //        Needs changes in various places using `int` (but not actually being in the negative range)
        VERIFY(index >= 0);
        if constexpr (NumericLimits<T>::max() >= NumericLimits<u32>::max()) {
            if (index >= NumericLimits<u32>::max()) {
                new (&m_string) FlyString { String::number(index) };
                return;
            }
        }
        m_number = static_cast<u64>(index) << 2 | NUMBER_FLAG;
    }

    PropertyKey(FlyString string, StringMayBeNumber string_may_be_number = StringMayBeNumber::Yes)
    {
        if (string_may_be_number == StringMayBeNumber::Yes) {
            auto view = string.bytes_as_string_view();
            if (!view.is_empty() && !(view[0] == '0' && view.length() > 1)) {
                auto property_index = view.to_number<u32>(TrimWhitespace::No);
                if (property_index.has_value() && property_index.value() < NumericLimits<u32>::max()) {
                    m_number = static_cast<u64>(property_index.release_value()) << 2 | NUMBER_FLAG;
                    return;
                }
            }
        }
        new (&m_string) FlyString(move(string));
    }

    PropertyKey(String const& string)
        : PropertyKey(FlyString(string))
    {
    }

    PropertyKey(GC::Ref<Symbol> symbol)
    {
        m_bits = reinterpret_cast<uintptr_t>(symbol.ptr()) | SYMBOL_FLAG;
    }

    PropertyKey& operator=(PropertyKey const& other)
    {
        if (this != &other) {
            if (is_string())
                m_string.~FlyString();
            new (this) PropertyKey(other);
        }
        return *this;
    }

    PropertyKey& operator=(PropertyKey&& other) noexcept
    {
        if (this != &other) {
            if (is_string())
                m_string.~FlyString();
            new (this) PropertyKey(move(other));
        }
        return *this;
    }

    ~PropertyKey()
    {
        if (is_string())
            m_string.~FlyString();
    }

    u32 as_number() const
    {
        VERIFY(is_number());
        return m_number >> 2;
    }

    FlyString const& as_string() const
    {
        VERIFY(is_string());
        return m_string;
    }

    Symbol const* as_symbol() const
    {
        VERIFY(is_symbol());
        return reinterpret_cast<Symbol const*>(m_bits & ~3ULL);
    }

    Value to_value(VM& vm) const
    {
        if (is_string())
            return Value { PrimitiveString::create(vm, as_string()) };
        if (is_symbol())
            return Value { as_symbol() };
        return Value { PrimitiveString::create(vm, String::number(as_number())) };
    }

    String to_string() const
    {
        if (is_string())
            return as_string().to_string();
        if (is_symbol())
            return MUST(as_symbol()->descriptive_string());
        return String::number(as_number());
    }

    void visit_edges(Cell::Visitor& visitor) const
    {
        if (is_symbol())
            visitor.visit(const_cast<Symbol*>(as_symbol()));
    }

    bool operator==(PropertyKey const& other) const
    {
        if (is_string())
            return other.is_string() && m_string == other.m_string;
        if (is_symbol())
            return other.is_symbol() && as_symbol() == other.as_symbol();
        if (other.is_number())
            return as_number() == other.as_number();
        return false;
    }

private:
    friend Traits<PropertyKey>;

    union {
        FlyString m_string;
        u64 m_number;
        Symbol const* m_symbol;
        uintptr_t m_bits;
    };
};

static_assert(sizeof(PropertyKey) == sizeof(uintptr_t));

}

namespace AK {

template<>
struct Traits<JS::PropertyKey> : public DefaultTraits<JS::PropertyKey> {
    static unsigned hash(JS::PropertyKey const& name)
    {
        if (name.is_string())
            return name.as_string().hash();
        if (name.is_symbol())
            return ptr_hash(name.as_symbol());
        if (name.is_number())
            return int_hash(name.as_number());
        VERIFY_NOT_REACHED();
    }

    static bool equals(JS::PropertyKey const& a, JS::PropertyKey const& b)
    {
        if (a.is_string())
            return b.is_string() && a.as_string() == b.as_string();
        if (a.is_symbol())
            return b.is_symbol() && a.as_symbol() == b.as_symbol();
        if (a.is_number())
            return b.is_number() && a.as_number() == b.as_number();
        VERIFY_NOT_REACHED();
    }
};

template<>
struct Formatter<JS::PropertyKey> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::PropertyKey const& property_key)
    {
        if (property_key.is_number())
            return builder.put_u64(property_key.as_number());
        return builder.put_string(property_key.to_string());
    }
};

}
