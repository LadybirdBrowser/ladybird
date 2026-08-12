/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>
#include <LibWeb/WebAudio/BindingsGlue.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioParamMap);

GC::Ref<AudioParamMap> AudioParamMap::create()
{
    return GC::Heap::the().allocate<AudioParamMap>();
}

AudioParamMap::AudioParamMap() = default;
AudioParamMap::~AudioParamMap() = default;

void AudioParamMap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& entry : m_entries)
        visitor.visit(entry.value);
}

}

namespace Web::Bindings {

static JS::Value wrapped_param(JS::Realm& realm, GC::Ref<WebAudio::AudioParam> param)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, param);
}

GC::Ref<JS::Map> map_entries(JS::Realm& realm, WebAudio::AudioParamMap& map)
{
    auto map_entries = JS::Map::create(realm);
    for (auto const& entry : map.entries()) {
        auto key = JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(entry.key));
        map_entries->map_set(JS::Value { key.ptr() }, wrapped_param(realm, entry.value));
    }
    return map_entries;
}

Optional<JS::Value> map_get(JS::Realm& realm, WebAudio::AudioParamMap& map, FlyString const& key)
{
    auto it = map.entries().find(key);
    if (it == map.entries().end())
        return {};
    return wrapped_param(realm, it->value);
}

bool map_has(WebAudio::AudioParamMap& map, FlyString const& key)
{
    return map.entries().contains(key);
}

}
