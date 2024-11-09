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
    JS_DECLARE_ALLOCATOR(ArrayIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArrayIteratorPrototype() override = default;

private:
    explicit ArrayIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
};

}
