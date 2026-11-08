/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Rootable.h>

namespace GC::Detail {

void RootGatheringVisitor::visit_impl(Cell& cell)
{
    m_roots.set(&cell, HeapRoot { .type = m_root_type });
}

void RootGatheringVisitor::visit_impl(ReadonlySpan<NanBoxedValue> values)
{
    for (auto const& value : values) {
        if (value.is_cell())
            visit_impl(value.as_cell());
    }
}

}
