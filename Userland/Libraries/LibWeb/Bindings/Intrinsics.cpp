/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(Intrinsics);

void Intrinsics::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_namespaces);
    visitor.visit(m_prototypes);
    visitor.visit(m_constructors);
    visitor.visit(m_realm);
}

bool Intrinsics::is_exposed(StringView name) const
{
    return m_constructors.contains(name) || m_prototypes.contains(name) || m_namespaces.contains(name);
}

}
