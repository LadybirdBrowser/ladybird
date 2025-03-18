/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibRegex/RegexParser.h>

namespace JS::Bytecode {

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(size_t, RegexTableIndex, Comparison);

struct ParsedRegex {
    regex::Parser::Result regex;
    String pattern;
    regex::RegexOptions<ECMAScriptFlags> flags;
};

class RegexTable {
    AK_MAKE_NONMOVABLE(RegexTable);
    AK_MAKE_NONCOPYABLE(RegexTable);

public:
    RegexTable() = default;

    RegexTableIndex insert(ParsedRegex);
    ParsedRegex const& get(RegexTableIndex) const;
    void dump() const;
    bool is_empty() const { return m_regexes.is_empty(); }

private:
    Vector<ParsedRegex> m_regexes;
};

}
