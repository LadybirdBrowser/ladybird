/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Streams {

extern "C" {

// Defined in Bindings/Rust/src/streams/exports.rs.
void* web_rust_readable_stream_create(JSVM*, void* realm);
void web_rust_readable_stream_set_up_with_byte_reading_support(JSVM*, void* realm, void* stream, PullAlgorithm* pull_algorithm, CancelAlgorithm* cancel_algorithm, double high_water_mark);
u8 web_rust_readable_stream_state(void* stream);
bool web_rust_readable_stream_is_locked(void* stream);
bool web_rust_readable_stream_is_disturbed(void* stream);
bool web_rust_readable_stream_close_requested(void* stream);
bool web_rust_readable_stream_is_readable_byte_stream(void* stream);
JSCompletion web_rust_readable_stream_enqueue(JSVM*, void* stream, JSValue chunk);
JSCompletion web_rust_readable_stream_enqueue_native_bytes(JSVM*, void* stream, ReadonlyBytes);
bool web_rust_readable_stream_current_byob_request_view_byte_length(JSVM*, void* stream, u64* byte_length);
JSCompletion web_rust_readable_stream_pull_from_bytes(JSVM*, void* stream, ReadonlyBytes);
void web_rust_readable_stream_close(JSVM*, void* stream);
void web_rust_readable_stream_error(JSVM*, void* stream, JSValue error);
void* web_rust_readable_stream_cancel(JSVM*, void* stream, JSValue reason);
JSCompletion web_rust_readable_stream_tee(JSVM*, void* stream, void** branch1, void** branch2);
JSCompletion web_rust_readable_stream_get_a_reader(JSVM*, void* stream, void** reader);
void* web_rust_readable_stream_piped_through(JSVM*, void* stream, void* transform, bool prevent_close, bool prevent_abort, bool prevent_cancel);
void web_rust_readable_stream_default_reader_read_a_chunk(JSVM*, void* reader, ReadRequest* read_request);
void web_rust_readable_stream_default_reader_read_all_bytes(JSVM*, void* reader, ReadableStreamDefaultReader::SuccessSteps* success_steps, ReadableStreamDefaultReader::FailureSteps* failure_steps);
void* web_rust_readable_stream_default_reader_read(JSVM*, void* reader);
void* web_rust_readable_stream_default_reader_cancel(JSVM*, void* reader, JSValue reason);
void* web_rust_readable_stream_default_reader_closed_promise_realm(void* reader);

// Called by Bindings/Rust/src/streams.
void* web_streams_call_promise_algorithm(GC::Function<GC::Ref<WebIDL::Promise>()>*);
void* web_streams_call_promise_algorithm_with_argument(GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>*, JSValue argument);
void web_streams_read_request_chunk_steps(ReadRequest*, JSValue chunk);
void web_streams_read_request_close_steps(ReadRequest*);
void web_streams_read_request_error_steps(ReadRequest*, JSValue error);
void web_streams_call_bytes_steps(ReadableStreamDefaultReader::SuccessSteps*, ReadonlyBytes);
void web_streams_call_value_steps(ReadableStreamDefaultReader::FailureSteps*, JSValue);
void web_streams_complete_rendering_pending_stream(ReadableStream*);
JSCompletion web_streams_structured_clone(void* realm, JSValue);
void* web_streams_abort_signal_from_wrapper(JSObject* wrapper);
JSObject* web_streams_abort_signal_wrapper(void* realm, DOM::AbortSignal*);
void* web_streams_create_abort_signal();
bool web_streams_abort_signal_aborted(DOM::AbortSignal*);
JSValue web_streams_abort_signal_reason(DOM::AbortSignal*);
bool web_streams_abort_signal_add_algorithm(DOM::AbortSignal*, JSObject* function, u64* id);
void web_streams_abort_signal_remove_algorithm(DOM::AbortSignal*, u64 id);
void web_streams_abort_signal_signal_abort(DOM::AbortSignal*, JSValue reason, void* realm);
void web_streams_create_entangled_ports(void* realm, void** port1, void** port2);
void web_streams_message_port_add_handlers(void* realm, HTML::MessagePort*, JSObject* on_message, JSObject* on_message_error);
JSValue web_streams_message_event_data(JSValue event);
JSCompletion web_streams_message_port_post_message(void* realm, HTML::MessagePort*, JSValue message);
void web_streams_message_port_disentangle(HTML::MessagePort*);
void web_streams_message_port_enable(HTML::MessagePort*);
void* web_streams_message_port_from_object(JSObject*);
JSObject* web_streams_message_port_wrapper(void* realm, HTML::MessagePort*);
void web_streams_serialize_with_transfer(Bindings::RustTransferDataWriter*, JSValue);
JSValue web_streams_deserialize_with_transfer(Bindings::RustTransferDataReader*);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

static JS::Realm& realm_of(void* realm)
{
    return *static_cast<JS::Realm*>(realm);
}

static WebIDL::ExceptionOr<void> exception_or_void(JSCompletion completion)
{
    auto result = JS::HostABI::completion_from_abi<JS::Value>(completion);
    if (result.is_error())
        return result.release_error();
    return {};
}

GC::Ref<ReadableStream> ReadableStream::create(JS::Realm& realm)
{
    return GC::Ref { *static_cast<ReadableStream*>(web_rust_readable_stream_create(vm_abi(), &realm)) };
}

void ReadableStream::set_up_with_byte_reading_support(JS::Realm& realm, GC::Ptr<PullAlgorithm> pull_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm, double high_water_mark)
{
    web_rust_readable_stream_set_up_with_byte_reading_support(vm_abi(), &realm, this, pull_algorithm.ptr(), cancel_algorithm.ptr(), high_water_mark);
}

bool ReadableStream::is_readable_byte_stream() const
{
    return web_rust_readable_stream_is_readable_byte_stream(const_cast<ReadableStream*>(this));
}

bool ReadableStream::is_readable() const
{
    return web_rust_readable_stream_state(const_cast<ReadableStream*>(this)) == 0;
}

bool ReadableStream::is_closed() const
{
    return web_rust_readable_stream_state(const_cast<ReadableStream*>(this)) == 1;
}

bool ReadableStream::is_errored() const
{
    return web_rust_readable_stream_state(const_cast<ReadableStream*>(this)) == 2;
}

bool ReadableStream::is_locked() const
{
    return web_rust_readable_stream_is_locked(const_cast<ReadableStream*>(this));
}

bool ReadableStream::is_disturbed() const
{
    return web_rust_readable_stream_is_disturbed(const_cast<ReadableStream*>(this));
}

bool ReadableStream::close_requested() const
{
    return web_rust_readable_stream_close_requested(const_cast<ReadableStream*>(this));
}

WebIDL::ExceptionOr<void> ReadableStream::enqueue(JS::Value chunk)
{
    return exception_or_void(web_rust_readable_stream_enqueue(vm_abi(), this, JS::HostABI::value_to_abi(chunk)));
}

WebIDL::ExceptionOr<void> ReadableStream::enqueue_bytes(ReadonlyBytes bytes)
{
    return exception_or_void(web_rust_readable_stream_enqueue_native_bytes(vm_abi(), this, bytes));
}

Optional<u64> ReadableStream::current_byob_request_view_byte_length()
{
    u64 byte_length = 0;
    if (!web_rust_readable_stream_current_byob_request_view_byte_length(vm_abi(), this, &byte_length))
        return {};
    return byte_length;
}

WebIDL::ExceptionOr<void> ReadableStream::pull_from_bytes(ReadonlyBytes bytes)
{
    return exception_or_void(web_rust_readable_stream_pull_from_bytes(vm_abi(), this, bytes));
}

void ReadableStream::close()
{
    web_rust_readable_stream_close(vm_abi(), this);
}

void ReadableStream::error(JS::Value error)
{
    web_rust_readable_stream_error(vm_abi(), this, JS::HostABI::value_to_abi(error));
}

GC::Ref<WebIDL::Promise> ReadableStream::cancel(JS::Value reason)
{
    return GC::Ref { *static_cast<WebIDL::Promise*>(web_rust_readable_stream_cancel(vm_abi(), this, JS::HostABI::value_to_abi(reason))) };
}

WebIDL::ExceptionOr<ReadableStreamPair> ReadableStream::tee()
{
    void* branch1 = nullptr;
    void* branch2 = nullptr;
    TRY(exception_or_void(web_rust_readable_stream_tee(vm_abi(), this, &branch1, &branch2)));
    return ReadableStreamPair { *static_cast<ReadableStream*>(branch1), *static_cast<ReadableStream*>(branch2) };
}

WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> ReadableStream::get_a_reader()
{
    void* reader = nullptr;
    TRY(exception_or_void(web_rust_readable_stream_get_a_reader(vm_abi(), this, &reader)));
    return GC::Ref { *static_cast<ReadableStreamDefaultReader*>(reader) };
}

GC::Ref<ReadableStream> ReadableStream::piped_through(GC::Ref<TransformStream> transform, bool prevent_close, bool prevent_abort, bool prevent_cancel)
{
    return GC::Ref { *static_cast<ReadableStream*>(web_rust_readable_stream_piped_through(vm_abi(), this, transform.ptr(), prevent_close, prevent_abort, prevent_cancel)) };
}

GC::Ref<WebIDL::Promise> ReadableStreamDefaultReader::read()
{
    return GC::Ref { *static_cast<WebIDL::Promise*>(web_rust_readable_stream_default_reader_read(vm_abi(), this)) };
}

void ReadableStreamDefaultReader::read_a_chunk(ReadRequest& read_request)
{
    web_rust_readable_stream_default_reader_read_a_chunk(vm_abi(), this, &read_request);
}

void ReadableStreamDefaultReader::read_all_bytes(GC::Ref<SuccessSteps> success_steps, GC::Ref<FailureSteps> failure_steps)
{
    web_rust_readable_stream_default_reader_read_all_bytes(vm_abi(), this, success_steps.ptr(), failure_steps.ptr());
}

// FIXME: This function is a promise-based wrapper around "read all bytes". The spec changed this function to not use promises
//        in https://github.com/whatwg/streams/commit/f894acdd417926a2121710803cef593e15127964 - however, it seems that the
//        FileAPI blob specification has not been updated to match, see: https://github.com/w3c/FileAPI/issues/187.
GC::Ref<WebIDL::Promise> ReadableStreamDefaultReader::read_all_bytes_deprecated()
{
    auto& realm = closed_promise_realm();

    auto promise = WebIDL::create_promise(realm);

    auto success_steps = GC::create_function(GC::Heap::the(), [promise, &realm](ByteBuffer bytes) {
        auto buffer = JS::ArrayBuffer::create(realm, move(bytes));
        WebIDL::resolve_promise(promise, buffer);
    });

    auto failure_steps = GC::create_function(GC::Heap::the(), [promise](JS::Value error) {
        WebIDL::reject_promise(promise, error);
    });

    read_all_bytes(success_steps, failure_steps);

    return promise;
}

GC::Ref<WebIDL::Promise> ReadableStreamDefaultReader::cancel(JS::Value reason)
{
    return GC::Ref { *static_cast<WebIDL::Promise*>(web_rust_readable_stream_default_reader_cancel(vm_abi(), this, JS::HostABI::value_to_abi(reason))) };
}

JS::Realm& ReadableStreamDefaultReader::closed_promise_realm() const
{
    return realm_of(web_rust_readable_stream_default_reader_closed_promise_realm(const_cast<ReadableStreamDefaultReader*>(this)));
}

void* web_streams_call_promise_algorithm(GC::Function<GC::Ref<WebIDL::Promise>()>* algorithm)
{
    return algorithm->function()().ptr();
}

void* web_streams_call_promise_algorithm_with_argument(GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>* algorithm, JSValue argument)
{
    return algorithm->function()(JS::HostABI::value_from_abi(argument)).ptr();
}

void web_streams_read_request_chunk_steps(ReadRequest* read_request, JSValue chunk)
{
    read_request->on_chunk(JS::HostABI::value_from_abi(chunk));
}

void web_streams_read_request_close_steps(ReadRequest* read_request)
{
    read_request->on_close();
}

void web_streams_read_request_error_steps(ReadRequest* read_request, JSValue error)
{
    read_request->on_error(JS::HostABI::value_from_abi(error));
}

void web_streams_call_bytes_steps(ReadableStreamDefaultReader::SuccessSteps* steps, ReadonlyBytes bytes)
{
    steps->function()(MUST(ByteBuffer::copy(bytes)));
}

void web_streams_call_value_steps(ReadableStreamDefaultReader::FailureSteps* steps, JSValue value)
{
    steps->function()(JS::HostABI::value_from_abi(value));
}

void web_streams_complete_rendering_pending_stream(ReadableStream* stream)
{
    complete_rendering_pending_stream(*stream);
}

// https://streams.spec.whatwg.org/#abstract-opdef-structuredclone
JSCompletion web_streams_structured_clone(void* realm, JSValue value)
{
    auto& js_realm = realm_of(realm);
    auto& vm = js_realm.vm();
    auto result = [&]() -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Let serialized be ? StructuredSerialize(v).
        auto serialized = TRY(HTML::structured_serialize(vm, JS::HostABI::value_from_abi(value)));

        // 2. Return ? StructuredDeserialize(serialized, the current Realm).
        return TRY(HTML::structured_deserialize(vm, serialized, js_realm));
    }();
    return Bindings::completion_to_abi(js_realm, move(result));
}

void* web_streams_abort_signal_from_wrapper(JSObject* wrapper)
{
    auto* signal = Bindings::impl_from<DOM::AbortSignal>(JS::HostABI::object_from_abi(wrapper));
    VERIFY(signal);
    return signal;
}

JSObject* web_streams_abort_signal_wrapper(void* realm, DOM::AbortSignal* signal)
{
    auto& js_realm = realm_of(realm);
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, GC::Ref { *signal }).ptr());
}

