/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<Map> Map::create(Realm& realm)
{
    return static_cast<Map&>(object_from_abi(js_collections_map_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm))));
}

void Map::map_clear()
{
    js_collections_map_clear(object_to_abi(*this));
}

bool Map::map_remove(Value const& key)
{
    return js_collections_map_remove(object_to_abi(*this), value_to_abi(key));
}

void Map::map_set(Value const& key, Value value)
{
    js_collections_map_set(object_to_abi(*this), value_to_abi(key), value_to_abi(value));
}

size_t Map::map_size() const
{
    return js_collections_map_size(object_to_abi(*this));
}

ThrowCompletionOr<void> Map::for_each_entry(Function<ThrowCompletionOr<void>(Value key, Value value)> const& callback) const
{
    using Callback = RemoveReference<decltype(callback)>;
    auto completion = js_collections_map_for_each_entry(
        object_to_abi(*this),
        [](void* context, JSValue key, JSValue value) {
            return completion_to_abi((*static_cast<Callback*>(context))(value_from_abi(key), value_from_abi(value)));
        },
        const_cast<void*>(static_cast<void const*>(&callback)));
    return completion_from_abi<void>(completion);
}

}
