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
#include <LibWeb/HTML/PromiseRejectionEvent.h>
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
    visitor.ignore(m_outstanding_rejected_promises_weak_set);
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

    // The queueMicrotask(callback) method must queue a microtask to invoke callback with « » and "report".
    HTML::queue_a_microtask(document, GC::create_function(realm.heap(), [&callback] {
        (void)WebIDL::invoke_callback(callback, {}, WebIDL::ExceptionBehavior::Report);
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
    auto deserialized = TRY(structured_deserialize(vm, serialized, relevant_realm(this_impl())));

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
        auto function = JS::NativeFunction::create(realm, move(steps), 0, "size"_fly_string, &realm);

        // 3. Set globalObject’s count queuing strategy size function to a Function that represents a reference to F, with callback context equal to globalObject’s relevant settings object.
        // FIXME: Update spec comment to pass globalObject's relevant realm once Streams spec is updated for ShadowRealm spec
        m_count_queuing_strategy_size_function = realm.create<WebIDL::CallbackType>(*function, realm);
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
        auto function = JS::NativeFunction::create(realm, move(steps), 1, "size"_fly_string, &realm);

        // 3. Set globalObject’s byte length queuing strategy size function to a Function that represents a reference to F, with callback context equal to globalObject’s relevant settings object.
        // FIXME: Update spec comment to pass globalObject's relevant realm once Streams spec is updated for ShadowRealm spec
        m_byte_length_queuing_strategy_size_function = realm.create<WebIDL::CallbackType>(*function, realm);
    }

    return GC::Ref { *m_byte_length_queuing_strategy_size_function };
}

void UniversalGlobalScopeMixin::push_onto_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    m_outstanding_rejected_promises_weak_set.append(promise);
}

bool UniversalGlobalScopeMixin::remove_from_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    return m_outstanding_rejected_promises_weak_set.remove_first_matching([&](JS::Promise* promise_in_set) {
        return promise == promise_in_set;
    });
}

void UniversalGlobalScopeMixin::push_onto_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise> promise)
{
    m_about_to_be_notified_rejected_promises_list.append(GC::make_root(promise));
}

bool UniversalGlobalScopeMixin::remove_from_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise> promise)
{
    return m_about_to_be_notified_rejected_promises_list.remove_first_matching([&](auto& promise_in_list) {
        return promise == promise_in_list;
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#notify-about-rejected-promises
void UniversalGlobalScopeMixin::notify_about_rejected_promises(Badge<EventLoop>)
{
    auto& realm = this_impl().realm();

    // 1. Let list be a copy of settings object's about-to-be-notified rejected promises list.
    auto list = m_about_to_be_notified_rejected_promises_list;

    // 2. If list is empty, return.
    if (list.is_empty())
        return;

    // 3. Clear settings object's about-to-be-notified rejected promises list.
    m_about_to_be_notified_rejected_promises_list.clear();

    // 4. Let global be settings object's global object.
    auto& global = this_impl();

    // 5. Queue a global task on the DOM manipulation task source given global to run the following substep:
    queue_global_task(Task::Source::DOMManipulation, global, GC::create_function(realm.heap(), [this, &global, list = move(list)] {
        auto& realm = global.realm();

        // 1. For each promise p in list:
        for (auto const& promise : list) {

            // 1. If p's [[PromiseIsHandled]] internal slot is true, continue to the next iteration of the loop.
            if (promise->is_handled())
                continue;

            // 2. Let notHandled be the result of firing an event named unhandledrejection at global, using PromiseRejectionEvent, with the cancelable attribute initialized to true,
            //    the promise attribute initialized to p, and the reason attribute initialized to the value of p's [[PromiseResult]] internal slot.
            PromiseRejectionEventInit event_init {
                {
                    .bubbles = false,
                    .cancelable = true,
                    .composed = false,
                },
                // Sadly we can't use .promise and .reason here, as we can't use the designator on the initialization of DOM::EventInit above.
                /* .promise = */ *promise,
                /* .reason = */ promise->result(),
            };

            auto promise_rejection_event = PromiseRejectionEvent::create(realm, HTML::EventNames::unhandledrejection, event_init);

            bool not_handled = global.dispatch_event(*promise_rejection_event);

            // 3. If notHandled is false, then the promise rejection is handled. Otherwise, the promise rejection is not handled.

            // 4. If p's [[PromiseIsHandled]] internal slot is false, add p to settings object's outstanding rejected promises weak set.
            if (!promise->is_handled())
                m_outstanding_rejected_promises_weak_set.append(*promise);

            // This algorithm results in promise rejections being marked as handled or not handled. These concepts parallel handled and not handled script errors.
            // If a rejection is still not handled after this, then the rejection may be reported to a developer console.
            if (not_handled)
                HTML::report_exception_to_console(promise->result(), realm, ErrorInPromise::Yes);
        }
    }));
}

}
