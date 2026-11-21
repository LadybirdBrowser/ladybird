/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/BlockDescriptor.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CellTypeInfo.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/NanBoxedValue.h>

namespace GC {

StringView class_name_of(Cell const& cell)
{
    if (cell.state() != Cell::State::Live)
        return "FreelistEntry"sv;
    auto const& type_info = cell.type_info();
    if (type_info.class_name) {
        size_t length = 0;
        auto const* characters = type_info.class_name(&cell, &length);
        return { characters, length };
    }
    return HeapBlock::from_cell(&cell)->cell_allocator().class_name().value_or("Cell"sv);
}

bool Cell::is_marked() const
{
    return GC::is_marked(this);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
size_t Cell::Visitor::marker_offset()
{
    return offsetof(Visitor, m_marker);
}
#pragma GCC diagnostic pop

void GC::Cell::Visitor::visit(NanBoxedValue const& value)
{
    if (!value.is_cell())
        return;
    // NB: The value is visited through its slot, which a collection that moves cells rewrites if it is in a cell.
    if (m_marker) {
        gc_marker_visit_values(m_marker, reinterpret_cast<uint64_t const*>(&value), 1);
        return;
    }
    visit_cell(value.as_cell());
}

void GC::Cell::Visitor::visit_ephemeron(Cell&, NanBoxedValue const& value)
{
    visit(value);
}

}
