/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::Bindings {

void HostDefined::visit_edges(JS::Cell::Visitor& visitor)
{
    JS::Realm::HostDefined::visit_edges(visitor);
    visitor.visit(intrinsics);
}

}
