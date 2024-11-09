/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class StringOrSymbol {
public:
    StringOrSymbol()
        : m_bits(0)
    {
    }

    StringOrSymbol(char const* chars)
        : StringOrSymbol(DeprecatedFlyString(chars))
    {
    }

    StringOrSymbol(ByteString const& string)
        : StringOrSymbol(DeprecatedFlyString(string))
    {
    }

    StringOrSymbol(DeprecatedFlyString const& string)
        : m_string(string)
    {
    }

    ~StringOrSymbol()
    {
        if (is_string())
            m_string.~DeprecatedFlyString();
    }

    StringOrSymbol(Symbol const* symbol)
        : m_symbol_with_tag(symbol)
    {
        set_symbol_flag();
    }

    StringOrSymbol(StringOrSymbol const& other)
    {
        if (other.is_string())
            new (&m_string) DeprecatedFlyString(other.m_string);
        else
            m_bits = other.m_bits;
    }

    StringOrSymbol(StringOrSymbol&& other)
    {
        if (other.is_string())
            new (&m_string) DeprecatedFlyString(move(other.m_string));
        else
            m_bits = exchange(other.m_bits, 0);
    }

    ALWAYS_INLINE bool is_valid() const { return m_bits != 0; }
    ALWAYS_INLINE bool is_symbol() const { return is_valid() && (m_bits & 2); }
    ALWAYS_INLINE bool is_string() const { return is_valid() && !(m_bits & 2); }

    ALWAYS_INLINE DeprecatedFlyString as_string() const
    {
        VERIFY(is_string());
        return m_string;
    }

    ALWAYS_INLINE Symbol const* as_symbol() const
    {
        VERIFY(is_symbol());
        return reinterpret_cast<Symbol const*>(m_bits & ~2ULL);
    }

    ByteString to_display_string() const
    {
        if (is_string())
            return as_string();
        if (is_symbol())
            return as_symbol()->descriptive_string().release_value_but_fixme_should_propagate_errors().to_byte_string();
        VERIFY_NOT_REACHED();
    }

    Value to_value(VM& vm) const
    {
        if (is_string())
            return PrimitiveString::create(vm, as_string());
        if (is_symbol())
            return const_cast<Symbol*>(as_symbol());
        return {};
    }

    void visit_edges(Cell::Visitor& visitor)
    {
        if (is_symbol())
            visitor.visit(const_cast<Symbol*>(as_symbol()));
    }

    ALWAYS_INLINE bool operator==(StringOrSymbol const& other) const
    {
        if (is_string())
            return other.is_string() && m_string == other.m_string;
        if (is_symbol())
            return other.is_symbol() && as_symbol() == other.as_symbol();
        return true;
    }

    StringOrSymbol& operator=(StringOrSymbol const& other)
    {
        if (this != &other) {
            this->~StringOrSymbol();
            new (this) StringOrSymbol(other);
        }
        return *this;
    }

    StringOrSymbol& operator=(StringOrSymbol&& other)
    {
        if (this != &other) {
            this->~StringOrSymbol();
            new (this) StringOrSymbol(move(other));
        }
        return *this;
    }

    unsigned hash() const
    {
        if (is_string())
            return m_string.hash();
        return ptr_hash(as_symbol());
    }

private:
    ALWAYS_INLINE void set_symbol_flag()
    {
        m_bits |= 2;
    }

    union {
        DeprecatedFlyString m_string;
        Symbol const* m_symbol_with_tag;
        uintptr_t m_bits;
    };
};

static_assert(sizeof(StringOrSymbol) == sizeof(uintptr_t));

}

template<>
struct AK::Traits<JS::StringOrSymbol> : public DefaultTraits<JS::StringOrSymbol> {
    static unsigned hash(JS::StringOrSymbol const& key)
    {
        return key.hash();
    }
};
