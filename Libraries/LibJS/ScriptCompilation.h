/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Result.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Position.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionKind.h>

namespace JS {

using ProgramType = RustIntegration::ProgramType;

// A function compiled from standalone source text the way CreateDynamicFunction compiles one, for hosts that create
// functions outside of any script, such as event handler content attributes.
class JS_API CompiledDynamicFunction {
public:
    // Fails with a syntax error message if the parameters or the body do not parse.
    static Result<CompiledDynamicFunction, Utf16String> compile(VM&, Utf16View source_text, Utf16View parameters_string, Utf16View body_parse_string, FunctionKind);

    // OrdinaryFunctionCreate over the compiled function, closing over the given environments, with the default
    // prototype for the function's kind and the given [[ScriptOrModule]].
    GC::Ref<FunctionObject> instantiate(Realm&, Environment& scope, GC::Ptr<PrivateEnvironment>, ScriptOrModule) const;

private:
    explicit CompiledDynamicFunction(GC::Ref<SharedFunctionInstanceData>);

    GC::Root<SharedFunctionInstanceData> m_function_data;
};

// Every position in the source where a breakpoint can be set, including positions inside functions that have not been
// compiled yet. This compiles a private copy of the source without touching the VM or the GC heap.
JS_API Vector<Position> breakpoint_positions_for_source(SourceCode const&, ProgramType, size_t line_number_offset);

}
