/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/CyclicModule.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS {

using namespace EmbeddingABI;

Vector<ModuleRequest> CyclicModule::requested_modules() const
{
    auto* module = module_to_abi(*this);
    auto requested_module_count = js_module_requested_module_count(module);
    Vector<ModuleRequest> requests;
    requests.ensure_capacity(requested_module_count);
    for (size_t index = 0; index < requested_module_count; ++index)
        requests.unchecked_append(module_request_from_abi(*js_module_requested_module(module, index)));
    return requests;
}

GC::Ref<Module> CyclicModule::get_imported_module(ModuleRequest const& request)
{
    ModuleRequestForABI abi_request { request };
    return module_from_abi(js_module_get_imported_module(module_to_abi(*this), abi_request.ptr()));
}

}
