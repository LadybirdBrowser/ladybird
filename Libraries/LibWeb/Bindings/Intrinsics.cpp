/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Forward.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/SyntheticHostDefined.h>

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

Intrinsics& host_defined_intrinsics(JS::Realm& realm)
{
    ASSERT(realm.host_defined());
    return static_cast<Bindings::HostDefined&>(*realm.host_defined()).intrinsics;
}

}
