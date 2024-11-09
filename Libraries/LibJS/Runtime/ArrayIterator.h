/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class ArrayIterator final : public Object {
    JS_OBJECT(ArrayIterator, Object);
    JS_DECLARE_ALLOCATOR(ArrayIterator);

public:
    static NonnullGCPtr<ArrayIterator> create(Realm&, Value array, Object::PropertyKind iteration_kind);

    virtual ~ArrayIterator() override = default;

    Value array() const { return m_array; }
    Object::PropertyKind iteration_kind() const { return m_iteration_kind; }
    size_t index() const { return m_index; }

private:
    friend class ArrayIteratorPrototype;

    ArrayIterator(Value array, Object::PropertyKind iteration_kind, Object& prototype);

    virtual bool is_array_iterator() const override { return true; }
    virtual void visit_edges(Cell::Visitor&) override;

    Value m_array;
    Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

template<>
inline bool Object::fast_is<ArrayIterator>() const { return is_array_iterator(); }

}
