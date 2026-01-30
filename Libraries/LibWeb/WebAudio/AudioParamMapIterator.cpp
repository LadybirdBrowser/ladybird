/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/AudioParamMapIteratorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMapIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<AudioParamMapIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = realm.create<AudioParamMapIteratorPrototype>(realm);
    m_prototypes.set("AudioParamMapIterator"_fly_string, prototype);
}

}

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioParamMapIterator);

GC::Ref<AudioParamMapIterator> AudioParamMapIterator::create(AudioParamMap const& map, JS::Object::PropertyKind iteration_kind)
{
    return map.realm().create<AudioParamMapIterator>(map, iteration_kind);
}

AudioParamMapIterator::AudioParamMapIterator(AudioParamMap const& map, JS::Object::PropertyKind iteration_kind)
    : PlatformObject(map.realm())
    , m_map(map)
    , m_iteration_kind(iteration_kind)
{
}

AudioParamMapIterator::~AudioParamMapIterator() = default;

void AudioParamMapIterator::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioParamMapIterator);
    Base::initialize(realm);
}

void AudioParamMapIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_map);
}

JS::Object* AudioParamMapIterator::next()
{
    auto& vm = this->vm();

    if (m_index >= m_map->m_entries.size())
        return create_iterator_result_object(vm, JS::js_undefined(), true);

    auto& entry = m_map->m_entries[m_index++];

    if (m_iteration_kind == JS::Object::PropertyKind::Key)
        return create_iterator_result_object(vm, JS::PrimitiveString::create(vm, entry.key), false);

    auto entry_value = JS::Value(entry.value);

    if (m_iteration_kind == JS::Object::PropertyKind::Value)
        return create_iterator_result_object(vm, entry_value, false);

    return create_iterator_result_object(vm, JS::Array::create_from(realm(), { JS::PrimitiveString::create(vm, entry.key), entry_value }), false);
}

}
