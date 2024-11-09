/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/DistinctNumeric.h>
#include <AK/Vector.h>

namespace JS::Bytecode {

struct IdentifierTableIndex {
    bool is_valid() const { return value != NumericLimits<u32>::max(); }
    u32 value { 0 };
};

class IdentifierTable {
    AK_MAKE_NONMOVABLE(IdentifierTable);
    AK_MAKE_NONCOPYABLE(IdentifierTable);

public:
    IdentifierTable() = default;

    IdentifierTableIndex insert(DeprecatedFlyString);
    DeprecatedFlyString const& get(IdentifierTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_identifiers.is_empty(); }

private:
    Vector<DeprecatedFlyString> m_identifiers;
};

}
