/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/DeferGC.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Shape);
GC_DEFINE_ALLOCATOR(PrototypeChainValidity);

Shape::~Shape() = default;

size_t Shape::external_memory_size() const
{
    size_t size = 0;
    if (m_property_table)
        size += hash_map_external_memory_size(*m_property_table);
    if (m_forward_transitions)
        size += hash_map_external_memory_size(*m_forward_transitions);
    if (m_prototype_transitions)
        size += hash_map_external_memory_size(*m_prototype_transitions);
    if (m_delete_transitions)
        size += hash_map_external_memory_size(*m_delete_transitions);
    if (m_child_prototype_shapes)
        size += vector_external_memory_size(*m_child_prototype_shapes);
    return size;
}

GC::Ref<Shape> Shape::create_dictionary_transition()
{
    auto new_shape = heap().allocate<Shape>(m_realm);
    new_shape->m_dictionary = true;
    new_shape->m_has_parameter_map = m_has_parameter_map;
    new_shape->m_prototype = m_prototype;
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    ensure_property_table();
    new_shape->ensure_property_table();
    (*new_shape->m_property_table) = *m_property_table;
    new_shape->m_property_count = new_shape->m_property_table->size();
    return new_shape;
}

GC::Ptr<Shape> Shape::get_or_prune_cached_forward_transition(TransitionKey const& key)
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

GC::Ptr<Shape> Shape::get_or_prune_cached_delete_transition(PropertyKey const& key)
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

GC::Ptr<Shape> Shape::get_or_prune_cached_prototype_transition(Object* prototype)
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

GC::Ref<Shape> Shape::create_put_transition(PropertyKey const& property_key, PropertyAttributes attributes)
{
    TransitionKey key { property_key, attributes };
    if (auto existing_shape = get_or_prune_cached_forward_transition(key))
        return *existing_shape;
    auto new_shape = heap().allocate<Shape>(*this, property_key, attributes, TransitionType::Put);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_forward_transitions)
            m_forward_transitions = make<HashMap<TransitionKey, GC::Weak<Shape>>>();
        m_forward_transitions->set(key, new_shape);
    }
    return new_shape;
}

GC::Ref<Shape> Shape::create_configure_transition(PropertyKey const& property_key, PropertyAttributes attributes)
{
    TransitionKey key { property_key, attributes };
    if (auto existing_shape = get_or_prune_cached_forward_transition(key))
        return *existing_shape;
    auto new_shape = heap().allocate<Shape>(*this, property_key, attributes, TransitionType::Configure);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_forward_transitions)
            m_forward_transitions = make<HashMap<TransitionKey, GC::Weak<Shape>>>();
        m_forward_transitions->set(key, new_shape.ptr());
    }
    return new_shape;
}

GC::Ref<Shape> Shape::create_prototype_transition(Object* new_prototype)
{
    if (new_prototype)
        new_prototype->convert_to_prototype_if_needed();
    if (auto existing_shape = get_or_prune_cached_prototype_transition(new_prototype))
        return *existing_shape;
    auto new_shape = heap().allocate<Shape>(*this, new_prototype);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_is_prototype_shape) {
        if (!m_prototype_transitions)
            m_prototype_transitions = make<HashMap<GC::Ptr<Object>, GC::Weak<Shape>>>();
        m_prototype_transitions->set(new_prototype, new_shape.ptr());
    }
    return new_shape;
}

Shape::Shape(Realm& realm)
    : m_realm(realm)
{
}

Shape::Shape(Shape& previous_shape, PropertyKey const& property_key, PropertyAttributes attributes, TransitionType transition_type)
    : m_attributes(attributes)
    , m_transition_type(transition_type)
    , m_has_parameter_map(previous_shape.m_has_parameter_map)
    , m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_property_key(property_key)
    , m_prototype(previous_shape.m_prototype)
    , m_property_count(transition_type == TransitionType::Put ? previous_shape.m_property_count + 1 : previous_shape.m_property_count)
{
}

Shape::Shape(Shape& previous_shape, PropertyKey const& property_key, TransitionType transition_type)
    : m_transition_type(transition_type)
    , m_has_parameter_map(previous_shape.m_has_parameter_map)
    , m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_property_key(property_key)
    , m_prototype(previous_shape.m_prototype)
    , m_property_count(previous_shape.m_property_count - 1)
{
    VERIFY(transition_type == TransitionType::Delete);
}

