/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class ArrayIterator final : public Object
    , public BuiltinIterator {
    JS_OBJECT(ArrayIterator, Object);
    GC_DECLARE_ALLOCATOR(ArrayIterator);

public:
    static GC::Ref<ArrayIterator> create(Realm&, Value array, Object::PropertyKind iteration_kind);

    virtual ~ArrayIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined() override
    {
        if (m_next_method_was_redefined)
            return nullptr;
        return this;
    }
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

private:
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
