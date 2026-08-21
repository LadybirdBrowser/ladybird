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
    static GC::Ref<NativeFunction> create(Realm&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<GC::Ptr<Realm>> = {}, Optional<StringView> const& prefix = {}, Optional<Bytecode::Builtin> builtin = {});
    static GC::Ref<NativeFunction> create(Realm&, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<GC::Ptr<Realm>> = {}, Optional<StringView> const& prefix = {}, Optional<Bytecode::Builtin> builtin = {});
    static GC::Ref<NativeFunction> create(Realm&, Utf16FlyString const& name, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>);
    static GC::Ref<NativeFunction> create(Realm&, Utf16FlyString const& name, NativeFunctionPointer);

    virtual ~NativeFunction() override = default;

    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ExecutionContext&, FunctionObject& new_target) override;

    // Used for [[Call]] / [[Construct]]'s "...result of evaluating F in a manner that conforms to the specification of F".
    virtual ThrowCompletionOr<Value> call();
    virtual ThrowCompletionOr<GC::Ref<Object>> construct(FunctionObject& new_target);

    virtual Utf16String name_for_call_stack() const override;

    Utf16FlyString const& name() const { return m_name; }
    virtual bool is_strict_mode() const override;
    virtual bool has_constructor() const override { return false; }
    virtual Realm* realm() const override { return m_realm.ptr(); }

    Optional<Utf16FlyString> const& initial_name() const { return m_initial_name; }
    void set_initial_name(Badge<FunctionObject>, Utf16FlyString initial_name) { m_initial_name = move(initial_name); }

    virtual bool function_environment_needed() const { return false; }
    virtual size_t function_environment_bindings_count() const { return 0; }

protected:
    NativeFunction(GC::Ptr<Object> prototype, Realm& realm, Optional<Bytecode::Builtin> builtin);
    NativeFunction(Utf16FlyString name, Object& prototype);
    explicit NativeFunction(Object& prototype);

    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    virtual bool is_native_function() const final { return true; }

    Utf16FlyString m_name;
    Optional<Utf16FlyString> m_initial_name; // [[InitialName]]
    GC::Ref<Realm> m_realm;
};

template<>
inline bool Object::fast_is<NativeFunction>() const { return is_native_function(); }

class JS_API RawNativeFunction : public NativeFunction {
    JS_OBJECT(RawNativeFunction, NativeFunction);
    GC_DECLARE_ALLOCATOR(RawNativeFunction);

public:
    static GC::Ref<RawNativeFunction> create(Realm&, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<GC::Ptr<Realm>> = {}, Optional<StringView> const& prefix = {}, Optional<Bytecode::Builtin> builtin = {});
    static GC::Ref<RawNativeFunction> create(Realm&, Utf16FlyString const& name, NativeFunctionPointer);

    virtual ~RawNativeFunction() override = default;

    virtual ThrowCompletionOr<Value> call() override;

    NativeFunctionPointer native_function() const;

protected:
    RawNativeFunction(NativeFunctionPointer, GC::Ptr<Object> prototype, Realm& realm, Optional<Bytecode::Builtin> builtin);
    RawNativeFunction(Utf16FlyString name, NativeFunctionPointer, Object& prototype);

private:
    u32 m_native_function_index { 0 };
};

template<>
inline bool Object::fast_is<RawNativeFunction>() const { return is_raw_native_function(); }

struct DirectGetterConfiguration {
    // These are byte offsets to pointer-sized fields read directly by the interpreter. The bindings
    // generator obtains them from PlatformObject::wrapped_implementation_offset(), the implementation
    // field's generated offset helper, Wrappable::main_world_wrapper_offset(), and
    // GC::WeakImpl::value_offset(), respectively. DirectGetterFunction validates their alignment and
    // converts them to word offsets.
    size_t wrapper_implementation_offset { 0 };
    size_t implementation_value_offset { 0 };
    size_t main_world_wrapper_offset { 0 };
    size_t weak_impl_value_offset { 0 };
};

class JS_API DirectGetterFunction final : public RawNativeFunction {
    JS_OBJECT(DirectGetterFunction, RawNativeFunction);
    GC_DECLARE_ALLOCATOR(DirectGetterFunction);

public:
    static GC::Ref<DirectGetterFunction> create(Realm&, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name, DirectGetterConfiguration, Optional<StringView> const& prefix = {});

private:
    DirectGetterFunction(NativeFunctionPointer, Object& prototype, Realm&, DirectGetterConfiguration);

    u32 m_wrapper_implementation_word_offset { 0 };
    u32 m_implementation_value_word_offset { 0 };
    u32 m_main_world_wrapper_word_offset { 0 };
    u32 m_weak_impl_value_word_offset { 0 };
};

template<>
inline bool Object::fast_is<DirectGetterFunction>() const { return is_direct_getter_function(); }

}
