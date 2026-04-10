/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/PropertyNameIterator.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Shape.h>

namespace JS::Bytecode {

GC_DEFINE_ALLOCATOR(PropertyNameIterator);

GC::Ref<PropertyNameIterator> PropertyNameIterator::create(Realm& realm, GC::Ref<Object> object, Vector<PropertyKey> properties, FastPath fast_path, u32 indexed_property_count, GC::Ptr<Shape> shape, GC::Ptr<PrototypeChainValidity> prototype_chain_validity)
{
    return realm.create<PropertyNameIterator>(realm, object, move(properties), fast_path, indexed_property_count, shape, prototype_chain_validity);
}

GC::Ref<PropertyNameIterator> PropertyNameIterator::create(Realm& realm, GC::Ref<Object> object, ObjectPropertyIteratorCacheData& property_cache, ObjectPropertyIteratorCache* iterator_cache_slot)
{
    VERIFY(property_cache.fast_path() != FastPath::None);
    return realm.create<PropertyNameIterator>(realm, object, property_cache, iterator_cache_slot);
}

ThrowCompletionOr<void> PropertyNameIterator::next(VM& vm, bool& done, Value& value)
{
    VERIFY(m_object);

    while (true) {
        if (m_next_indexed_property < m_indexed_property_count) {
            auto current_index = m_next_indexed_property++;
            auto entry = PropertyKey { current_index };

            if (m_fast_path != FastPath::None && !fast_path_still_valid())
                disable_fast_path();

            if (m_fast_path == FastPath::None && !TRY(m_object->has_property(entry)))
                continue;

            done = false;
            if (m_fast_path != FastPath::None && m_property_cache)
                // Cache-backed iterators keep the return values pre-materialized
                // so the asm fast path can hand back keys without converting a
                // PropertyKey on every iteration.
                value = property_value_list()[current_index];
            else
                value = entry.to_value(vm);
            return {};
        }

        auto properties = property_list();
        if (m_next_property >= properties.size()) {
            if (m_iterator_cache_slot) {
                // Once exhausted, hand the iterator object back to the bytecode
                // site cache so the next execution of the same loop can reuse it.
                m_object = nullptr;
                m_iterator_cache_slot->reusable_property_name_iterator = this;
                m_iterator_cache_slot = nullptr;
            }
            done = true;
            return {};
        }

        auto current_index = m_next_property++;
        auto const& entry = properties[current_index];

        if (m_fast_path != FastPath::None && !fast_path_still_valid())
            disable_fast_path();

        // If the property is deleted, don't include it (invariant no. 2)
        if (m_fast_path == FastPath::None && !TRY(m_object->has_property(entry)))
            continue;

        done = false;
        if (m_fast_path != FastPath::None && m_property_cache)
            value = property_value_list()[m_indexed_property_count + current_index];
        else
            value = entry.to_value(vm);
        return {};
    }
}

void PropertyNameIterator::reset_with_cache_data(GC::Ref<Object> object, ObjectPropertyIteratorCacheData& property_cache, ObjectPropertyIteratorCache* iterator_cache_slot)
{
    VERIFY(property_cache.fast_path() != FastPath::None);
    m_object = object;
    m_owned_properties.clear();
    m_property_cache = &property_cache;
    m_shape = property_cache.shape();
    m_prototype_chain_validity = property_cache.prototype_chain_validity();
    m_indexed_property_count = property_cache.indexed_property_count();
    m_next_indexed_property = 0;
    m_next_property = 0;
    m_shape_is_dictionary = property_cache.shape()->is_dictionary();
    m_shape_dictionary_generation = property_cache.shape_dictionary_generation();
    m_fast_path = property_cache.fast_path();
    m_iterator_cache_slot = iterator_cache_slot;
    VERIFY(m_property_cache);
    VERIFY(m_shape);
}

PropertyNameIterator::PropertyNameIterator(Realm& realm, GC::Ref<Object> object, Vector<PropertyKey> properties, FastPath fast_path, u32 indexed_property_count, GC::Ptr<Shape> shape, GC::Ptr<PrototypeChainValidity> prototype_chain_validity)
    : Object(realm, nullptr)
    , m_object(object)
    , m_owned_properties(move(properties))
    , m_shape(shape)
    , m_prototype_chain_validity(prototype_chain_validity)
    , m_indexed_property_count(indexed_property_count)
    , m_fast_path(fast_path)
{
    if (m_shape)
        m_shape_is_dictionary = m_shape->is_dictionary();
    if (m_shape_is_dictionary)
        m_shape_dictionary_generation = m_shape->dictionary_generation();
}

PropertyNameIterator::PropertyNameIterator(Realm& realm, GC::Ref<Object> object, ObjectPropertyIteratorCacheData& property_cache, ObjectPropertyIteratorCache* iterator_cache_slot)
    : Object(realm, nullptr)
    , m_object(object)
    , m_property_cache(&property_cache)
    , m_shape(property_cache.shape())
    , m_prototype_chain_validity(property_cache.prototype_chain_validity())
    , m_iterator_cache_slot(iterator_cache_slot)
    , m_indexed_property_count(property_cache.indexed_property_count())
    , m_shape_is_dictionary(property_cache.shape()->is_dictionary())
    , m_shape_dictionary_generation(property_cache.shape_dictionary_generation())
    , m_fast_path(property_cache.fast_path())
{
    VERIFY(m_fast_path != FastPath::None);
    VERIFY(m_property_cache);
    VERIFY(m_shape);
}

ReadonlySpan<PropertyKey> PropertyNameIterator::property_list() const
{
    if (m_property_cache)
        return m_property_cache->properties();
    return m_owned_properties.span();
}

ReadonlySpan<Value> PropertyNameIterator::property_value_list() const
{
    VERIFY(m_property_cache);
    return m_property_cache->property_values();
}

bool PropertyNameIterator::fast_path_still_valid() const
{
    VERIFY(m_object);
    VERIFY(m_shape);

    // We revalidate on every next() call so active enumeration can deopt if the
    // receiver or prototype chain changes underneath us. After deopting, the
    // iterator resumes with has_property() checks for the remaining snapshot.
    auto& shape = m_object->shape();
    if (&shape != m_shape)
        return false;

    if (m_shape_is_dictionary && shape.dictionary_generation() != m_shape_dictionary_generation)
        return false;

    if (m_fast_path == FastPath::PackedIndexed) {
        if (m_object->indexed_storage_kind() != IndexedStorageKind::Packed)
            return false;
        if (m_object->indexed_array_like_size() != m_indexed_property_count)
            return false;
    }

    if (m_prototype_chain_validity && !m_prototype_chain_validity->is_valid())
        return false;

    return true;
}

void PropertyNameIterator::disable_fast_path()
{
    m_fast_path = FastPath::None;
    m_shape = nullptr;
    m_prototype_chain_validity = nullptr;
    m_shape_is_dictionary = false;
    m_shape_dictionary_generation = 0;
}

void PropertyNameIterator::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_object);
    visitor.visit(m_property_cache);
    visitor.visit(m_shape);
    visitor.visit(m_prototype_chain_validity);
    for (auto& key : m_owned_properties)
        key.visit_edges(visitor);
}

}
