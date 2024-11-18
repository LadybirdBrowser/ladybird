/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

UniversalGlobalScopeMixin::~UniversalGlobalScopeMixin() = default;

void UniversalGlobalScopeMixin::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_count_queuing_strategy_size_function);
    visitor.visit(m_byte_length_queuing_strategy_size_function);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-btoa
WebIDL::ExceptionOr<String> UniversalGlobalScopeMixin::btoa(String const& data) const
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    // The btoa(data) method must throw an "InvalidCharacterError" DOMException if data contains any character whose code point is greater than U+00FF.
    Vector<u8> byte_string;
    byte_string.ensure_capacity(data.bytes().size());
    for (u32 code_point : Utf8View(data)) {
        if (code_point > 0xff)
            return WebIDL::InvalidCharacterError::create(realm, "Data contains characters outside the range U+0000 and U+00FF"_string);
        byte_string.append(code_point);
    }

    // Otherwise, the user agent must convert data to a byte sequence whose nth byte is the eight-bit representation of the nth code point of data,
    // and then must apply forgiving-base64 encode to that byte sequence and return the result.
    return TRY_OR_THROW_OOM(vm, encode_base64(byte_string.span()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-atob
WebIDL::ExceptionOr<String> UniversalGlobalScopeMixin::atob(String const& data) const
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    // 1. Let decodedData be the result of running forgiving-base64 decode on data.
    auto decoded_data = decode_base64(data);

    // 2. If decodedData is failure, then throw an "InvalidCharacterError" DOMException.
    if (decoded_data.is_error())
        return WebIDL::InvalidCharacterError::create(realm, "Input string is not valid base64 data"_string);

    // 3. Return decodedData.
    // decode_base64() returns a byte buffer. LibJS uses UTF-8 for strings. Use isomorphic decoding to convert bytes to UTF-8.
    return Infra::isomorphic_decode(decoded_data.value());
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-queuemicrotask
void UniversalGlobalScopeMixin::queue_microtask(WebIDL::CallbackType& callback)
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    GC::Ptr<DOM::Document> document;
    if (is<Window>(this_impl()))
        document = &static_cast<Window&>(this_impl()).associated_document();

    // The queueMicrotask(callback) method must queue a microtask to invoke callback, and if callback throws an exception, report the exception.
    HTML::queue_a_microtask(document, GC::create_function(realm.heap(), [&callback, &realm] {
        auto result = WebIDL::invoke_callback(callback, {});
        if (result.is_error())
            HTML::report_exception(result, realm);
    }));
}

// https://html.spec.whatwg.org/multipage/structured-data.html#dom-structuredclone
WebIDL::ExceptionOr<JS::Value> UniversalGlobalScopeMixin::structured_clone(JS::Value value, StructuredSerializeOptions const& options) const
{
    auto& vm = this_impl().vm();
    (void)options;

    // 1. Let serialized be ? StructuredSerializeWithTransfer(value, options["transfer"]).
    // FIXME: Use WithTransfer variant of the AO
    auto serialized = TRY(structured_serialize(vm, value));

    // 2. Let deserializeRecord be ? StructuredDeserializeWithTransfer(serialized, this's relevant realm).
    // FIXME: Use WithTransfer variant of the AO
    auto deserialized = TRY(structured_deserialize(vm, serialized, relevant_realm(this_impl()), {}));

    // 3. Return deserializeRecord.[[Deserialized]].
    return deserialized;
}

// https://streams.spec.whatwg.org/#count-queuing-strategy-size-function
GC::Ref<WebIDL::CallbackType> UniversalGlobalScopeMixin::count_queuing_strategy_size_function()
{
    auto& realm = HTML::relevant_realm(this_impl());

    if (!m_count_queuing_strategy_size_function) {
        // 1. Let steps be the following steps:
        auto steps = [](auto const&) {
            // 1. Return 1.
            return 1.0;
        };

        // 2. Let F be ! CreateBuiltinFunction(steps, 0, "size", « », globalObject’s relevant Realm).
        auto function = JS::NativeFunction::create(realm, move(steps), 0, "size", &realm);

        // 3. Set globalObject’s count queuing strategy size function to a Function that represents a reference to F, with callback context equal to globalObject’s relevant settings object.
        m_count_queuing_strategy_size_function = realm.create<WebIDL::CallbackType>(*function, relevant_settings_object(this_impl()));
    }

    return GC::Ref { *m_count_queuing_strategy_size_function };
}

// https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function
GC::Ref<WebIDL::CallbackType> UniversalGlobalScopeMixin::byte_length_queuing_strategy_size_function()
{
    auto& realm = HTML::relevant_realm(this_impl());

    if (!m_byte_length_queuing_strategy_size_function) {
        // 1. Let steps be the following steps, given chunk:
        auto steps = [](JS::VM& vm) {
            auto chunk = vm.argument(0);

            // 1. Return ? GetV(chunk, "byteLength").
            return chunk.get(vm, vm.names.byteLength);
        };

        // 2. Let F be ! CreateBuiltinFunction(steps, 1, "size", « », globalObject’s relevant Realm).
        auto function = JS::NativeFunction::create(realm, move(steps), 1, "size", &realm);

        // 3. Set globalObject’s byte length queuing strategy size function to a Function that represents a reference to F, with callback context equal to globalObject’s relevant settings object.
        m_byte_length_queuing_strategy_size_function = realm.create<WebIDL::CallbackType>(*function, relevant_settings_object(this_impl()));
    }

    return GC::Ref { *m_byte_length_queuing_strategy_size_function };
}

}
