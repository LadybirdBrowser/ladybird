/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Utf16FlyString.h>
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

    IdentifierTableIndex insert(Utf16FlyString);
    Utf16FlyString const& get(IdentifierTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_identifiers.is_empty(); }

    ReadonlySpan<Utf16FlyString const> identifiers() const { return m_identifiers; }

private:
    Vector<Utf16FlyString> m_identifiers;
};

}

namespace AK {

template<>
struct SentinelOptionalTraits<JS::Bytecode::IdentifierTableIndex> {
    static constexpr JS::Bytecode::IdentifierTableIndex sentinel_value() { return { JS::Bytecode::IdentifierTableIndex::invalid }; }
    static constexpr bool is_sentinel(JS::Bytecode::IdentifierTableIndex const& value) { return !value.is_valid(); }
};

template<>
class Optional<JS::Bytecode::IdentifierTableIndex> : public SentinelOptional<JS::Bytecode::IdentifierTableIndex> {
public:
    using SentinelOptional::SentinelOptional;
};

}