void* web_streams_create_abort_signal()
{
    return DOM::AbortSignal::create().ptr();
}

bool web_streams_abort_signal_aborted(DOM::AbortSignal* signal)
{
    return signal->aborted();
}

JSValue web_streams_abort_signal_reason(DOM::AbortSignal* signal)
{
    return JS::HostABI::value_to_abi(signal->reason());
}

bool web_streams_abort_signal_add_algorithm(DOM::AbortSignal* signal, JSObject* function, u64* id)
{
    GC::Ref<JS::FunctionObject> algorithm = as<JS::FunctionObject>(*JS::HostABI::object_from_abi(function));
    auto algorithm_id = signal->add_abort_algorithm([algorithm] {
        MUST(JS::call(algorithm->vm(), *algorithm, JS::js_undefined()));
    });
    if (!algorithm_id.has_value())
        return false;
    *id = algorithm_id.value();
    return true;
}

void web_streams_abort_signal_remove_algorithm(DOM::AbortSignal* signal, u64 id)
{
    signal->remove_abort_algorithm(id);
}

void web_streams_abort_signal_signal_abort(DOM::AbortSignal* signal, JSValue reason, void* realm)
{
    signal->signal_abort(JS::HostABI::value_from_abi(reason), realm_of(realm).global_object());
}

