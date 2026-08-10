/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ExternalEntityTable.h>

namespace GC {

ExternalEntityTableHandle ExternalEntityTable::allocate_entry(ExternalEntityTableTag tag)
{
    VERIFY(tag.is_balanced());

    u32 index;
    if (m_free_entry_indices.is_empty()) {
        VERIFY(m_entries.size() < ExternalEntityTableHandle::invalid_index);
        index = static_cast<u32>(m_entries.size());
        m_entries.append({});
    } else {
        index = m_free_entry_indices.take_last();
    }

    auto& entry = m_entries[index];
    VERIFY(!entry.allocated);
    entry.allocated = true;
    entry.tag = tag;
    return { index, entry.generation };
}

bool ExternalEntityTable::is_valid(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag) const
{
    return entry_for(handle, expected_tag) != nullptr;
}

void ExternalEntityTable::free_entry(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag)
{
    auto* entry = entry_for(handle, expected_tag);
    if (!entry)
        return;

    entry->allocated = false;
    ++entry->generation;
    if (entry->generation == 0)
        ++entry->generation;
    m_free_entry_indices.append(handle.index);
}

ExternalEntityTable::Entry* ExternalEntityTable::entry_for(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag)
{
    if (!handle.is_valid() || handle.index >= m_entries.size())
        return nullptr;
    auto& entry = m_entries[handle.index];
    if (!entry.allocated || entry.generation != handle.generation || entry.tag != expected_tag)
        return nullptr;
    return &entry;
}

ExternalEntityTable::Entry const* ExternalEntityTable::entry_for(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag) const
{
    return const_cast<ExternalEntityTable&>(*this).entry_for(handle, expected_tag);
}

}
