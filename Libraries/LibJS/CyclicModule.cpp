/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGC/WeakHashMap.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS {

using namespace EmbeddingABI;

// C++ hands out [[RequestedModules]] by reference, so the facade keeps a copy of each module's, which never change,
// for as long as the module lives.
static GC::WeakHashMap<CyclicModule, NonnullOwnPtr<Vector<ModuleRequest>>>& requested_modules_of_live_cyclic_modules()
{
    static NeverDestroyed<GC::WeakHashMap<CyclicModule, NonnullOwnPtr<Vector<ModuleRequest>>>> requested_modules;
    return *requested_modules;
}

Vector<ModuleRequest> const& CyclicModule::requested_modules() const
{
    auto& requested_modules = requested_modules_of_live_cyclic_modules().ensure(*this, [this] {
        auto* module = module_to_abi(*this);
        auto requested_module_count = js_module_requested_module_count(module);
        auto requests = make<Vector<ModuleRequest>>();
        requests->ensure_capacity(requested_module_count);
        for (size_t index = 0; index < requested_module_count; ++index)
            requests->unchecked_append(module_request_from_abi(*js_module_requested_module(module, index)));
        return requests;
    });
    return *requested_modules;
}

GC::Ref<Module> CyclicModule::get_imported_module(ModuleRequest const& request)
{
    ModuleRequestForABI abi_request { request };
    return module_from_abi(js_module_get_imported_module(module_to_abi(*this), abi_request.ptr()));
}

}
