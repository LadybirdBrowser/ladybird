/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/FlyString.h>
#include <AK/Vector.h>

namespace JS::Bytecode {

struct IdentifierTableIndex {
    static constexpr u32 invalid = 0xffffffffu;
    bool is_valid() const { return value != invalid; }
    u32 value { 0 };
};

class IdentifierTable {
    AK_MAKE_NONMOVABLE(IdentifierTable);
    AK_MAKE_NONCOPYABLE(IdentifierTable);

public:
    IdentifierTable() = default;

    IdentifierTableIndex insert(FlyString);
    FlyString const& get(IdentifierTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_identifiers.is_empty(); }

private:
    Vector<FlyString> m_identifiers;
};

}

namespace AK {
template<>
class Optional<JS::Bytecode::IdentifierTableIndex> : public OptionalBase<JS::Bytecode::IdentifierTableIndex> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Bytecode::IdentifierTableIndex;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Bytecode::IdentifierTableIndex> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Bytecode::IdentifierTableIndex>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Bytecode::IdentifierTableIndex>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Bytecode::IdentifierTableIndex>> && IsConstructible<JS::Bytecode::IdentifierTableIndex, U &&>)
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
        m_value.value = JS::Bytecode::IdentifierTableIndex::invalid;
    }

    [[nodiscard]] bool has_value() const
    {
        return m_value.is_valid();
    }

    [[nodiscard]] JS::Bytecode::IdentifierTableIndex& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::IdentifierTableIndex const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::IdentifierTableIndex value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Bytecode::IdentifierTableIndex release_value()
    {
        VERIFY(has_value());
        JS::Bytecode::IdentifierTableIndex released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Bytecode::IdentifierTableIndex m_value { JS::Bytecode::IdentifierTableIndex::invalid };
};

}
