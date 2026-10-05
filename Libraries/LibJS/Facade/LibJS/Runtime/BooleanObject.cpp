/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<BooleanObject> BooleanObject::create(Realm& realm, bool value)
{
    return static_cast<BooleanObject&>(object_from_abi(js_primitive_wrapper_create_boolean(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), value)));
}

bool BooleanObject::boolean() const
{
    return js_primitive_wrapper_boolean(object_to_abi(*this));
}

}