void web_streams_create_entangled_ports(void* realm, void** port1, void** port2)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm_of(realm).global_object());
    VERIFY(global_scope);
    auto first_port = HTML::MessagePort::create(global_scope->this_impl());
    auto second_port = HTML::MessagePort::create(global_scope->this_impl());
    first_port->entangle_with(second_port);
    *port1 = first_port.ptr();
    *port2 = second_port.ptr();
}

static void add_message_event_listener(JS::Realm& realm, HTML::MessagePort& port, Utf16FlyString const& name, JSObject* function)
{
    auto& callback_object = *JS::HostABI::object_from_abi(function);
    auto callback = GC::Heap::the().allocate<WebIDL::CallbackType>(callback_object, realm);
    port.add_event_listener_without_options(name, DOM::IDLEventListener::create(callback));
}

void web_streams_message_port_add_handlers(void* realm, HTML::MessagePort* port, JSObject* on_message, JSObject* on_message_error)
{
    add_message_event_listener(realm_of(realm), *port, HTML::EventNames::message, on_message);
    add_message_event_listener(realm_of(realm), *port, HTML::EventNames::messageerror, on_message_error);
}

JSValue web_streams_message_event_data(JSValue event)
{
    auto* message_event = Bindings::impl_from<HTML::MessageEvent>(&JS::HostABI::value_from_abi(event).as_object());
    VERIFY(message_event);
    return JS::HostABI::value_to_abi(message_event->data());
}

