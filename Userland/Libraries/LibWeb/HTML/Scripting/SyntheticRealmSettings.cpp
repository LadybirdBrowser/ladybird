/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/SyntheticRealmSettings.h>

namespace Web::HTML {

void SyntheticRealmSettings::visit_edges(JS::Cell::Visitor& visitor)
{
    execution_context->visit_edges(visitor);
    visitor.visit(principal_realm);
    visitor.visit(underlying_realm);
    visitor.visit(module_map);
}

}
