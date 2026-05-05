/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>

namespace JS::Bytecode {

class JS_API PropertyNameIterator final
    : public Object
    , public BuiltinIterator {
    JS_OBJECT(PropertyNameIterator, Object);
    GC_DECLARE_ALLOCATOR(PropertyNameIterator);

public:
    using FastPath = ObjectPropertyIteratorFastPath;

    static GC::Ref<PropertyNameIterator> create(Realm&, GC::Ref<Object>, Vector<PropertyKey>, FastPath = FastPath::None, u32 indexed_property_count = 0, GC::Ptr<Shape> = nullptr, GC::Ptr<PrototypeChainValidity> = nullptr);
    static GC::Ref<PropertyNameIterator> create(Realm&, GC::Ref<Object>, ObjectPropertyIteratorCacheData&, ObjectPropertyIteratorCache* = nullptr);

    virtual ~PropertyNameIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(Value) override { return this; }
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

    void reset_with_cache_data(GC::Ref<Object>, ObjectPropertyIteratorCacheData&, ObjectPropertyIteratorCache*);

private:
    PropertyNameIterator(Realm&, GC::Ref<Object>, Vector<PropertyKey>, FastPath, u32 indexed_property_count, GC::Ptr<Shape>, GC::Ptr<PrototypeChainValidity>);
    PropertyNameIterator(Realm&, GC::Ref<Object>, ObjectPropertyIteratorCacheData&, ObjectPropertyIteratorCache*);

    ReadonlySpan<PropertyKey> property_list() const;
    ReadonlySpan<Value> property_value_list() const;

    bool fast_path_still_valid() const;
    void disable_fast_path();

    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    GC::Ptr<Object> m_object;
    Vector<PropertyKey> m_owned_properties;
    GC::Ptr<ObjectPropertyIteratorCacheData> m_property_cache;
    GC::Ptr<Shape> m_shape;
    GC::Ptr<PrototypeChainValidity> m_prototype_chain_validity;
    ObjectPropertyIteratorCache* m_iterator_cache_slot { nullptr };
    u32 m_indexed_property_count { 0 };
    u32 m_next_indexed_property { 0 };
    size_t m_next_property { 0 };
    bool m_shape_is_dictionary { false };
    u32 m_shape_dictionary_generation { 0 };
    FastPath m_fast_path { FastPath::None };
};

}
