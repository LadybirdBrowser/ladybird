/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/DeferGC.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

JS_DEFINE_ALLOCATOR(Shape);
JS_DEFINE_ALLOCATOR(PrototypeChainValidity);

static HashTable<JS::GCPtr<Shape>> s_all_prototype_shapes;

Shape::~Shape()
{
    if (m_is_prototype_shape)
        s_all_prototype_shapes.remove(this);
}

NonnullGCPtr<Shape> Shape::create_cacheable_dictionary_transition()
{
    auto new_shape = heap().allocate_without_realm<Shape>(m_realm);
    new_shape->m_dictionary = true;
    new_shape->m_cacheable = true;
    new_shape->m_prototype = m_prototype;
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    ensure_property_table();
    new_shape->ensure_property_table();
    (*new_shape->m_property_table) = *m_property_table;
    new_shape->m_property_count = new_shape->m_property_table->size();
    return new_shape;
}

NonnullGCPtr<Shape> Shape::create_uncacheable_dictionary_transition()
{
    auto new_shape = heap().allocate_without_realm<Shape>(m_realm);
    new_shape->m_dictionary = true;
    new_shape->m_cacheable = true;
    new_shape->m_prototype = m_prototype;
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    ensure_property_table();
    new_shape->ensure_property_table();
    (*new_shape->m_property_table) = *m_property_table;
    new_shape->m_property_count = new_shape->m_property_table->size();
    return new_shape;
}

GCPtr<Shape> Shape::get_or_prune_cached_forward_transition(TransitionKey const& key)
{
    if (m_is_prototype_shape)
        return nullptr;
    if (!m_forward_transitions)
        return nullptr;
    auto it = m_forward_transitions->find(key);
    if (it == m_forward_transitions->end())
        return nullptr;
    if (!it->value) {
        // The cached forward transition has gone stale (from garbage collection). Prune it.
        m_forward_transitions->remove(it);
        return nullptr;
    }
    return it->value.ptr();
}

GCPtr<Shape> Shape::get_or_prune_cached_delete_transition(StringOrSymbol const& key)
{
    if (m_is_prototype_shape)
        return nullptr;
    if (!m_delete_transitions)
        return nullptr;
    auto it = m_delete_transitions->find(key);
    if (it == m_delete_transitions->end())
        return nullptr;
    if (!it->value) {
        // The cached delete transition has gone stale (from garbage collection). Prune it.
        m_delete_transitions->remove(it);
        return nullptr;
    }
    return it->value.ptr();
}

GCPtr<Shape> Shape::get_or_prune_cached_prototype_transition(Object* prototype)
{
    if (m_is_prototype_shape)
        return nullptr;
    if (!m_prototype_transitions)
        return nullptr;
    auto it = m_prototype_transitions->find(prototype);
    if (it == m_prototype_transitions->end())
        return nullptr;
    if (!it->value) {
        // The cached prototype transition has gone stale (from garbage collection). Prune it.
        m_prototype_transitions->remove(it);
        return nullptr;
    }
    return it->value.ptr();
}

NonnullGCPtr<Shape> Shape::create_put_transition(StringOrSymbol const& property_key, PropertyAttributes attributes)
{
    TransitionKey key { property_key, attributes };
    if (auto existing_shape = get_or_prune_cached_forward_transition(key))
        return *existing_shape;
    auto new_shape = heap().allocate_without_realm<Shape>(*this, property_key, attributes, TransitionType::Put);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_forward_transitions)
            m_forward_transitions = make<HashMap<TransitionKey, WeakPtr<Shape>>>();
        m_forward_transitions->set(key, new_shape.ptr());
    }
    return new_shape;
}

NonnullGCPtr<Shape> Shape::create_configure_transition(StringOrSymbol const& property_key, PropertyAttributes attributes)
{
    TransitionKey key { property_key, attributes };
    if (auto existing_shape = get_or_prune_cached_forward_transition(key))
        return *existing_shape;
    auto new_shape = heap().allocate_without_realm<Shape>(*this, property_key, attributes, TransitionType::Configure);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_forward_transitions)
            m_forward_transitions = make<HashMap<TransitionKey, WeakPtr<Shape>>>();
        m_forward_transitions->set(key, new_shape.ptr());
    }
    return new_shape;
}

NonnullGCPtr<Shape> Shape::create_prototype_transition(Object* new_prototype)
{
    if (new_prototype)
        new_prototype->convert_to_prototype_if_needed();
    if (auto existing_shape = get_or_prune_cached_prototype_transition(new_prototype))
        return *existing_shape;
    auto new_shape = heap().allocate_without_realm<Shape>(*this, new_prototype);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_prototype_transitions)
            m_prototype_transitions = make<HashMap<GCPtr<Object>, WeakPtr<Shape>>>();
        m_prototype_transitions->set(new_prototype, new_shape.ptr());
    }
    return new_shape;
}

