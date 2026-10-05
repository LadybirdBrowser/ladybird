/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/CyclicModule.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(static_cast<int>(ResolvedBinding::BindingName) == JS_RESOLVED_BINDING_BINDING_NAME);
static_assert(static_cast<int>(ResolvedBinding::Namespace) == JS_RESOLVED_BINDING_NAMESPACE);
static_assert(static_cast<int>(ResolvedBinding::Ambiguous) == JS_RESOLVED_BINDING_AMBIGUOUS);
static_assert(static_cast<int>(ResolvedBinding::Null) == JS_RESOLVED_BINDING_NULL);

static void set_utf16_fly_string(void* context, u16 const* code_units, size_t length_in_code_units)
{
    auto& string = *static_cast<Utf16FlyString*>(context);
    string = Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length_in_code_units });
}

Realm& Module::realm()
{
    return *cell_from_abi<Realm>(js_module_realm(module_to_abi(*this)));
}

Realm const& Module::realm() const
{
    return *cell_from_abi<Realm>(js_module_realm(module_to_abi(*this)));
}

GC::Ptr<ModuleEnvironment> Module::environment()
{
    return reinterpret_cast<ModuleEnvironment*>(js_module_environment(module_to_abi(*this)));
}

GC::Ptr<GC::Cell> Module::host_defined() const
{
    return static_cast<GC::Cell*>(js_module_host_defined(module_to_abi(*this)));
}

ThrowCompletionOr<void> Module::link(VM& vm)
{
    return completion_from_abi<void>(js_module_link(vm_to_abi(vm), module_to_abi(*this)));
}

ThrowCompletionOr<GC::Ref<PromiseCapability>> Module::evaluate(VM& vm)
{
    return completion_from_abi<GC::Ref<PromiseCapability>>(js_module_evaluate(vm_to_abi(vm), module_to_abi(*this)));
}

ResolvedBinding Module::resolve_export(VM& vm, Utf16FlyString const& export_name, Vector<ResolvedBinding> resolve_set)
{
    VERIFY(resolve_set.is_empty());

    Utf16FlyString binding_name;
    JSResolvedBinding abi_binding {
        .module = nullptr,
        .binding_name = { .context = &binding_name, .append = set_utf16_fly_string },
        .type = JS_RESOLVED_BINDING_NULL,
    };
    js_module_resolve_export(vm_to_abi(vm), module_to_abi(*this), utf16_view_to_abi(export_name.view()), &abi_binding);

    ResolvedBinding binding;
    binding.type = static_cast<ResolvedBinding::Type>(abi_binding.type);
    binding.module = cell_from_abi<Module>(abi_binding.module);
    binding.export_name = move(binding_name);
    return binding;
}

PromiseCapability& Module::load_requested_modules(GC::Ptr<GC::Cell> host_defined)
{
    auto* capability = js_module_load_requested_modules(vm_to_abi(vm()), module_to_abi(*this), host_defined.ptr());
    VERIFY(capability);
    return *reinterpret_cast<PromiseCapability*>(capability);
}

void Module::set_environment(GC::Ref<ModuleEnvironment> environment)
{
    js_host_module_set_environment(module_to_abi(*this), reinterpret_cast<JSEnvironment*>(environment.ptr()));
}

// 16.2.1.10 FinishLoadingImportedModule ( referrer, moduleRequest, payload, result ), https://tc39.es/ecma262/#sec-FinishLoadingImportedModule
void finish_loading_imported_module(ImportedModuleReferrer referrer, ModuleRequest const& module_request, ImportedModulePayload payload, ThrowCompletionOr<GC::Ref<Module>> const& result)
{
    ModuleRequestForABI abi_module_request { module_request };
    js_module_finish_loading_imported_module(
        vm_to_abi(VM::the()),
        imported_module_referrer_to_abi(referrer),
        abi_module_request.ptr(),
        imported_module_payload_to_abi(payload),
        module_completion_to_abi(result));
}

}
