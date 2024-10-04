/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class ObjectPrototype final : public Object {
    JS_OBJECT(ObjectPrototype, Object);
    JS_DECLARE_ALLOCATOR(ObjectPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ObjectPrototype() override = default;

    // 10.4.7 Immutable Prototype Exotic Objects, https://tc39.es/ecma262/#sec-immutable-prototype-exotic-objects

    virtual ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype) override;

    // public to serve as intrinsic function %Object.prototype.toString%
    JS_DECLARE_NATIVE_FUNCTION(to_string);

private:
    explicit ObjectPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(has_own_property);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(property_is_enumerable);
    JS_DECLARE_NATIVE_FUNCTION(is_prototype_of);
    JS_DECLARE_NATIVE_FUNCTION(define_getter);
    JS_DECLARE_NATIVE_FUNCTION(define_setter);
    JS_DECLARE_NATIVE_FUNCTION(lookup_getter);
    JS_DECLARE_NATIVE_FUNCTION(lookup_setter);
    JS_DECLARE_NATIVE_FUNCTION(proto_getter);
    JS_DECLARE_NATIVE_FUNCTION(proto_setter);
};

}