Shape::Shape(Realm& realm)
    : m_realm(realm)
{
}

Shape::Shape(Shape& previous_shape, StringOrSymbol const& property_key, PropertyAttributes attributes, TransitionType transition_type)
    : m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_property_key(property_key)
    , m_prototype(previous_shape.m_prototype)
    , m_property_count(transition_type == TransitionType::Put ? previous_shape.m_property_count + 1 : previous_shape.m_property_count)
    , m_attributes(attributes)
    , m_transition_type(transition_type)
{
}

Shape::Shape(Shape& previous_shape, StringOrSymbol const& property_key, TransitionType transition_type)
    : m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_property_key(property_key)
    , m_prototype(previous_shape.m_prototype)
    , m_property_count(previous_shape.m_property_count - 1)
    , m_transition_type(transition_type)
{
    VERIFY(transition_type == TransitionType::Delete);
}

Shape::Shape(Shape& previous_shape, Object* new_prototype)
    : m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_prototype(new_prototype)
    , m_property_count(previous_shape.m_property_count)
    , m_transition_type(TransitionType::Prototype)
{
}

void Shape::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_prototype);
    visitor.visit(m_previous);
    m_property_key.visit_edges(visitor);

    // NOTE: We don't need to mark the keys in the property table, since they are guaranteed
    //       to also be marked by the chain of shapes leading up to this one.

    visitor.ignore(m_prototype_transitions);

    // FIXME: The forward transition keys should be weak, but we have to mark them for now in case they go stale.
    if (m_forward_transitions) {
        for (auto& it : *m_forward_transitions)
            it.key.property_key.visit_edges(visitor);
    }

    // FIXME: The delete transition keys should be weak, but we have to mark them for now in case they go stale.
    if (m_delete_transitions) {
        for (auto& it : *m_delete_transitions)
            it.key.visit_edges(visitor);
    }

    visitor.visit(m_prototype_chain_validity);
}

Optional<PropertyMetadata> Shape::lookup(StringOrSymbol const& property_key) const
{
    if (m_property_count == 0)
        return {};
    auto property = property_table().get(property_key);
    if (!property.has_value())
        return {};
    return property;
}

FLATTEN OrderedHashMap<StringOrSymbol, PropertyMetadata> const& Shape::property_table() const
{
    ensure_property_table();
    return *m_property_table;
}

void Shape::ensure_property_table() const
{
    if (m_property_table)
        return;
    m_property_table = make<OrderedHashMap<StringOrSymbol, PropertyMetadata>>();

    u32 next_offset = 0;

    Vector<Shape const&, 64> transition_chain;
    transition_chain.append(*this);
    for (auto shape = m_previous; shape; shape = shape->m_previous) {
        if (shape->m_property_table) {
            *m_property_table = *shape->m_property_table;
            next_offset = shape->m_property_count;
            break;
        }
        transition_chain.append(*shape);
    }

    for (auto const& shape : transition_chain.in_reverse()) {
        if (!shape.m_property_key.is_valid()) {
            // Ignore prototype transitions as they don't affect the key map.
            continue;
        }
        if (shape.m_transition_type == TransitionType::Put) {
            m_property_table->set(shape.m_property_key, { next_offset++, shape.m_attributes });
        } else if (shape.m_transition_type == TransitionType::Configure) {
            auto it = m_property_table->find(shape.m_property_key);
            VERIFY(it != m_property_table->end());
            it->value.attributes = shape.m_attributes;
        } else if (shape.m_transition_type == TransitionType::Delete) {
            auto remove_it = m_property_table->find(shape.m_property_key);
            VERIFY(remove_it != m_property_table->end());
            auto removed_offset = remove_it->value.offset;
            m_property_table->remove(remove_it);
            for (auto& it : *m_property_table) {
                if (it.value.offset > removed_offset)
                    --it.value.offset;
            }
            --next_offset;
        }
    }
}

NonnullGCPtr<Shape> Shape::create_delete_transition(StringOrSymbol const& property_key)
{
    if (auto existing_shape = get_or_prune_cached_delete_transition(property_key))
        return *existing_shape;
    auto new_shape = heap().allocate_without_realm<Shape>(*this, property_key, TransitionType::Delete);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_delete_transitions)
        m_delete_transitions = make<HashMap<StringOrSymbol, WeakPtr<Shape>>>();
    m_delete_transitions->set(property_key, new_shape.ptr());
    return new_shape;
}

