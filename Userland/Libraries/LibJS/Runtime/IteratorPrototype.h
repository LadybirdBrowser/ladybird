/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class IteratorPrototype : public PrototypeObject<IteratorPrototype, Iterator> {
    JS_PROTOTYPE_OBJECT(IteratorPrototype, Iterator, Iterator);
    JS_DECLARE_ALLOCATOR(IteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~IteratorPrototype() override = default;

private:
    IteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(constructor_getter);
    JS_DECLARE_NATIVE_FUNCTION(constructor_setter);

    JS_DECLARE_NATIVE_FUNCTION(drop);
    JS_DECLARE_NATIVE_FUNCTION(every);
    JS_DECLARE_NATIVE_FUNCTION(filter);
    JS_DECLARE_NATIVE_FUNCTION(find);
    JS_DECLARE_NATIVE_FUNCTION(flat_map);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(map);
    JS_DECLARE_NATIVE_FUNCTION(reduce);
    JS_DECLARE_NATIVE_FUNCTION(some);
    JS_DECLARE_NATIVE_FUNCTION(take);
    JS_DECLARE_NATIVE_FUNCTION(to_array);

    JS_DECLARE_NATIVE_FUNCTION(symbol_iterator);
    JS_DECLARE_NATIVE_FUNCTION(to_string_tag_getter);
    JS_DECLARE_NATIVE_FUNCTION(to_string_tag_setter);
};

}
