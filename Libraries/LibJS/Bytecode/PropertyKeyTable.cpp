/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/PropertyKeyTable.h>

namespace JS::Bytecode {

PropertyKeyTableIndex PropertyKeyTable::insert(PropertyKey key)
{
    m_property_keys.append(move(key));
    VERIFY(m_property_keys.size() <= NumericLimits<u32>::max());
    return { static_cast<u32>(m_property_keys.size() - 1) };
}

PropertyKey const& PropertyKeyTable::get(PropertyKeyTableIndex index) const
{
    return m_property_keys[index.value];
}

void PropertyKeyTable::dump() const
{
    outln("Property Key Table:");
    for (size_t i = 0; i < m_property_keys.size(); i++)
        outln("{}: {}", i, m_property_keys[i]);
}

}
