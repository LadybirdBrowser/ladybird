/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

struct ValueAndAttributes {
    Value value;
    PropertyAttributes attributes { default_attributes };

    Optional<u32> property_offset {};

    void visit_edges(Cell::Visitor& visitor)
    {
        visitor.visit(value);
    }
};

class GenericIndexedPropertyStorage {
public:
    explicit GenericIndexedPropertyStorage()
    {
    }

    bool has_index(u32 index) const;
    Optional<ValueAndAttributes> get(u32 index) const;
    void put(u32 index, Value value, PropertyAttributes attributes = default_attributes);
    void remove(u32 index);

    ValueAndAttributes take_first();
    ValueAndAttributes take_last();

    size_t size() const { return m_sparse_elements.size(); }
    size_t array_like_size() const { return m_array_size; }
    bool set_array_like_size(size_t new_size);

    void visit_edges(Cell::Visitor& visitor)
    {
        for (auto& element : m_sparse_elements)
            element.value.visit_edges(visitor);
    }

    HashMap<u32, ValueAndAttributes> const& sparse_elements() const { return m_sparse_elements; }

    size_t external_memory_size() const
    {
        return m_sparse_elements.capacity() * (sizeof(u32) + sizeof(ValueAndAttributes));
    }

private:
    size_t m_array_size { 0 };
    HashMap<u32, ValueAndAttributes> m_sparse_elements;
};

}
