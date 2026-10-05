/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/HostModule.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<HostModule> HostModule::create(Realm& realm, JSHostClass const& host_class, StringView filename, Vector<ModuleRequest> requested_modules, GC::Ptr<GC::Cell> host_defined, GC::Ptr<GC::Cell> host_data)
{
    Vector<NonnullOwnPtr<ModuleRequestForABI>> abi_requested_modules;
    Vector<JSModuleRequest const*> abi_requested_module_pointers;
    abi_requested_modules.ensure_capacity(requested_modules.size());
    abi_requested_module_pointers.ensure_capacity(requested_modules.size());
    for (auto const& request : requested_modules) {
        abi_requested_modules.unchecked_append(make<ModuleRequestForABI>(request));
        abi_requested_module_pointers.unchecked_append(abi_requested_modules.last()->ptr());
    }

    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_host_module_create(
        vm_to_abi(realm.vm()),
        realm_to_abi(realm),
        &host_class,
        utf16_view_to_abi(utf16_filename.utf16_view()),
        abi_requested_module_pointers.data(),
        abi_requested_module_pointers.size(),
        host_defined.ptr(),
        host_data.ptr());
    return module_from_abi<HostModule>(module);
}

JSHostClass const& HostModule::host_class() const
{
    auto const* host_class = js_host_module_host_class(module_to_abi(*this));
    VERIFY(host_class);
    return *host_class;
}

GC::Ptr<GC::Cell> HostModule::host_data() const
{
    return static_cast<GC::Cell*>(js_host_module_host_data(module_to_abi(*this)));
}

StringView HostModule::class_name() const
{
    auto const& host_class = this->host_class();
    return { host_class.name, host_class.name_length };
}

JSHostClass const* host_class_of(Module const& module)
{
    return js_host_module_host_class(module_to_abi(module));
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
