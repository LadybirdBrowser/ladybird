/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/CompletionCell.h>

namespace JS {

GC_DEFINE_ALLOCATOR(CompletionCell);

CompletionCell::~CompletionCell() = default;

void CompletionCell::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_completion.value().has_value())
        visitor.visit(m_completion.value().value());
}

}
