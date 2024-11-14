/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ReadableStreamPrototype.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/QueuingStrategy.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#typedefdef-readablestreamreader
using ReadableStreamReader = Variant<GC::Ref<ReadableStreamDefaultReader>, GC::Ref<ReadableStreamBYOBReader>>;

// https://streams.spec.whatwg.org/#typedefdef-readablestreamcontroller
using ReadableStreamController = Variant<GC::Ref<ReadableStreamDefaultController>, GC::Ref<ReadableByteStreamController>>;

// https://streams.spec.whatwg.org/#dictdef-readablestreamgetreaderoptions
struct ReadableStreamGetReaderOptions {
    Optional<Bindings::ReadableStreamReaderMode> mode;
};

struct ReadableWritablePair {
    GC::Ptr<ReadableStream> readable;
    GC::Ptr<WritableStream> writable;
};

struct StreamPipeOptions {
    bool prevent_close { false };
    bool prevent_abort { false };
    bool prevent_cancel { false };
    GC::Ptr<DOM::AbortSignal> signal;
};

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
class ReadableStream final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ReadableStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ReadableStream);

public:
    enum class State {
        Readable,
        Closed,
        Errored,
    };

    static WebIDL::ExceptionOr<GC::Ref<ReadableStream>> construct_impl(JS::Realm&, Optional<GC::Root<JS::Object>> const& underlying_source, QueuingStrategy const& = {});

    static WebIDL::ExceptionOr<GC::Ref<ReadableStream>> from(JS::VM& vm, JS::Value async_iterable);

    virtual ~ReadableStream() override;

    bool locked() const;
    GC::Ref<WebIDL::Promise> cancel(JS::Value reason);
    WebIDL::ExceptionOr<ReadableStreamReader> get_reader(ReadableStreamGetReaderOptions const& = {});
    WebIDL::ExceptionOr<GC::Ref<ReadableStream>> pipe_through(ReadableWritablePair transform, StreamPipeOptions const& = {});
    GC::Ref<WebIDL::Promise> pipe_to(WritableStream& destination, StreamPipeOptions const& = {});
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

private:
    explicit ReadableStream(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#readablestream-controller
    // A ReadableStreamDefaultController or ReadableByteStreamController created with the ability to control the state and queue of this stream
    Optional<ReadableStreamController> m_controller;

    // https://streams.spec.whatwg.org/#readablestream-detached
    // A boolean flag set to true when the stream is transferred
    bool m_detached { false };

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
