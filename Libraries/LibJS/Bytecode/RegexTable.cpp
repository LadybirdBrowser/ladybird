/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/RegexTable.h>

namespace JS::Bytecode {

RegexTableIndex RegexTable::insert(ParsedRegex regex)
{
    m_regexes.append(move(regex));
    return m_regexes.size() - 1;
}

ParsedRegex const& RegexTable::get(RegexTableIndex index) const
{
    return m_regexes[index.value()];
}

void RegexTable::dump() const
{
    outln("Regex Table:");
    for (size_t i = 0; i < m_regexes.size(); i++)
        outln("{}: {}", i, m_regexes[i].pattern);
}

}
