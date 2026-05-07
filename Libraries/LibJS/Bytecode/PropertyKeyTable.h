/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/ExternalMemory.h>
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
    size_t external_memory_size() const { return vector_external_memory_size(m_property_keys); }

    ReadonlySpan<PropertyKey const> property_keys() const { return m_property_keys; }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        for (auto& key : m_property_keys)
            key.visit_edges(visitor);
    }

private:
    Vector<PropertyKey> m_property_keys;
};

}

namespace AK {

template<>
struct SentinelOptionalTraits<JS::Bytecode::PropertyKeyTableIndex> {
    static constexpr JS::Bytecode::PropertyKeyTableIndex sentinel_value() { return { JS::Bytecode::PropertyKeyTableIndex::invalid }; }
    static constexpr bool is_sentinel(JS::Bytecode::PropertyKeyTableIndex const& value) { return !value.is_valid(); }
};

template<>
class Optional<JS::Bytecode::PropertyKeyTableIndex> : public SentinelOptional<JS::Bytecode::PropertyKeyTableIndex> {
public:
    using SentinelOptional::SentinelOptional;
};

}
