/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>

namespace JS {

void ClassFieldDefinition::visit_edges(Cell::Visitor& visitor)
{
    name.visit(
        [&](PropertyKey const& key) { key.visit_edges(visitor); },
        [&](PrivateName const&) {});
    initializer.visit(
        [&](GC::Ref<ECMAScriptFunctionObject>& function) { visitor.visit(function); },
        [&](Value& value) { visitor.visit(value); },
        [&](Empty) {});
}

}