Shape::Shape(Shape& previous_shape, Object* new_prototype)
    : m_transition_type(TransitionType::Prototype)
    , m_has_parameter_map(previous_shape.m_has_parameter_map)
    , m_realm(previous_shape.m_realm)
    , m_previous(&previous_shape)
    , m_prototype(new_prototype)
    , m_property_count(previous_shape.m_property_count)
{
}

void Shape::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_prototype);
    visitor.visit(m_previous);
    if (m_property_key.has_value())
        m_property_key->visit_edges(visitor);

    visitor.ignore(m_prototype_transitions);

    // Child prototype-shape weak refs need no marking; pruning is lazy.
    visitor.ignore(m_child_prototype_shapes);

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

    // Only dictionary shapes actually need us to mark the keys in m_property_table.
    //
    // For non-dictionary shapes, m_property_table is a lazily-built cache of the
    // transition chain: every key it contains was originally inserted into some
    // ancestor's m_property_key, and that ancestor is kept alive by m_previous
    // (which we already visit above). So those keys are guaranteed to be marked
    // transitively via the chain, and re-marking them here is pure overhead.
    //
    // The exception used to be the handful of intrinsic shapes populated via
    // add_property_without_transition() in Intrinsics.cpp (iterator-result,
    // function, arguments, regexp-exec-array, ...). Those shapes are not
    // dictionaries and have no m_previous to reach their property keys through.
    // However, every key they hold is a vm.names.* string or a well-known
    // symbol, both of which are strongly rooted by the VM for its entire
    // lifetime, so skipping them here is safe.
    if (m_dictionary && m_property_table) {
        for (auto& it : *m_property_table)
            it.key.visit_edges(visitor);
    }
}

Optional<PropertyMetadata> Shape::lookup(PropertyKey const& property_key) const
{
    if (m_property_count == 0)
        return {};
    auto property = property_table().get(property_key);
    if (!property.has_value())
        return {};
    return property;
}

FLATTEN OrderedHashMap<PropertyKey, PropertyMetadata> const& Shape::property_table() const
{
    ensure_property_table();
    return *m_property_table;
}

