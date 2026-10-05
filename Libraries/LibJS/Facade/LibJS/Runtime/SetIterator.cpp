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

GC::Ref<SetIterator> SetIterator::create_of_property_kind(Realm& realm, Set& set, u8 property_kind)
{
    VERIFY(property_kind <= JS_PROPERTY_KIND_KEY_AND_VALUE);
    auto* iterator = js_collections_set_iterator_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), object_to_abi(set), property_kind);
    return static_cast<SetIterator&>(object_from_abi(iterator));
}

}