void Shape::add_property_without_transition(StringOrSymbol const& property_key, PropertyAttributes attributes)
{
    VERIFY(property_key.is_valid());
    ensure_property_table();
    if (m_property_table->set(property_key, { m_property_count, attributes }) == AK::HashSetResult::InsertedNewEntry) {
        VERIFY(m_property_count < NumericLimits<u32>::max());
        ++m_property_count;
    }
}

FLATTEN void Shape::add_property_without_transition(PropertyKey const& property_key, PropertyAttributes attributes)
{
    VERIFY(property_key.is_valid());
    add_property_without_transition(property_key.to_string_or_symbol(), attributes);
}

void Shape::set_property_attributes_without_transition(StringOrSymbol const& property_key, PropertyAttributes attributes)
{
    VERIFY(is_dictionary());
    VERIFY(m_property_table);
    auto it = m_property_table->find(property_key);
    VERIFY(it != m_property_table->end());
    it->value.attributes = attributes;
    m_property_table->set(property_key, it->value);
}

void Shape::remove_property_without_transition(StringOrSymbol const& property_key, u32 offset)
{
    VERIFY(is_uncacheable_dictionary());
    VERIFY(m_property_table);
    if (m_property_table->remove(property_key))
        --m_property_count;
    for (auto& it : *m_property_table) {
        VERIFY(it.value.offset != offset);
        if (it.value.offset > offset)
            --it.value.offset;
    }
}

NonnullGCPtr<Shape> Shape::create_for_prototype(NonnullGCPtr<Realm> realm, GCPtr<Object> prototype)
{
    auto new_shape = realm->heap().allocate_without_realm<Shape>(realm);
    s_all_prototype_shapes.set(new_shape);
    new_shape->m_is_prototype_shape = true;
    new_shape->m_prototype = prototype;
    new_shape->m_prototype_chain_validity = realm->heap().allocate_without_realm<PrototypeChainValidity>();
    return new_shape;
}

NonnullGCPtr<Shape> Shape::clone_for_prototype()
{
    VERIFY(!m_is_prototype_shape);
    VERIFY(!m_prototype_chain_validity);
    auto new_shape = heap().allocate_without_realm<Shape>(m_realm);
    s_all_prototype_shapes.set(new_shape);
    new_shape->m_is_prototype_shape = true;
    new_shape->m_prototype = m_prototype;
    ensure_property_table();
    new_shape->ensure_property_table();
    (*new_shape->m_property_table) = *m_property_table;
    new_shape->m_property_count = new_shape->m_property_table->size();
    new_shape->m_prototype_chain_validity = heap().allocate_without_realm<PrototypeChainValidity>();
    return new_shape;
}

void Shape::set_prototype_without_transition(Object* new_prototype)
{
    VERIFY(new_prototype);
    new_prototype->convert_to_prototype_if_needed();
    m_prototype = new_prototype;
}

void Shape::set_prototype_shape()
{
    VERIFY(!m_is_prototype_shape);
    s_all_prototype_shapes.set(this);
    m_is_prototype_shape = true;
    m_prototype_chain_validity = heap().allocate_without_realm<PrototypeChainValidity>();
}

void Shape::invalidate_prototype_if_needed_for_new_prototype(NonnullGCPtr<Shape> new_prototype_shape)
{
    if (!m_is_prototype_shape)
        return;
    new_prototype_shape->set_prototype_shape();
    m_prototype_chain_validity->set_valid(false);

    invalidate_all_prototype_chains_leading_to_this();
}

void Shape::invalidate_all_prototype_chains_leading_to_this()
{
    HashTable<Shape*> shapes_to_invalidate;
    for (auto& candidate : s_all_prototype_shapes) {
        if (!candidate->m_prototype)
            continue;
        for (auto* current_prototype_shape = &candidate->m_prototype->shape(); current_prototype_shape; current_prototype_shape = current_prototype_shape->prototype() ? &current_prototype_shape->prototype()->shape() : nullptr) {
            if (current_prototype_shape == this) {
                VERIFY(candidate->m_is_prototype_shape);
                shapes_to_invalidate.set(candidate);
                break;
            }
        }
    }
    if (shapes_to_invalidate.is_empty())
        return;
    for (auto* shape : shapes_to_invalidate) {
        shape->m_prototype_chain_validity->set_valid(false);
        shape->m_prototype_chain_validity = heap().allocate_without_realm<PrototypeChainValidity>();
    }
}

}
