/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(Environment::InitializeBindingHint::Normal) == JS_INITIALIZE_BINDING_HINT_NORMAL);
static_assert(to_underlying(Environment::InitializeBindingHint::SyncDispose) == JS_INITIALIZE_BINDING_HINT_SYNC_DISPOSE);
static_assert(to_underlying(Environment::InitializeBindingHint::AsyncDispose) == JS_INITIALIZE_BINDING_HINT_ASYNC_DISPOSE);

static_assert(to_underlying(Environment::EngineEnvironmentKind::Declarative) == JS_ENVIRONMENT_KIND_DECLARATIVE);
static_assert(to_underlying(Environment::EngineEnvironmentKind::Function) == JS_ENVIRONMENT_KIND_FUNCTION);
static_assert(to_underlying(Environment::EngineEnvironmentKind::Module) == JS_ENVIRONMENT_KIND_MODULE);
static_assert(to_underlying(Environment::EngineEnvironmentKind::Global) == JS_ENVIRONMENT_KIND_GLOBAL);
static_assert(to_underlying(Environment::EngineEnvironmentKind::Object) == JS_ENVIRONMENT_KIND_OBJECT);

static JSEnvironment* environment_to_abi(Environment const& environment)
{
    return cell_to_abi<JSEnvironment>(environment);
}

static JSUtf16View binding_name_to_abi(Utf16FlyString const& name)
{
    return utf16_view_to_abi(name.view());
}

ThrowCompletionOr<void> Environment::create_immutable_binding(VM& vm, Utf16FlyString const& name, bool strict)
{
    return completion_from_abi<void>(js_environment_create_immutable_binding(vm_to_abi(vm), environment_to_abi(*this), binding_name_to_abi(name), strict));
}

ThrowCompletionOr<void> Environment::initialize_binding(VM& vm, Utf16FlyString const& name, Value value, InitializeBindingHint hint)
{
    return completion_from_abi<void>(js_environment_initialize_binding(vm_to_abi(vm), environment_to_abi(*this), binding_name_to_abi(name), value_to_abi(value), to_underlying(hint)));
}

ThrowCompletionOr<void> Environment::set_mutable_binding(VM& vm, Utf16FlyString const& name, Value value, bool strict)
{
    return completion_from_abi<void>(js_environment_set_mutable_binding(vm_to_abi(vm), environment_to_abi(*this), binding_name_to_abi(name), value_to_abi(value), strict));
}

ThrowCompletionOr<Value> Environment::get_binding_value(VM& vm, Utf16FlyString const& name, bool strict)
{
    return completion_from_abi<Value>(js_environment_get_binding_value(vm_to_abi(vm), environment_to_abi(*this), binding_name_to_abi(name), strict));
}

ThrowCompletionOr<bool> Environment::delete_binding(VM& vm, Utf16FlyString const& name)
{
    return completion_from_abi<bool>(js_environment_delete_binding(vm_to_abi(vm), environment_to_abi(*this), binding_name_to_abi(name)));
}

Environment* Environment::outer_environment()
{
    return cell_from_abi<Environment>(js_environment_outer(environment_to_abi(*this)));
}

Environment const* Environment::outer_environment() const
{
    return cell_from_abi<Environment>(js_environment_outer(environment_to_abi(*this)));
}

Environment::EngineEnvironmentKind Environment::engine_environment_kind() const
{
    return static_cast<EngineEnvironmentKind>(js_environment_kind(environment_to_abi(*this)));
}

}
