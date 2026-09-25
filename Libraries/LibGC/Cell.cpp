/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void GC::Cell::Visitor::visit(NanBoxedValue const& value)
{
    if (value.is_cell())
        visit_impl(value.as_cell());
}

}
