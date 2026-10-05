/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

ThrowCompletionOr<void> FinalizationRegistry::cleanup(GC::Ptr<JobCallback> callback)
{
    auto* abi_callback = callback ? cell_to_abi<JSJobCallback>(*callback) : nullptr;
    return completion_from_abi<void>(js_weak_finalization_registry_cleanup(vm_to_abi(vm()), object_to_abi(*this), abi_callback));
}

Realm& FinalizationRegistry::realm()
{
    auto* realm = js_weak_finalization_registry_realm(object_to_abi(*this));
    VERIFY(realm);
    return cell_ref_from_abi<Realm>(realm);
}

Realm const& FinalizationRegistry::realm() const
{
    return const_cast<FinalizationRegistry&>(*this).realm();
}

JobCallback& FinalizationRegistry::cleanup_callback()
{
    auto* cleanup_callback = js_weak_finalization_registry_cleanup_callback(object_to_abi(*this));
    VERIFY(cleanup_callback);
    return cell_ref_from_abi<JobCallback>(cleanup_callback);
}

JobCallback const& FinalizationRegistry::cleanup_callback() const
{
    return const_cast<FinalizationRegistry&>(*this).cleanup_callback();
}

}
