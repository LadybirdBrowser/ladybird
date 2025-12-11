/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS::Bytecode {

struct PropertyKeyTableIndex {
    static constexpr u32 invalid = 0xffffffffu;
    bool is_valid() const { return value != invalid; }
    u32 value { 0 };
};

class PropertyKeyTable {
    AK_MAKE_NONMOVABLE(PropertyKeyTable);
    AK_MAKE_NONCOPYABLE(PropertyKeyTable);

public:
    PropertyKeyTable() = default;

    PropertyKeyTableIndex insert(PropertyKey);
    PropertyKey const& get(PropertyKeyTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_property_keys.is_empty(); }

    ReadonlySpan<PropertyKey const> property_keys() const { return m_property_keys; }

private:
    Vector<PropertyKey> m_property_keys;
};

}

namespace AK {

template<>
class Optional<JS::Bytecode::PropertyKeyTableIndex> : public OptionalBase<JS::Bytecode::PropertyKeyTableIndex> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Bytecode::PropertyKeyTableIndex;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<JS::Bytecode::PropertyKeyTableIndex> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = JS::Bytecode::PropertyKeyTableIndex>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, JS::Bytecode::PropertyKeyTableIndex>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Bytecode::PropertyKeyTableIndex>> && IsConstructible<JS::Bytecode::PropertyKeyTableIndex, U &&>)
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
        m_value.value = JS::Bytecode::PropertyKeyTableIndex::invalid;
    }

    [[nodiscard]] bool has_value() const
    {
        return m_value.is_valid();
    }

    [[nodiscard]] JS::Bytecode::PropertyKeyTableIndex& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::PropertyKeyTableIndex const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Bytecode::PropertyKeyTableIndex value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Bytecode::PropertyKeyTableIndex release_value()
    {
        VERIFY(has_value());
        JS::Bytecode::PropertyKeyTableIndex released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Bytecode::PropertyKeyTableIndex m_value { JS::Bytecode::PropertyKeyTableIndex::invalid };
};

}
