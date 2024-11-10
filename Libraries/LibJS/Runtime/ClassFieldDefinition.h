/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

using ClassElementName = Variant<PropertyKey, PrivateName>;

// 6.2.10 The ClassFieldDefinition Record Specification Type, https://tc39.es/ecma262/#sec-classfielddefinition-record-specification-type
struct ClassFieldDefinition {
    ClassElementName name;                       // [[Name]]
    GCPtr<ECMAScriptFunctionObject> initializer; // [[Initializer]]
};

}
