/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DisposableStack.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DisposableStack);

DisposableStack::DisposableStack(DisposeCapability dispose_capability, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_dispose_capability(move(dispose_capability))
{
}

void DisposableStack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_dispose_capability.visit_edges(visitor);
}

}
