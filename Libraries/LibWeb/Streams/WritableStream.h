/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#pending-abort-request
struct PendingAbortRequest {
    // https://streams.spec.whatwg.org/#pending-abort-request-promise
    // A promise returned from WritableStreamAbort
    GC::Ref<WebIDL::Promise> promise;

    // https://streams.spec.whatwg.org/#pending-abort-request-reason
    // A JavaScript value that was passed as the abort reason to WritableStreamAbort
    JS::Value reason;

    // https://streams.spec.whatwg.org/#pending-abort-request-was-already-erroring
    // A boolean indicating whether or not the stream was in the "erroring" state when WritableStreamAbort was called, which impacts the outcome of the abort request
    bool was_already_erroring;
};

// https://streams.spec.whatwg.org/#writablestream
class WritableStream final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WritableStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WritableStream);

public:
    enum class State {
        Writable,
        Closed,
        Erroring,
        Errored,
    };

    static WebIDL::ExceptionOr<GC::Ref<WritableStream>> construct_impl(JS::Realm& realm, Optional<GC::Root<JS::Object>> const& underlying_sink, QueuingStrategy const& = {});

    virtual ~WritableStream() = default;

    bool locked() const;
    GC::Ref<WebIDL::Promise> abort(JS::Value reason);
    GC::Ref<WebIDL::Promise> close();
    WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> get_writer();

    bool backpressure() const { return m_backpressure; }
    void set_backpressure(bool value) { m_backpressure = value; }

    GC::Ptr<WebIDL::Promise const> close_request() const { return m_close_request; }
    GC::Ptr<WebIDL::Promise> close_request() { return m_close_request; }
    void set_close_request(GC::Ptr<WebIDL::Promise> value) { m_close_request = value; }

    GC::Ptr<WritableStreamDefaultController const> controller() const { return m_controller; }
    GC::Ptr<WritableStreamDefaultController> controller() { return m_controller; }
    void set_controller(GC::Ptr<WritableStreamDefaultController> value) { m_controller = value; }

    GC::Ptr<WebIDL::Promise const> in_flight_write_request() const { return m_in_flight_write_request; }
    void set_in_flight_write_request(GC::Ptr<WebIDL::Promise> value) { m_in_flight_write_request = value; }

    GC::Ptr<WebIDL::Promise const> in_flight_close_request() const { return m_in_flight_close_request; }
    void set_in_flight_close_request(GC::Ptr<WebIDL::Promise> value) { m_in_flight_close_request = value; }

    Optional<PendingAbortRequest>& pending_abort_request() { return m_pending_abort_request; }
    void set_pending_abort_request(Optional<PendingAbortRequest>&& value) { m_pending_abort_request = move(value); }

    State state() const { return m_state; }
    void set_state(State value) { m_state = value; }

    JS::Value stored_error() const { return m_stored_error; }
    void set_stored_error(JS::Value value) { m_stored_error = value; }

    GC::Ptr<WritableStreamDefaultWriter const> writer() const { return m_writer; }
    GC::Ptr<WritableStreamDefaultWriter> writer() { return m_writer; }
    void set_writer(GC::Ptr<WritableStreamDefaultWriter> value) { m_writer = value; }

    SinglyLinkedList<GC::Ref<WebIDL::Promise>>& write_requests() { return m_write_requests; }

private:
    explicit WritableStream(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#writablestream-backpressure
    // A boolean indicating the backpressure signal set by the controller
    bool m_backpressure { false };

    // https://streams.spec.whatwg.org/#writablestream-closerequest
    // The promise returned from the writer’s close() method
    GC::Ptr<WebIDL::Promise> m_close_request;

    // https://streams.spec.whatwg.org/#writablestream-controller
    // A WritableStreamDefaultController created with the ability to control the state and queue of this stream
    GC::Ptr<WritableStreamDefaultController> m_controller;

    // https://streams.spec.whatwg.org/#writablestream-detached
    // A boolean flag set to true when the stream is transferred
    bool m_detached { false };

    // https://streams.spec.whatwg.org/#writablestream-inflightwriterequest
    // A slot set to the promise for the current in-flight write operation while the underlying sink's write algorithm is executing and has not yet fulfilled, used to prevent reentrant calls
    GC::Ptr<WebIDL::Promise> m_in_flight_write_request;

    // https://streams.spec.whatwg.org/#writablestream-inflightcloserequest
    // A slot set to the promise for the current in-flight close operation while the underlying sink's close algorithm is executing and has not yet fulfilled, used to prevent the abort() method from interrupting close
    GC::Ptr<WebIDL::Promise> m_in_flight_close_request;

    // https://streams.spec.whatwg.org/#writablestream-pendingabortrequest
    // A pending abort request
    Optional<PendingAbortRequest> m_pending_abort_request;

    // https://streams.spec.whatwg.org/#writablestream-state
    // A string containing the stream’s current state, used internally; one of "writable", "closed", "erroring", or "errored"
    State m_state { State::Writable };

    // https://streams.spec.whatwg.org/#writablestream-storederror
    // A value indicating how the stream failed, to be given as a failure reason or exception when trying to operate on the stream while in the "errored" state
    JS::Value m_stored_error { JS::js_undefined() };

    // https://streams.spec.whatwg.org/#writablestream-writer
    // A WritableStreamDefaultWriter instance, if the stream is locked to a writer, or undefined if it is not
    GC::Ptr<WritableStreamDefaultWriter> m_writer;

    // https://streams.spec.whatwg.org/#writablestream-writerequests
    // A list of promises representing the stream’s internal queue of write requests not yet processed by the underlying sink
    SinglyLinkedList<GC::Ref<WebIDL::Promise>> m_write_requests;
};

}
