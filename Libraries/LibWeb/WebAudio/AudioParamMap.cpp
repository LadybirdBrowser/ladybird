/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioParamMap);

GC::Ref<AudioParamMap> AudioParamMap::create(JS::Realm& realm)
{
    return realm.create<AudioParamMap>(realm);
}

AudioParamMap::AudioParamMap(JS::Realm& realm)
    : PlatformObject(realm)
{
}

AudioParamMap::~AudioParamMap() = default;

void AudioParamMap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioParamMap);
    Base::initialize(realm);
}

void AudioParamMap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_entries)
        visitor.visit(entry.value);
}

size_t AudioParamMap::size() const
{
    return m_entries.size();
}

GC::Ptr<AudioParam> AudioParamMap::get(String const& key)
{
    for (auto& entry : m_entries) {
        if (entry.key == key)
            return entry.value;
    }

    return {};
}

bool AudioParamMap::has(String const& key)
{
    return get(key) != nullptr;
}

void AudioParamMap::set(FlyString key, GC::Ref<AudioParam> value)
{
    for (auto& entry : m_entries) {
        if (entry.key == key) {
            entry.value = value;
            return;
        }
    }

    m_entries.append(Entry { .key = move(key), .value = value });
}

JS::ThrowCompletionOr<void> AudioParamMap::for_each(ForEachCallback callback)
{
    for (auto& entry : m_entries)
        TRY(callback(entry.key, entry.value));
    return {};
}

}
