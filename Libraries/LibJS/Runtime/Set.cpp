/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<Set> Set::create(Realm& realm)
{
    return static_cast<Set&>(object_from_abi(js_collections_set_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm))));
}

void Set::set_clear()
{
    js_collections_set_clear(object_to_abi(*this));
}

bool Set::set_remove(Value const& value)
{
    return js_collections_set_remove(object_to_abi(*this), value_to_abi(value));
}

void Set::set_add(Value const& key)
{
    js_collections_set_add(object_to_abi(*this), value_to_abi(key));
}

size_t Set::set_size() const
{
    return js_collections_set_size(object_to_abi(*this));
}

ThrowCompletionOr<void> Set::for_each_value(Function<ThrowCompletionOr<void>(Value)> const& callback) const
{
    using Callback = RemoveReference<decltype(callback)>;
    auto completion = js_collections_set_for_each_value(
        object_to_abi(*this),
        [](void* context, JSValue value) {
            return completion_to_abi((*static_cast<Callback*>(context))(value_from_abi(value)));
        },
        const_cast<void*>(static_cast<void const*>(&callback)));
    return completion_from_abi<void>(completion);
}

}
