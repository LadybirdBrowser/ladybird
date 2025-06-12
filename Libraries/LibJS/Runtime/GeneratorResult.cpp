/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GeneratorResult.h>

namespace JS {

GC_DEFINE_ALLOCATOR(GeneratorResult);

GeneratorResult::~GeneratorResult() = default;

void GeneratorResult::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_result);
    visitor.visit(m_continuation);
}

}
