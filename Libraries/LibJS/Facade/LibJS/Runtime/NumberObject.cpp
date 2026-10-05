/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<NumberObject> NumberObject::create(Realm& realm, double value)
{
    return static_cast<NumberObject&>(object_from_abi(js_primitive_wrapper_create_number(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), value)));
}

double NumberObject::number() const
{
    return js_primitive_wrapper_number(object_to_abi(*this));
}

}
