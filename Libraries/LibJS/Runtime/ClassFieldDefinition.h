/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

using ClassElementName = Variant<PropertyKey, PrivateName>;

// 6.2.10 The ClassFieldDefinition Record Specification Type, https://tc39.es/ecma262/#sec-classfielddefinition-record-specification-type
struct ClassFieldDefinition {
    ClassElementName name;                                                // [[Name]] / [[Key]]
    Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty> initializer; // [[Initializer]]
    Vector<GC::Ref<FunctionObject>> decorator_initializers;               // [[Initializers]] (decorator init chain)
    Vector<GC::Ref<FunctionObject>> extra_initializers;                   // [[ExtraInitializers]] (addInitializer callbacks)

    void visit_edges(Cell::Visitor& visitor);
};

}
