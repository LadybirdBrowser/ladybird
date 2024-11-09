/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/PropertyAttributes.h>
#include <LibJS/Runtime/StringOrSymbol.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

struct PropertyMetadata {
    u32 offset { 0 };
    PropertyAttributes attributes { 0 };
};

struct TransitionKey {
    StringOrSymbol property_key;
    PropertyAttributes attributes { 0 };

    bool operator==(TransitionKey const& other) const
    {
        return property_key == other.property_key && attributes == other.attributes;
    }
};

class PrototypeChainValidity final : public Cell {
    JS_CELL(PrototypeChainValidity, Cell);
    JS_DECLARE_ALLOCATOR(PrototypeChainValidity);

public:
    [[nodiscard]] bool is_valid() const { return m_valid; }
    void set_valid(bool valid) { m_valid = valid; }

private:
    bool m_valid { true };
    size_t padding { 0 };
};

class Shape final : public Cell {
    JS_CELL(Shape, Cell);
    JS_DECLARE_ALLOCATOR(Shape);

public:
    virtual ~Shape() override;

    enum class TransitionType : u8 {
        Invalid,
        Put,
        Configure,
        Prototype,
        Delete,
        CacheableDictionary,
        UncacheableDictionary,
    };

    [[nodiscard]] NonnullGCPtr<Shape> create_put_transition(StringOrSymbol const&, PropertyAttributes attributes);
    [[nodiscard]] NonnullGCPtr<Shape> create_configure_transition(StringOrSymbol const&, PropertyAttributes attributes);
    [[nodiscard]] NonnullGCPtr<Shape> create_prototype_transition(Object* new_prototype);
    [[nodiscard]] NonnullGCPtr<Shape> create_delete_transition(StringOrSymbol const&);
    [[nodiscard]] NonnullGCPtr<Shape> create_cacheable_dictionary_transition();
    [[nodiscard]] NonnullGCPtr<Shape> create_uncacheable_dictionary_transition();
    [[nodiscard]] NonnullGCPtr<Shape> clone_for_prototype();
    [[nodiscard]] static NonnullGCPtr<Shape> create_for_prototype(NonnullGCPtr<Realm>, GCPtr<Object> prototype);

    void add_property_without_transition(StringOrSymbol const&, PropertyAttributes);
    void add_property_without_transition(PropertyKey const&, PropertyAttributes);

    void remove_property_without_transition(StringOrSymbol const&, u32 offset);
    void set_property_attributes_without_transition(StringOrSymbol const&, PropertyAttributes);

    [[nodiscard]] bool is_cacheable() const { return m_cacheable; }
    [[nodiscard]] bool is_dictionary() const { return m_dictionary; }
    [[nodiscard]] bool is_cacheable_dictionary() const { return m_dictionary && m_cacheable; }
    [[nodiscard]] bool is_uncacheable_dictionary() const { return m_dictionary && !m_cacheable; }

    [[nodiscard]] bool is_prototype_shape() const { return m_is_prototype_shape; }
    void set_prototype_shape();

    GCPtr<PrototypeChainValidity> prototype_chain_validity() const { return m_prototype_chain_validity; }

    Realm& realm() const { return m_realm; }

    Object* prototype() { return m_prototype; }
    Object const* prototype() const { return m_prototype; }

    Optional<PropertyMetadata> lookup(StringOrSymbol const&) const;
    OrderedHashMap<StringOrSymbol, PropertyMetadata> const& property_table() const;
    u32 property_count() const { return m_property_count; }

    struct Property {
        StringOrSymbol key;
        PropertyMetadata value;
    };

    void set_prototype_without_transition(Object* new_prototype);

private:
    explicit Shape(Realm&);
    Shape(Shape& previous_shape, StringOrSymbol const& property_key, PropertyAttributes attributes, TransitionType);
    Shape(Shape& previous_shape, StringOrSymbol const& property_key, TransitionType);
    Shape(Shape& previous_shape, Object* new_prototype);

    void invalidate_prototype_if_needed_for_new_prototype(NonnullGCPtr<Shape> new_prototype_shape);
    void invalidate_all_prototype_chains_leading_to_this();

    virtual void visit_edges(Visitor&) override;

    [[nodiscard]] GCPtr<Shape> get_or_prune_cached_forward_transition(TransitionKey const&);
    [[nodiscard]] GCPtr<Shape> get_or_prune_cached_prototype_transition(Object* prototype);
    [[nodiscard]] GCPtr<Shape> get_or_prune_cached_delete_transition(StringOrSymbol const&);

    void ensure_property_table() const;

    NonnullGCPtr<Realm> m_realm;

    mutable OwnPtr<OrderedHashMap<StringOrSymbol, PropertyMetadata>> m_property_table;

    OwnPtr<HashMap<TransitionKey, WeakPtr<Shape>>> m_forward_transitions;
    OwnPtr<HashMap<GCPtr<Object>, WeakPtr<Shape>>> m_prototype_transitions;
    OwnPtr<HashMap<StringOrSymbol, WeakPtr<Shape>>> m_delete_transitions;
    GCPtr<Shape> m_previous;
    StringOrSymbol m_property_key;
    GCPtr<Object> m_prototype;

    GCPtr<PrototypeChainValidity> m_prototype_chain_validity;

    u32 m_property_count { 0 };

    PropertyAttributes m_attributes { 0 };
    TransitionType m_transition_type { TransitionType::Invalid };

    bool m_dictionary : 1 { false };
    bool m_cacheable : 1 { true };
    bool m_is_prototype_shape : 1 { false };
};

}

template<>
struct AK::Traits<JS::TransitionKey> : public DefaultTraits<JS::TransitionKey> {
    static unsigned hash(const JS::TransitionKey& key)
    {
        return pair_int_hash(key.attributes.bits(), Traits<JS::StringOrSymbol>::hash(key.property_key));
    }
};