JSCompletion web_streams_message_port_post_message(void* realm, HTML::MessagePort* port, JSValue message)
{
    auto& js_realm = realm_of(realm);

    // 4. Let targetPort be the port with which port is entangled, if any; otherwise let it be null.
    auto target_port = port->entangled_port();

    // 5. Let options be «[ "transfer" → « » ]».
    HTML::StructuredSerializeOptions options;

    // 6. Run the message port post message steps providing targetPort, message, and options.
    auto result = port->message_port_post_message_steps(js_realm, target_port, JS::HostABI::value_from_abi(message), options);
    if (result.is_exception())
        return Bindings::completion_to_abi(js_realm, result.release_error());
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}

void web_streams_message_port_disentangle(HTML::MessagePort* port)
{
    port->disentangle();
}

void web_streams_message_port_enable(HTML::MessagePort* port)
{
    port->enable();
}

void* web_streams_message_port_from_object(JSObject* object)
{
    auto* port = Bindings::message_port_from_object(*JS::HostABI::object_from_abi(object));
    VERIFY(port);
    return port;
}

JSObject* web_streams_message_port_wrapper(void* realm, HTML::MessagePort* port)
{
    return JS::HostABI::object_to_abi(Bindings::message_port(realm_of(realm), *port).ptr());
}

void web_streams_serialize_with_transfer(Bindings::RustTransferDataWriter* writer, JSValue value)
{
    auto js_value = JS::HostABI::value_from_abi(value);
    auto result = MUST(HTML::structured_serialize_with_transfer(writer->realm, js_value, { { js_value.as_object() } }));
    writer->data_holder.extend(move(result.transfer_data_holders));
}

JSValue web_streams_deserialize_with_transfer(Bindings::RustTransferDataReader* reader)
{
    return JS::HostABI::value_to_abi(MUST(HTML::structured_deserialize_with_transfer_internal(reader->data_holder, reader->realm)));
}

}
