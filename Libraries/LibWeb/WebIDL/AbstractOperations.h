/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebIDL {

bool is_buffer_source_type(JS::Value);
GC::Ptr<JS::ArrayBuffer> underlying_buffer_source(JS::Object& buffer_source);
WEB_API ErrorOr<ByteBuffer> get_buffer_source_copy(JS::Object const& buffer_source);

JS::Completion call_user_object_operation(CallbackType& callback, Utf16FlyString const& operation_name, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

WEB_API JS::ThrowCompletionOr<String> to_string(JS::VM&, JS::Value);
WEB_API JS::ThrowCompletionOr<Utf16String> to_utf16_string(JS::VM&, JS::Value);
WEB_API JS::ThrowCompletionOr<String> to_usv_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<Utf16String> to_utf16_usv_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<String> to_byte_string(JS::VM&, JS::Value);

enum class ExceptionBehavior {
    NotSpecified,
    Report,
    Rethrow,
};
WEB_API JS::Completion invoke_callback(CallbackType& callback, Optional<JS::Value> this_argument, ExceptionBehavior exception_behavior, ReadonlySpan<JS::Value> args);
WEB_API JS::Completion invoke_callback(CallbackType& callback, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

WEB_API GC::Ref<Promise> invoke_promise_callback(CallbackType& callback, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

JS::Completion construct(CallbackType& callable, ReadonlySpan<JS::Value> args);

// https://webidl.spec.whatwg.org/#abstract-opdef-integerpart
double integer_part(double n);

enum class EnforceRange {
    Yes,
    No,
};

enum class Clamp {
    Yes,
    No,
};

// https://webidl.spec.whatwg.org/#abstract-opdef-converttoint
template<Integral T>
JS::ThrowCompletionOr<T> convert_to_int(JS::VM& vm, JS::Value, EnforceRange enforce_range = EnforceRange::No, Clamp clamp = Clamp::No);

bool lists_contain_same_elements(GC::Ptr<JS::Array> array, Optional<GC::RootVector<GC::Ref<DOM::Element>>> const& elements);

// https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
// Steps from "internally create a new object implementing the interface"
template<typename PrototypeType>
JS::ThrowCompletionOr<void> set_prototype_from_new_target(JS::VM& vm, JS::FunctionObject& new_target, FlyString const& interface_name, JS::Object& object)
{
    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    // 3.3. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {
        // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
        auto* target_realm = TRY(JS::get_function_realm(vm, new_target));

        // 2. Set prototype to the interface prototype object for interface in targetRealm.
        VERIFY(target_realm);
        prototype = &Bindings::ensure_web_prototype<PrototypeType>(*target_realm, interface_name);
    }

    // 9. Set instance.[[Prototype]] to prototype.
    VERIFY(prototype.is_object());
    TRY(object.internal_set_prototype_of(&prototype.as_object()));
    return {};
}

}
