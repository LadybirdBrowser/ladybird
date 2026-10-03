/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostClassInternals.h>
#include <LibJS/Runtime/HostModule.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

using namespace HostABI;

GC::Ref<HostModule> HostModule::create(Realm& realm, JSHostClass const& host_class, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined, GC::Ptr<GC::Cell> host_data)
{
    auto module = realm.heap().allocate_with_descriptor(cell_allocator_for_host_class<HostModule>(host_class), realm, host_class, filename, move(requested_modules), host_defined, host_data);
    static_cast<Cell&>(*module).initialize(realm);
    return module;
}

HostModule::HostModule(Realm& realm, JSHostClass const& host_class, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined, GC::Ptr<GC::Cell> host_data)
    : CyclicModule(realm, filename, false, move(requested_modules), host_defined)
    , m_host_class(&host_class)
    , m_host_data(host_data)
{
    VERIFY(host_class.abi_version == JS_HOST_ABI_VERSION && host_class.kind == JS_HOST_CLASS_MODULE);
    VERIFY(host_class.hooks);
    auto const& hooks = this->hooks();
    VERIFY(hooks.get_exported_names && hooks.resolve_export && hooks.initialize_environment && hooks.execute_module);
}

void HostModule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_data);
}

JSHostModuleHooks const& HostModule::hooks() const
{
    return *static_cast<JSHostModuleHooks const*>(m_host_class->hooks);
}

StringView HostModule::class_name() const
{
    return { m_host_class->name, m_host_class->name_length };
}

Vector<Utf16FlyString> HostModule::get_exported_names(VM&, GC::RootHashTable<GC::Ref<Module const>>&)
{
    Vector<Utf16FlyString> exported_names;
    JSStringSink sink {
        .context = &exported_names,
        .append = [](void* context, u16 const* code_units, size_t length_in_code_units) {
            static_cast<Vector<Utf16FlyString>*>(context)->append(string_from_abi(code_units, length_in_code_units));
        },
    };
    hooks().get_exported_names(module_to_abi(this), &sink);
    return exported_names;
}

ResolvedBinding HostModule::resolve_export(VM&, Utf16FlyString const& export_name, Vector<ResolvedBinding>)
{
    Optional<Utf16FlyString> binding_name;
    JSResolvedBinding abi_binding {
        .module = nullptr,
        .binding_name = {
            .context = &binding_name,
            .append = [](void* context, u16 const* code_units, size_t length_in_code_units) {
                auto& binding_name = *static_cast<Optional<Utf16FlyString>*>(context);
                VERIFY(!binding_name.has_value());
                binding_name = string_from_abi(code_units, length_in_code_units);
            },
        },
        .type = JS_RESOLVED_BINDING_NULL,
    };
    with_string_as_abi(export_name, [&](u16 const* code_units, size_t length_in_code_units) {
        hooks().resolve_export(module_to_abi(this), code_units, length_in_code_units, &abi_binding);
    });

    ResolvedBinding binding;
    binding.type = resolved_binding_type_from_abi(abi_binding.type);
    binding.module = module_from_abi(abi_binding.module);
    if (binding.type == ResolvedBinding::BindingName)
        binding.export_name = binding_name.release_value();
    return binding;
}

ThrowCompletionOr<void> HostModule::initialize_environment(VM&)
{
    return completion_from_abi<void>(hooks().initialize_environment(module_to_abi(this)));
}

ThrowCompletionOr<void> HostModule::execute_module(VM&, GC::Ptr<PromiseCapability> capability)
{
    return completion_from_abi<void>(hooks().execute_module(module_to_abi(this), promise_capability_to_abi(capability)));
}

JSHostClass const* host_class_of(Module const& module)
{
    if (auto const* host_module = as_if<HostModule>(module))
        return &host_module->host_class();
    return nullptr;
}

bool is_host_instance_of(Module const& module, JSHostClass const& host_class)
{
    for (auto const* candidate = host_class_of(module); candidate; candidate = candidate->parent) {
        if (candidate == &host_class)
            return true;
    }
    return false;
}

}
