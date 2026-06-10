/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/UnderlyingSink.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/WritableStream.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(WritableStream);

WebIDL::ExceptionOr<GC::Ref<WritableStream>> WritableStream::create_for_constructor(JS::Realm& realm, GC::Ptr<JS::Object> underlying_sink_object, QueuingStrategy const& strategy)
{
    auto underlying_sink = underlying_sink_object ? JS::Value { underlying_sink_object } : JS::js_null();
    auto underlying_sink_dict = TRY(Bindings::convert_to_idl_value_for_underlying_sink(realm.vm(), underlying_sink));
    if (underlying_sink_dict.type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid use of reserved key 'type'"sv };

    return create(realm, underlying_sink_object, underlying_sink_dict, strategy);
}

// https://streams.spec.whatwg.org/#ws-constructor
WebIDL::ExceptionOr<GC::Ref<WritableStream>> WritableStream::create(JS::Realm& realm, GC::Ptr<JS::Object> underlying_sink_object, UnderlyingSink const& underlying_sink_dict, QueuingStrategy const& strategy)
{
    auto& vm = realm.vm();

    auto writable_stream = GC::Heap::the().allocate<WritableStream>();

    // 1. If underlyingSink is missing, set it to null.
    auto underlying_sink = underlying_sink_object ? JS::Value(underlying_sink_object) : JS::js_null();

    // 4. Perform ! InitializeWritableStream(this).
    // Note: This AO configures slot values which are already specified in the class's field initializers.

    // 5. Let sizeAlgorithm be ! ExtractSizeAlgorithm(strategy).
    auto size_algorithm = extract_size_algorithm(vm, strategy);

    // 6. Let highWaterMark be ? ExtractHighWaterMark(strategy, 1).
    auto high_water_mark = TRY(extract_high_water_mark(strategy, 1));

    // 7. Perform ? SetUpWritableStreamDefaultControllerFromUnderlyingSink(this, underlyingSink, underlyingSinkDict, highWaterMark, sizeAlgorithm).
    TRY(set_up_writable_stream_default_controller_from_underlying_sink(realm, *writable_stream, underlying_sink, underlying_sink_dict, high_water_mark, size_algorithm));

    return writable_stream;
}

WritableStream::WritableStream()
{
}

}

namespace Web::Bindings {

Streams::WritableStream* writable_stream_from_object(JS::Object& object)
{
    return Bindings::impl_from<Streams::WritableStream>(&object);
}

void serialize_writable_stream_with_transfer(JS::Realm& realm, HTML::TransferDataEncoder& data_holder, GC::Ref<Streams::WritableStream> stream)
{
    JS::Value wrapped_stream = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, stream);
    auto result = MUST(HTML::structured_serialize_with_transfer(realm, wrapped_stream, { { wrapped_stream.as_object() } }));
    data_holder.extend(move(result.transfer_data_holders));
}

}

namespace Web::Streams {

void WritableStream::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_close_request);
    visitor.visit(m_controller);
    visitor.visit(m_in_flight_write_request);
    visitor.visit(m_in_flight_close_request);
    if (m_pending_abort_request.has_value()) {
        visitor.visit(m_pending_abort_request->promise);
        visitor.visit(m_pending_abort_request->reason);
    }
    visitor.visit(m_stored_error);
    visitor.visit(m_writer);
    for (auto& write_request : m_write_requests)
        visitor.visit(write_request);
}

// https://streams.spec.whatwg.org/#ws-locked
bool WritableStream::locked() const
{
    // 1. Return ! IsWritableStreamLocked(this).
    return is_writable_stream_locked(*this);
}

// https://streams.spec.whatwg.org/#ws-close
GC::Ref<WebIDL::Promise> WritableStream::close(JS::Realm& realm)
{
    // 1. If ! IsWritableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_writable_stream_locked(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot close a locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. If ! WritableStreamCloseQueuedOrInFlight(this) is true, return a promise rejected with a TypeError exception.
    if (writable_stream_close_queued_or_in_flight(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot close a stream that is already closed or errored"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 3. Return ! WritableStreamClose(this).
    return writable_stream_close(realm, *this);
}

// https://streams.spec.whatwg.org/#ws-abort
GC::Ref<WebIDL::Promise> WritableStream::abort(JS::Realm& realm, Optional<JS::Value> reason)
{
    // 1. If ! IsWritableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_writable_stream_locked(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot abort a locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. Return ! WritableStreamAbort(this, reason).
    return writable_stream_abort(realm, *this, reason.value_or(JS::js_undefined()));
}

// https://streams.spec.whatwg.org/#ws-get-writer
WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> WritableStream::get_writer(JS::Realm& realm)
{
    // 1. Return ? AcquireWritableStreamDefaultWriter(this).
    return acquire_writable_stream_default_writer(realm, *this);
}

// https://streams.spec.whatwg.org/#ref-for-transfer-steps①
WebIDL::ExceptionOr<void> WritableStream::transfer_steps(JS::Realm& realm, HTML::TransferDataEncoder& data_holder)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. If ! IsWritableStreamLocked(value) is true, throw a "DataCloneError" DOMException.
    if (is_writable_stream_locked(*this))
        return WebIDL::DataCloneError::create("Cannot transfer locked WritableStream"_utf16);

    // 2. Let port1 be a new MessagePort in the current Realm.
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    auto port1 = HTML::MessagePort::create(global_scope->this_impl());

    // 3. Let port2 be a new MessagePort in the current Realm.
    auto port2 = HTML::MessagePort::create(global_scope->this_impl());

    // 4. Entangle port1 and port2.
    port1->entangle_with(port2);

    // 5. Let readable be a new ReadableStream in the current Realm.
    auto readable = GC::Heap::the().allocate<ReadableStream>();

    // 6. Perform ! SetUpCrossRealmTransformReadable(readable, port1).
    set_up_cross_realm_transform_readable(realm, readable, port1);

    // 7. Let promise be ! ReadableStreamPipeTo(readable, value, false, false, false).
    auto promise = readable_stream_pipe_to(realm, readable, *this, false, false, false);

    // 8. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(promise);

    // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2, « port2 »).
    Bindings::serialize_message_port_with_transfer(realm, data_holder, port2);

    return {};
}

// https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps①
WebIDL::ExceptionOr<void> WritableStream::transfer_receiving_steps(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. Let deserializedRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[port]], the current Realm).
    auto deserialized_record = MUST(HTML::structured_deserialize_with_transfer_internal(data_holder, realm));

    // 2. Let port be deserializedRecord.[[Deserialized]].
    auto* port = Bindings::message_port_from_object(deserialized_record.as_object());
    VERIFY(port);

    // 3. Perform ! SetUpCrossRealmTransformWritable(value, port).
    set_up_cross_realm_transform_writable(realm, *this, *port);

    return {};
}

}
