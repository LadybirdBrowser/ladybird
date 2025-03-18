/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class NativeFunction : public FunctionObject {
    JS_OBJECT(NativeFunction, FunctionObject);
    GC_DECLARE_ALLOCATOR(NativeFunction);

public:
    static GC::Ref<NativeFunction> create(Realm&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name = FlyString {}, Optional<Realm*> = {}, Optional<Object*> prototype = {}, Optional<StringView> const& prefix = {});
    static GC::Ref<NativeFunction> create(Realm&, FlyString const& name, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>);

    virtual ~NativeFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

    // Used for [[Call]] / [[Construct]]'s "...result of evaluating F in a manner that conforms to the specification of F".
    // Needs to be overridden by all NativeFunctions without an m_native_function.
    virtual ThrowCompletionOr<Value> call();
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target);

    virtual FlyString const& name() const override { return m_name; }
    virtual bool is_strict_mode() const override;
    virtual bool has_constructor() const override { return false; }
    virtual Realm* realm() const override { return m_realm; }

    Optional<FlyString> const& initial_name() const { return m_initial_name; }
    void set_initial_name(Badge<FunctionObject>, FlyString initial_name) { m_initial_name = move(initial_name); }

protected:
    NativeFunction(FlyString name, Object& prototype);
    NativeFunction(GC::Ptr<GC::Function<ThrowCompletionOr<Value>(VM&)>>, Object* prototype, Realm& realm);
    NativeFunction(FlyString name, GC::Ptr<GC::Function<ThrowCompletionOr<Value>(VM&)>>, Object& prototype);
    explicit NativeFunction(Object& prototype);

    virtual void initialize(Realm&) override;
    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    virtual bool is_native_function() const final { return true; }

    FlyString m_name;
    GC::Ptr<PrimitiveString> m_name_string;
    Optional<FlyString> m_initial_name; // [[InitialName]]
    GC::Ptr<GC::Function<ThrowCompletionOr<Value>(VM&)>> m_native_function;
    GC::Ptr<Realm> m_realm;
};

template<>
inline bool Object::fast_is<NativeFunction>() const { return is_native_function(); }

}
