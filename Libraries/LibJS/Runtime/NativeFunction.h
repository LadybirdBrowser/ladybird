/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

// A built-in function whose behaviour is C++: a raw native function, which JS_DEFINE_NATIVE_FUNCTION defines and the
// runtime calls directly, or a closure, which the function keeps in a GC::Function of the embedder's heap.
class JS_API NativeFunction : public FunctionObject {
public:
    static GC::Ref<NativeFunction> create(Realm&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<GC::Ptr<Realm>> = {}, Optional<StringView> const& prefix = {});
    static GC::Ref<NativeFunction> create(Realm&, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name = Utf16FlyString {}, Optional<GC::Ptr<Realm>> = {}, Optional<StringView> const& prefix = {});
    static GC::Ref<NativeFunction> create(Realm&, Utf16FlyString const& name, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>);

    static bool is_engine_class_of(Object const& object) { return object.is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_NATIVE_FUNCTION); }
};

class JS_API RawNativeFunction : public NativeFunction {
public:
    static bool is_engine_class_of(Object const& object) { return object.has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION); }
};

struct DirectGetterConfiguration {
    // These are byte offsets to pointer-sized fields read directly by the interpreter. The bindings
    // generator obtains them from HostObject::wrappable_offset(), the implementation
    // field's generated offset helper, Wrappable::main_world_wrapper_offset(), and
    // GC::WeakImpl::value_offset(), respectively. DirectGetterFunction validates their alignment and
    // converts them to word offsets.
    size_t wrapper_implementation_offset { 0 };
    size_t implementation_value_offset { 0 };
    size_t main_world_wrapper_offset { 0 };
    size_t weak_impl_value_offset { 0 };
};

class JS_API DirectGetterFunction final : public RawNativeFunction {
public:
    static GC::Ref<DirectGetterFunction> create(Realm&, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name, DirectGetterConfiguration, Optional<StringView> const& prefix = {});

    static bool is_engine_class_of(Object const& object) { return object.has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_DIRECT_GETTER_FUNCTION); }
};

}
