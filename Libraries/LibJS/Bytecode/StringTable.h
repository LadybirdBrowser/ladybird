/*
 * Copyright (c) 2021, Gunnar Beutner <gbeutner@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace JS::Bytecode {

struct StringTableIndex {
    static constexpr u32 invalid = 0xffffffffu;
    bool is_valid() const { return value != invalid; }
    u32 value { 0 };
};

class StringTable {
    AK_MAKE_NONMOVABLE(StringTable);
    AK_MAKE_NONCOPYABLE(StringTable);

public:
    StringTable() = default;

    StringTableIndex insert(String);
    String const& get(StringTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_strings.is_empty(); }

private:
    Vector<String> m_strings;
};

}

namespace AK {

template<>
class Optional<JS::Bytecode::StringTableIndex> : public OptionalBase<JS::Bytecode::StringTableIndex> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Bytecode::StringTableIndex;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Bytecode::StringTableIndex> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Bytecode::StringTableIndex>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Bytecode::StringTableIndex>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Bytecode::StringTableIndex>> && IsConstructible<JS::Bytecode::StringTableIndex, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    Optional& operator=(V)
    {
        clear();
        return *this;
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    void clear()
    {
        m_value.value = JS::Bytecode::StringTableIndex::invalid;
    }

    [[nodiscard]] bool has_value() const
    {
        return m_value.is_valid();
    }

    [[nodiscard]] JS::Bytecode::StringTableIndex& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::StringTableIndex const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::StringTableIndex value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Bytecode::StringTableIndex release_value()
    {
        VERIFY(has_value());
        JS::Bytecode::StringTableIndex released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Bytecode::StringTableIndex m_value { JS::Bytecode::StringTableIndex::invalid };
};

}
