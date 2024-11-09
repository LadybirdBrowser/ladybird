/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class NativeFunction : public FunctionObject {
    JS_OBJECT(NativeFunction, FunctionObject);
    JS_DECLARE_ALLOCATOR(NativeFunction);

public:
    static NonnullGCPtr<NativeFunction> create(Realm&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name, Optional<Realm*> = {}, Optional<Object*> prototype = {}, Optional<StringView> const& prefix = {});
    static NonnullGCPtr<NativeFunction> create(Realm&, DeprecatedFlyString const& name, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>);

    virtual ~NativeFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

    // Used for [[Call]] / [[Construct]]'s "...result of evaluating F in a manner that conforms to the specification of F".
    // Needs to be overridden by all NativeFunctions without an m_native_function.
    virtual ThrowCompletionOr<Value> call();
    virtual ThrowCompletionOr<NonnullGCPtr<Object>> construct(FunctionObject& new_target);

    virtual DeprecatedFlyString const& name() const override { return m_name; }
    virtual bool is_strict_mode() const override;
    virtual bool has_constructor() const override { return false; }
    virtual Realm* realm() const override { return m_realm; }

    Optional<DeprecatedFlyString> const& initial_name() const { return m_initial_name; }
    void set_initial_name(Badge<FunctionObject>, DeprecatedFlyString initial_name) { m_initial_name = move(initial_name); }

protected:
    NativeFunction(DeprecatedFlyString name, Object& prototype);
    NativeFunction(JS::GCPtr<JS::HeapFunction<ThrowCompletionOr<Value>(VM&)>>, Object* prototype, Realm& realm);
    NativeFunction(DeprecatedFlyString name, JS::GCPtr<JS::HeapFunction<ThrowCompletionOr<Value>(VM&)>>, Object& prototype);
    explicit NativeFunction(Object& prototype);

    virtual void initialize(Realm&) override;
    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    virtual bool is_native_function() const final { return true; }

    DeprecatedFlyString m_name;
    GCPtr<PrimitiveString> m_name_string;
    Optional<DeprecatedFlyString> m_initial_name; // [[InitialName]]
    JS::GCPtr<JS::HeapFunction<ThrowCompletionOr<Value>(VM&)>> m_native_function;
    GCPtr<Realm> m_realm;
};

template<>
inline bool Object::fast_is<NativeFunction>() const { return is_native_function(); }

}
