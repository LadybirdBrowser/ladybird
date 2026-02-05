/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class JS_API NativeFunction : public FunctionObject {
    JS_OBJECT(NativeFunction, FunctionObject);
    GC_DECLARE_ALLOCATOR(NativeFunction);

public:
    static GC::Ref<NativeFunction> create(Realm&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<Realm*> = {}, Optional<StringView> const& prefix = {}, Optional<Bytecode::Builtin> builtin = {});
    static GC::Ref<NativeFunction> create(Realm&, Utf16FlyString const& name, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>);

    virtual ~NativeFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ExecutionContext&, FunctionObject& new_target) override;

    // Used for [[Call]] / [[Construct]]'s "...result of evaluating F in a manner that conforms to the specification of F".
    // Needs to be overridden by all NativeFunctions without an m_native_function.
    virtual ThrowCompletionOr<Value> call();
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target);

    virtual Utf16String name_for_call_stack() const override;

    Utf16FlyString const& name() const { return m_name; }
    virtual bool is_strict_mode() const override;
    virtual bool has_constructor() const override { return false; }
    virtual Realm* realm() const override { return m_realm; }

    Optional<Utf16FlyString> const& initial_name() const { return m_initial_name; }
    void set_initial_name(Badge<FunctionObject>, Utf16FlyString initial_name) { m_initial_name = move(initial_name); }

    virtual bool function_environment_needed() const { return false; }
    virtual size_t function_environment_bindings_count() const { return 0; }

protected:
    NativeFunction(Utf16FlyString name, Object& prototype);
    NativeFunction(AK::Function<ThrowCompletionOr<Value>(VM&)>, Object* prototype, Realm& realm, Optional<Bytecode::Builtin> builtin);
    NativeFunction(Utf16FlyString name, AK::Function<ThrowCompletionOr<Value>(VM&)>, Object& prototype);
    explicit NativeFunction(Object& prototype);

    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    virtual bool is_native_function() const final { return true; }

    Utf16FlyString m_name;
    Optional<Utf16FlyString> m_initial_name; // [[InitialName]]
    AK::Function<ThrowCompletionOr<Value>(VM&)> m_native_function;
    GC::Ref<Realm> m_realm;
};

template<>
inline bool Object::fast_is<NativeFunction>() const { return is_native_function(); }

}
