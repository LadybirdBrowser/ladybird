/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ExecutionContext.h>

namespace JS {

using namespace EmbeddingABI;

static JSExecutionContext const* execution_context_to_abi(ExecutionContext const& execution_context)
{
    return reinterpret_cast<JSExecutionContext const*>(&execution_context);
}

static NonnullOwnPtr<ExecutionContext> adopt_execution_context_from_abi(JSExecutionContext* execution_context)
{
    VERIFY(execution_context);
    return adopt_own(*reinterpret_cast<ExecutionContext*>(execution_context));
}

NonnullOwnPtr<ExecutionContext> ExecutionContext::create(u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count)
{
    return adopt_execution_context_from_abi(js_execution_context_create(registers_and_locals_count, constants.size(), arguments_count));
}

NonnullOwnPtr<ExecutionContext> ExecutionContext::copy() const
{
    return adopt_execution_context_from_abi(js_execution_context_copy(execution_context_to_abi(*this)));
}

void ExecutionContext::operator delete(void* execution_context)
{
    js_execution_context_destroy(static_cast<JSExecutionContext*>(execution_context));
}

void ExecutionContext::visit_edges(GC::Cell::Visitor& visitor)
{
    js_execution_context_visit(execution_context_to_abi(*this), reinterpret_cast<GCVisitor*>(&visitor));
}

SourceCode const* ExecutionContext::source_code() const
{
    return reinterpret_cast<SourceCode const*>(js_source_code_of_execution_context(execution_context_to_abi(*this)));
}

GC::Ptr<Realm> ExecutionContext::realm() const
{
    return cell_from_abi<Realm>(js_execution_context_realm(execution_context_to_abi(*this)));
}

void ExecutionContext::set_realm(GC::Ptr<Realm> realm)
{
    // NB: The VM reads the realm of a context when it starts running.
    VERIFY(!function);
    realm_without_function = realm;
}

ScriptOrModule ExecutionContext::script_or_module() const
{
    return bit_cast<ScriptOrModule>(js_execution_context_script_or_module(execution_context_to_abi(*this)));
}

void ExecutionContext::set_script_or_module(ScriptOrModule script_or_module)
{
    VERIFY(!function);
    script_or_module_without_function = script_or_module;
}

Utf16FlyString ExecutionContext::function_name() const
{
    return owned_utf16_string_from_abi(js_execution_context_function_name(execution_context_to_abi(*this)));
}

}
