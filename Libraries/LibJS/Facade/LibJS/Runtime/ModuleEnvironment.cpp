/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<ModuleEnvironment> new_module_environment(GC::Ptr<Environment> outer_environment)
{
    auto* abi_outer_environment = outer_environment ? cell_to_abi<JSEnvironment>(*outer_environment) : nullptr;
    return *cell_from_abi<ModuleEnvironment>(js_environment_new_module_environment(vm_to_abi(VM::the()), abi_outer_environment));
}

}
