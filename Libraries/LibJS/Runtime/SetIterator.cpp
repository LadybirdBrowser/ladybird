/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<SetIterator> SetIterator::create(Realm& realm, Set& set, Object::PropertyKind iteration_kind)
{
    auto* iterator = js_collections_set_iterator_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), object_to_abi(set), to_underlying(iteration_kind));
    return static_cast<SetIterator&>(object_from_abi(iterator));
}

}
