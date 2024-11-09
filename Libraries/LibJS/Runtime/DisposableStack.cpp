/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DisposableStack.h>

namespace JS {

JS_DEFINE_ALLOCATOR(DisposableStack);

DisposableStack::DisposableStack(Vector<DisposableResource> stack, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_disposable_resource_stack(move(stack))
{
}

void DisposableStack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& resource : m_disposable_resource_stack) {
        visitor.visit(resource.resource_value);
        visitor.visit(resource.dispose_method);
    }
}

}
