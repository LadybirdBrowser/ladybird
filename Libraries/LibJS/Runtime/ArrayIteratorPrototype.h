/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class ArrayIteratorPrototype final : public PrototypeObject<ArrayIteratorPrototype, ArrayIterator> {
    JS_PROTOTYPE_OBJECT(ArrayIteratorPrototype, ArrayIterator, ArrayIterator);
    GC_DECLARE_ALLOCATOR(ArrayIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArrayIteratorPrototype() override = default;

    bool next_method_was_redefined() const { return m_next_method_was_redefined; }
    void set_next_method_was_redefined() { m_next_method_was_redefined = true; }

    virtual bool is_array_iterator_prototype() const override { return true; }

private:
    explicit ArrayIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);

    bool m_next_method_was_redefined { false };
};

template<>
inline bool Object::fast_is<ArrayIteratorPrototype>() const { return is_array_iterator_prototype(); }

}
