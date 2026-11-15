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
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebIDL {

bool is_buffer_source_type(JS::Value);
GC::Ptr<JS::ArrayBuffer> underlying_buffer_source(JS::Object& buffer_source);
WEB_API ErrorOr<ByteBuffer> get_buffer_source_copy(JS::Object const& buffer_source);
WEB_API ErrorOr<ByteBuffer> get_buffer_source_copy(BufferSource const&);
WEB_API ErrorOr<ByteBuffer> get_buffer_source_copy(ArrayBufferView const&);

// AD-HOC: Whether to refuse a callback whose realm's global object is a Window whose associated Document isn't fully
//         active. A caller that has just skipped such a callback itself passes No.
enum class RefuseCallbackFromInactiveWindow {
    Yes,
    No,
};
JS::Completion call_user_object_operation(CallbackType& callback, Utf16FlyString const& operation_name, Optional<JS::Value> this_argument, ReadonlySpan<JS::Value> args, RefuseCallbackFromInactiveWindow = RefuseCallbackFromInactiveWindow::Yes);

WEB_API JS::ThrowCompletionOr<Utf16String> to_utf16_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<Utf16String> to_utf16_usv_string(JS::VM&, JS::Value);
JS::ThrowCompletionOr<String> to_byte_string(JS::VM&, JS::Value);

// The string that a JavaScript value converts to, for an operation or a setter that takes a string.
// OPTIMIZATION: A String value lends the string it holds, which takes no reference to it. That string stays where it
//               is, unchanged, for as long as the value lives, and a call keeps its arguments alive until it returns.
class StringArgument {
public:
    static StringArgument borrowed(Utf16String const& string) { return StringArgument { &string, {} }; }
    static StringArgument borrowed(Utf16String const&&) = delete;
    static StringArgument owned(Utf16String string) { return StringArgument { nullptr, move(string) }; }

    Utf16String const& string() const { return m_borrowed ? *m_borrowed : m_owned; }

private:
    StringArgument(Utf16String const* borrowed, Utf16String owned)
        : m_borrowed(borrowed)
        , m_owned(move(owned))
    {
    }

    Utf16String const* m_borrowed { nullptr };
    Utf16String m_owned;
};

ALWAYS_INLINE JS::ThrowCompletionOr<StringArgument> to_string_argument(JS::VM& vm, JS::Value value)
{
    if (value.is_string())
        return StringArgument::borrowed(value.as_string().utf16_string());
    return StringArgument::owned(TRY(to_utf16_string(vm, value)));
}

JS::ThrowCompletionOr<StringArgument> to_usv_string_argument(JS::VM&, JS::Value);

template<typename T>
GC::Ref<JS::PrimitiveString> primitive_string_from_string(JS::VM& vm, T&& value)
{
    using ValueType = RemoveCVReference<T>;
    if constexpr (IsSame<ValueType, String> || IsSame<ValueType, StringView> || IsSame<ValueType, FlyString> || IsSame<ValueType, ByteString>)
        return JS::PrimitiveString::create(vm, Utf16String::from_utf8(value));
    else
        return JS::PrimitiveString::create(vm, forward<T>(value));
}

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

}
