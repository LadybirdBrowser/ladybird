/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class GlobalObject : public Object {
    JS_OBJECT(GlobalObject, Object);
    JS_DECLARE_ALLOCATOR(GlobalObject);

    friend class Intrinsics;

public:
    virtual void initialize(Realm&) override;
    virtual ~GlobalObject() override;

protected:
    explicit GlobalObject(Realm&);

private:
    virtual bool is_global_object() const final { return true; }

    JS_DECLARE_NATIVE_FUNCTION(gc);
    JS_DECLARE_NATIVE_FUNCTION(is_nan);
    JS_DECLARE_NATIVE_FUNCTION(is_finite);
    JS_DECLARE_NATIVE_FUNCTION(parse_float);
    JS_DECLARE_NATIVE_FUNCTION(parse_int);
    JS_DECLARE_NATIVE_FUNCTION(eval);
    JS_DECLARE_NATIVE_FUNCTION(encode_uri);
    JS_DECLARE_NATIVE_FUNCTION(decode_uri);
    JS_DECLARE_NATIVE_FUNCTION(encode_uri_component);
    JS_DECLARE_NATIVE_FUNCTION(decode_uri_component);
    JS_DECLARE_NATIVE_FUNCTION(escape);
    JS_DECLARE_NATIVE_FUNCTION(unescape);
};

Object& set_default_global_bindings(Realm&);

template<>
inline bool Object::fast_is<GlobalObject>() const { return is_global_object(); }

template<typename... Args>
[[nodiscard]] ALWAYS_INLINE ThrowCompletionOr<Value> Value::invoke(VM& vm, PropertyKey const& property_key, Args... args)
{
    if constexpr (sizeof...(Args) > 0) {
        MarkedVector<Value> arglist { vm.heap() };
        (..., arglist.append(move(args)));
        return invoke_internal(vm, property_key, move(arglist));
    }

    return invoke_internal(vm, property_key, Optional<MarkedVector<Value>> {});
}

}
