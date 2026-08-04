/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#typedefdef-readablestreamreader
using ReadableStreamReader = Variant<GC::Ref<ReadableStreamDefaultReader>, GC::Ref<ReadableStreamBYOBReader>>;

// https://streams.spec.whatwg.org/#typedefdef-readablestreamcontroller
using ReadableStreamController = Variant<GC::Ref<ReadableStreamDefaultController>, GC::Ref<ReadableByteStreamController>>;

using ReadableWritablePair = Bindings::ReadableWritablePair;
using StreamPipeOptions = Bindings::StreamPipeOptions;
using ReadableStreamGetReaderOptions = Bindings::ReadableStreamGetReaderOptions;
using ReadableStreamIteratorOptions = Bindings::ReadableStreamIteratorOptions;

struct ReadableStreamPair {
    // Define a couple container-like methods so this type may be used as the return type of the IDL `tee` implementation.
    size_t size() const { return 2; }

    GC::Ref<ReadableStream>& at(size_t index)
    {
        if (index == 0)
            return first;
        if (index == 1)
            return second;
        VERIFY_NOT_REACHED();
    }

    GC::Ref<ReadableStream> first;
    GC::Ref<ReadableStream> second;
};

// https://streams.spec.whatwg.org/#readablestream
class ReadableStream final
    : public Bindings::GCAllocatedWrappable
    , public Bindings::Transferable {
    WEB_WRAPPABLE(ReadableStream, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(ReadableStream);

public:
    enum class State {
        Readable,
        Closed,
        Errored,
    };

    static WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create_for_constructor(JS::Object&, GC::Ptr<JS::Object> underlying_source_object, QueuingStrategy const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create(JS::Realm&, GC::Ptr<JS::Object> underlying_source_object, UnderlyingSource const&, QueuingStrategy const& = {});

    static WebIDL::ExceptionOr<GC::Ref<ReadableStream>> from(JS::Realm&, JS::Value async_iterable);

    virtual ~ReadableStream() override;

    bool locked() const;
    GC::Ref<WebIDL::Promise> cancel(Optional<JS::Value> reason);
    WebIDL::ExceptionOr<ReadableStreamReader> get_reader(ReadableStreamGetReaderOptions const& = {});
    WebIDL::ExceptionOr<GC::Ref<ReadableStream>> pipe_through(ReadableWritablePair transform, StreamPipeOptions const& = {});
    GC::Ref<WebIDL::Promise> pipe_to(WritableStream& destination, StreamPipeOptions const& = {});
    WebIDL::ExceptionOr<GC::Ref<ReadableStreamAsyncIterator>> values(ReadableStreamIteratorOptions);
    WebIDL::ExceptionOr<ReadableStreamPair> tee();

    void close();
    void error(JS::Value);

    Optional<ReadableStreamController>& controller() { return m_controller; }
    void set_controller(Optional<ReadableStreamController> value) { m_controller = move(value); }

    JS::Value stored_error() const { return m_stored_error; }
    void set_stored_error(JS::Value value) { m_stored_error = value; }

    Optional<ReadableStreamReader> const& reader() const { return m_reader; }
    void set_reader(Optional<ReadableStreamReader> value) { m_reader = move(value); }

    bool is_disturbed() const;
    void set_disturbed(bool value) { m_disturbed = value; }

    bool is_readable() const;
    bool is_closed() const;
    bool is_errored() const;
    bool is_locked() const;

    State state() const { return m_state; }
    void set_state(State value) { m_state = value; }

    JS::Realm& realm() const { return *m_realm; }
    void set_realm(JS::Realm& realm) { m_realm = realm; }
    GC::Ptr<JS::Realm> result_realm() const { return m_result_realm; }
    void set_result_realm(JS::Realm& realm) { m_result_realm = realm; }

    WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> get_a_reader();
    WebIDL::ExceptionOr<void> pull_from_bytes(ByteBuffer);
    WebIDL::ExceptionOr<void> enqueue(JS::Value chunk);
    void set_up_with_byte_reading_support(JS::Realm&, GC::Ptr<PullAlgorithm> = {}, GC::Ptr<CancelAlgorithm> = {}, double high_water_mark = 0);
    GC::Ref<ReadableStream> piped_through(GC::Ref<TransformStream>, bool prevent_close = false, bool prevent_abort = false, bool prevent_cancel = false, GC::Ptr<DOM::AbortSignal> signal = {});

    Optional<WebIDL::ArrayBufferView> current_byob_request_view();

    // ^Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(JS::Realm&, HTML::TransferDataEncoder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(JS::Realm&, HTML::TransferDataDecoder&) override;
    virtual HTML::TransferType primary_interface() const override { return HTML::TransferType::ReadableStream; }

private:
    ReadableStream();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<JS::Realm> m_realm;
    GC::Ptr<JS::Realm> m_result_realm;

    // https://streams.spec.whatwg.org/#readablestream-controller
    // A ReadableStreamDefaultController or ReadableByteStreamController created with the ability to control the state and queue of this stream
    Optional<ReadableStreamController> m_controller;

    // https://streams.spec.whatwg.org/#readablestream-disturbed
    // A boolean flag set to true when the stream has been read from or canceled
    bool m_disturbed { false };

    // https://streams.spec.whatwg.org/#readablestream-reader
    // A ReadableStreamDefaultReader or ReadableStreamBYOBReader instance, if the stream is locked to a reader, or undefined if it is not
    Optional<ReadableStreamReader> m_reader;

    // https://streams.spec.whatwg.org/#readablestream-state
    // A string containing the stream’s current state, used internally; one of "readable", "closed", or "errored"
    State m_state { State::Readable };

    // https://streams.spec.whatwg.org/#readablestream-storederror
    // A value indicating how the stream failed, to be given as a failure reason or exception when trying to operate on an errored stream
    JS::Value m_stored_error { JS::js_undefined() };
};

}
