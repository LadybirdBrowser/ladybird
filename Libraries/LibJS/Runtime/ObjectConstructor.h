/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class ObjectConstructor final : public NativeFunction {
    JS_OBJECT(ObjectConstructor, NativeFunction);
    JS_DECLARE_ALLOCATOR(ObjectConstructor);

public:
    virtual void initialize(Realm&) override;
    virtual ~ObjectConstructor() override = default;

    virtual ThrowCompletionOr<Value> call() override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target) override;

private:
    explicit ObjectConstructor(Realm&);

    virtual bool has_constructor() const override { return true; }

    JS_DECLARE_NATIVE_FUNCTION(define_property);
    JS_DECLARE_NATIVE_FUNCTION(define_properties);
    JS_DECLARE_NATIVE_FUNCTION(is);
    JS_DECLARE_NATIVE_FUNCTION(get_own_property_descriptor);
    JS_DECLARE_NATIVE_FUNCTION(get_own_property_descriptors);
    JS_DECLARE_NATIVE_FUNCTION(get_own_property_names);
    JS_DECLARE_NATIVE_FUNCTION(get_own_property_symbols);
    JS_DECLARE_NATIVE_FUNCTION(get_prototype_of);
    JS_DECLARE_NATIVE_FUNCTION(group_by);
    JS_DECLARE_NATIVE_FUNCTION(set_prototype_of);
    JS_DECLARE_NATIVE_FUNCTION(is_extensible);
    JS_DECLARE_NATIVE_FUNCTION(is_frozen);
    JS_DECLARE_NATIVE_FUNCTION(is_sealed);
    JS_DECLARE_NATIVE_FUNCTION(prevent_extensions);
    JS_DECLARE_NATIVE_FUNCTION(seal);
    JS_DECLARE_NATIVE_FUNCTION(freeze);
    JS_DECLARE_NATIVE_FUNCTION(from_entries);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(values);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(create);
    JS_DECLARE_NATIVE_FUNCTION(has_own);
    JS_DECLARE_NATIVE_FUNCTION(assign);
};

}
