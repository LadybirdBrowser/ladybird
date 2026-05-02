/*
 * Copyright (c) 2021, Gunnar Beutner <gbeutner@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Utf16String.h>
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

    StringTableIndex insert(Utf16String);
    Utf16String const& get(StringTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_strings.is_empty(); }
    size_t size() const { return m_strings.size(); }

private:
    Vector<Utf16String> m_strings;
};

}

namespace AK {

template<>
struct SentinelOptionalTraits<JS::Bytecode::StringTableIndex> {
    static constexpr JS::Bytecode::StringTableIndex sentinel_value() { return { JS::Bytecode::StringTableIndex::invalid }; }
    static constexpr bool is_sentinel(JS::Bytecode::StringTableIndex const& value) { return !value.is_valid(); }
};

template<>
class Optional<JS::Bytecode::StringTableIndex> : public SentinelOptional<JS::Bytecode::StringTableIndex> {
public:
    using SentinelOptional::SentinelOptional;
};

}
