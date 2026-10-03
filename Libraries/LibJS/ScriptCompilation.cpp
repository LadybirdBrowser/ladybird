/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/ScriptCompilation.h>

namespace JS {

CompiledDynamicFunction::CompiledDynamicFunction(GC::Ref<SharedFunctionInstanceData> function_data)
    : m_function_data(function_data)
{
}

Result<CompiledDynamicFunction, Utf16String> CompiledDynamicFunction::compile(VM& vm, Utf16View source_text, Utf16View parameters_string, Utf16View body_parse_string, FunctionKind kind)
{
    auto compilation = RustIntegration::compile_dynamic_function(vm, source_text, parameters_string, body_parse_string, kind);
    if (!compilation.has_value())
        return "Failed to compile dynamic function"_utf16;
    if (compilation->is_error())
        return compilation->release_error();
    return CompiledDynamicFunction { compilation->value() };
}

GC::Ref<FunctionObject> CompiledDynamicFunction::instantiate(Realm& realm, Environment& scope, GC::Ptr<PrivateEnvironment> private_environment, ScriptOrModule script_or_module) const
{
    auto function = ECMAScriptFunctionObject::create_from_function_data(realm, *m_function_data, scope, private_environment);
    function->set_script_or_module(move(script_or_module));
    return function;
}

}
