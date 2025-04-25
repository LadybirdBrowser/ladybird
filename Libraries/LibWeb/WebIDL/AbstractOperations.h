/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::WebIDL {

bool is_buffer_source_type(JS::Value);
GC::Ptr<JS::ArrayBuffer> underlying_buffer_source(JS::Object& buffer_source);
ErrorOr<ByteBuffer> get_buffer_source_copy(JS::Object const& buffer_source);

JS::Completion call_user_object_operation(CallbackType& callback, String const& operation_name, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

JS::ThrowCompletionOr<String> to_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<String> to_usv_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<String> to_byte_string(JS::VM&, JS::Value);

enum class ExceptionBehavior {
    NotSpecified,
    Report,
    Rethrow,
};
JS::Completion invoke_callback(CallbackType& callback, Optional<JS::Value> this_argument, ExceptionBehavior exception_behavior, ReadonlySpan<JS::Value> args);
JS::Completion invoke_callback(CallbackType& callback, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

GC::Ref<Promise> invoke_promise_callback(CallbackType& callback, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args);

JS::Completion construct(CallbackType& callback, ReadonlySpan<JS::Value> args);

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

}
