/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/Array.h>

namespace JS {

using namespace EmbeddingABI;

// 10.4.2.2 ArrayCreate ( length [ , proto ] ), https://tc39.es/ecma262/#sec-arraycreate
ThrowCompletionOr<GC::Ref<Array>> Array::create(Realm& realm, u64 length, GC::Ptr<Object> prototype)
{
    return completion_from_abi<GC::Ref<Array>>(js_array_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), length, optional_object_to_abi(prototype.ptr())));
}

// 7.3.18 CreateArrayFromList ( elements ), https://tc39.es/ecma262/#sec-createarrayfromlist
GC::Ref<Array> Array::create_from(Realm& realm, ReadonlySpan<Value> elements)
{
    static_assert(sizeof(Value) == sizeof(JSValue));
    auto* array = js_array_create_from(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), reinterpret_cast<JSValue const*>(elements.data()), elements.size());
    return static_cast<Array&>(object_from_abi(array));
}

}
