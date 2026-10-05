/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<StringObject> StringObject::create(Realm& realm, PrimitiveString& string, Object& prototype)
{
    auto* string_object = js_primitive_wrapper_create_string(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), primitive_string_to_abi(string), object_to_abi(prototype));
    return static_cast<StringObject&>(object_from_abi(string_object));
}

PrimitiveString const& StringObject::primitive_string() const
{
    return const_cast<StringObject&>(*this).primitive_string();
}

PrimitiveString& StringObject::primitive_string()
{
    auto* string = js_primitive_wrapper_string(object_to_abi(*this));
    VERIFY(string);
    return *cell_from_abi<PrimitiveString>(string);
}

}
