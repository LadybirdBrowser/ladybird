/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncDisposableStack.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncDisposableStack);

AsyncDisposableStack::AsyncDisposableStack(DisposeCapability dispose_capability, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_dispose_capability(move(dispose_capability))
{
}

void AsyncDisposableStack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_dispose_capability.visit_edges(visitor);
}

}
