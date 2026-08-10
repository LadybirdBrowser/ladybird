/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/ClassBlueprint.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>

namespace JS {

ThrowCompletionOr<GC::Ref<ECMAScriptFunctionObject>> construct_class(
    VM&,
    Bytecode::ClassBlueprint const&,
    Bytecode::Executable const&,
    GC::Ptr<Environment> class_environment,
    GC::Ptr<Environment> outer_environment,
    Value super_class,
    ReadonlySpan<Value> element_keys,
    Optional<Utf16FlyString> const& binding_name,
    Utf16FlyString const& class_name);

}