void Shape::ensure_property_table() const
{
    if (m_property_table)
        return;
    m_property_table = make<OrderedHashMap<PropertyKey, PropertyMetadata>>();

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
        if (!shape.m_property_key.has_value()) {
            // Ignore prototype transitions as they don't affect the key map.
            continue;
        }
        if (shape.m_transition_type == TransitionType::Put) {
            m_property_table->set(*shape.m_property_key, { next_offset++, shape.m_attributes });
        } else if (shape.m_transition_type == TransitionType::Configure) {
            auto it = m_property_table->find(*shape.m_property_key);
            VERIFY(it != m_property_table->end());
            it->value.attributes = shape.m_attributes;
        } else if (shape.m_transition_type == TransitionType::Delete) {
            auto remove_it = m_property_table->find(*shape.m_property_key);
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

GC::Ref<Shape> Shape::create_delete_transition(PropertyKey const& property_key)
{
    if (auto existing_shape = get_or_prune_cached_delete_transition(property_key))
        return *existing_shape;
    auto new_shape = heap().allocate<Shape>(*this, property_key, TransitionType::Delete);
    invalidate_prototype_if_needed_for_new_prototype(new_shape);
    if (!m_delete_transitions)
        m_delete_transitions = make<HashMap<PropertyKey, GC::Weak<Shape>>>();
    m_delete_transitions->set(property_key, new_shape.ptr());
    return new_shape;
}

void Shape::add_property_without_transition(PropertyKey const& property_key, PropertyAttributes attributes)
{
    invalidate_prototype_if_needed_for_change_without_transition();
    ensure_property_table();
    if (m_property_table->set(property_key, { m_property_count, attributes }) == AK::HashSetResult::InsertedNewEntry) {
        VERIFY(m_property_count < NumericLimits<u32>::max());
        ++m_property_count;
        ++m_dictionary_generation;
    }
}

void Shape::set_property_attributes_without_transition(PropertyKey const& property_key, PropertyAttributes attributes)
{
    invalidate_prototype_if_needed_for_change_without_transition();
    VERIFY(is_dictionary());
    VERIFY(m_property_table);
    auto it = m_property_table->find(property_key);
    VERIFY(it != m_property_table->end());
    it->value.attributes = attributes;
    m_property_table->set(property_key, it->value);
    ++m_dictionary_generation;
}

void Shape::remove_property_without_transition(PropertyKey const& property_key, u32 offset)
{
    invalidate_prototype_if_needed_for_change_without_transition();
    VERIFY(is_dictionary());
    VERIFY(m_property_table);
    if (m_property_table->remove(property_key))
        --m_property_count;
    for (auto& it : *m_property_table) {
        VERIFY(it.value.offset != offset);
        if (it.value.offset > offset)
            --it.value.offset;
    }
    ++m_dictionary_generation;
}

GC::Ref<Shape> Shape::clone_for_prototype()
{
    VERIFY(!m_is_prototype_shape);
    VERIFY(!m_prototype_chain_validity);
    auto new_shape = heap().allocate<Shape>(m_realm);
    new_shape->m_is_prototype_shape = true;
    new_shape->m_has_parameter_map = m_has_parameter_map;
    new_shape->m_prototype = m_prototype;
    ensure_property_table();
    new_shape->ensure_property_table();
    (*new_shape->m_property_table) = *m_property_table;
    new_shape->m_property_count = new_shape->m_property_table->size();
    new_shape->m_prototype_chain_validity = heap().allocate<PrototypeChainValidity>();
    if (new_shape->m_prototype)
        new_shape->m_prototype->shape().add_child_prototype_shape(*new_shape);
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
    m_is_prototype_shape = true;
    m_prototype_chain_validity = heap().allocate<PrototypeChainValidity>();
    if (m_prototype)
        m_prototype->shape().add_child_prototype_shape(*this);
}

void Shape::add_child_prototype_shape(GC::Ref<Shape> child)
{
    VERIFY(m_is_prototype_shape);
    VERIFY(child->m_is_prototype_shape);
    if (!m_child_prototype_shapes)
        m_child_prototype_shapes = make<Vector<GC::Weak<Shape>>>();
    m_child_prototype_shapes->append(GC::Weak<Shape> { *child });
}

void Shape::invalidate_prototype_if_needed_for_new_prototype(GC::Ref<Shape> new_prototype_shape)
{
    if (!m_is_prototype_shape)
        return;
    new_prototype_shape->set_prototype_shape();
    m_prototype_chain_validity->set_valid(false);

    invalidate_all_prototype_chains_leading_to_this();

    // The owning object is keeping the same [[Prototype]], so its existing
    // children descend from new_prototype_shape going forward.
    new_prototype_shape->m_child_prototype_shapes = move(m_child_prototype_shapes);
}

void Shape::invalidate_prototype_if_needed_for_change_without_transition()
{
    if (!m_is_prototype_shape)
        return;
    m_prototype_chain_validity->set_valid(false);
    m_prototype_chain_validity = heap().allocate<PrototypeChainValidity>();

    invalidate_all_prototype_chains_leading_to_this();
}

void Shape::invalidate_all_prototype_chains_leading_to_this()
{
    if (!m_child_prototype_shapes || m_child_prototype_shapes->is_empty())
        return;

    HashTable<Shape*> shapes_to_invalidate;
    Vector<Shape*> worklist;
    auto enqueue_children_of = [&](Shape& shape) {
        if (!shape.m_child_prototype_shapes)
            return;
        // Prune dead weak refs and enqueue the live ones in one pass.
        shape.m_child_prototype_shapes->remove_all_matching([&](GC::Weak<Shape> const& weak) {
            auto child = weak.ptr();
            if (!child)
                return true;
            if (shapes_to_invalidate.set(child.ptr()) == HashSetResult::InsertedNewEntry)
                worklist.append(child.ptr());
            return false;
        });
    };
    enqueue_children_of(*this);
    while (!worklist.is_empty())
        enqueue_children_of(*worklist.take_last());

    for (auto* shape : shapes_to_invalidate) {
        shape->m_prototype_chain_validity->set_valid(false);
        shape->m_prototype_chain_validity = heap().allocate<PrototypeChainValidity>();
    }
}

}
