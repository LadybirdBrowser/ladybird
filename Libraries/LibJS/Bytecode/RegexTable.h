/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace JS::Bytecode {

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, RegexTableIndex, Comparison);

class RegexTable {
    AK_MAKE_NONMOVABLE(RegexTable);
    AK_MAKE_NONCOPYABLE(RegexTable);

public:
    RegexTable() = default;

    bool is_empty() const { return true; }
};

}
