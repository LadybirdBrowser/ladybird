/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamAsyncIteratorPrototype.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamAsyncIterator.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<ReadableStreamAsyncIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = realm.create<ReadableStreamAsyncIteratorPrototype>(realm);
    m_prototypes.set("ReadableStreamAsyncIterator"_fly_string, prototype);
}

}

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStreamAsyncIterator);

// https://streams.spec.whatwg.org/#ref-for-asynchronous-iterator-initialization-steps
WebIDL::ExceptionOr<GC::Ref<ReadableStreamAsyncIterator>> ReadableStreamAsyncIterator::create(JS::Realm& realm, JS::Object::PropertyKind kind, ReadableStream& stream, ReadableStreamIteratorOptions options)
{
    // 1. Let reader be ? AcquireReadableStreamDefaultReader(stream).
    // 2. Set iterator’s reader to reader.
    auto reader = TRY(acquire_readable_stream_default_reader(stream));

    // 3. Let preventCancel be args[0]["preventCancel"].
    // 4. Set iterator’s prevent cancel to preventCancel.
    auto prevent_cancel = options.prevent_cancel;

    return realm.create<ReadableStreamAsyncIterator>(realm, kind, reader, prevent_cancel);
}

ReadableStreamAsyncIterator::ReadableStreamAsyncIterator(JS::Realm& realm, JS::Object::PropertyKind kind, GC::Ref<ReadableStreamDefaultReader> reader, bool prevent_cancel)
    : AsyncIterator(realm, kind)
    , m_reader(reader)
    , m_prevent_cancel(prevent_cancel)
{
}

ReadableStreamAsyncIterator::~ReadableStreamAsyncIterator() = default;

void ReadableStreamAsyncIterator::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStreamAsyncIterator);
    Base::initialize(realm);
}

void ReadableStreamAsyncIterator::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_reader);
}

class ReadableStreamAsyncIteratorReadRequest final : public ReadRequest {
    GC_CELL(ReadableStreamAsyncIteratorReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(ReadableStreamAsyncIteratorReadRequest);

public:
    ReadableStreamAsyncIteratorReadRequest(JS::Realm& realm, ReadableStreamDefaultReader& reader, WebIDL::Promise& promise)
        : m_realm(realm)
        , m_reader(reader)
        , m_promise(promise)
    {
    }

    // chunk steps, given chunk
    virtual void on_chunk(JS::Value chunk) override
    {
        // 1. Resolve promise with chunk.
        WebIDL::resolve_promise(m_realm, m_promise, chunk);
    }

    // close steps
    virtual void on_close() override
    {
        // 1. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(m_reader);

        // 2. Resolve promise with end of iteration.
        WebIDL::resolve_promise(m_realm, m_promise, JS::js_special_empty_value());
    }

    // error steps, given e
    virtual void on_error(JS::Value error) override
    {
        // 1. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(m_reader);

        // 2. Reject promise with e.
        WebIDL::reject_promise(m_realm, m_promise, error);
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_reader);
        visitor.visit(m_promise);
    }

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<ReadableStreamDefaultReader> m_reader;
    GC::Ref<WebIDL::Promise> m_promise;
};

GC_DEFINE_ALLOCATOR(ReadableStreamAsyncIteratorReadRequest);

// https://streams.spec.whatwg.org/#ref-for-dfn-get-the-next-iteration-result
GC::Ref<WebIDL::Promise> ReadableStreamAsyncIterator::next_iteration_result(JS::Realm& realm)
{
    // 1. Let reader be iterator’s reader.
    // 2. Assert: reader.[[stream]] is not undefined.
    VERIFY(m_reader->stream());

    // 3. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 4. Let readRequest be a new read request with the following items:
    auto read_request = heap().allocate<ReadableStreamAsyncIteratorReadRequest>(realm, m_reader, promise);

    // 5. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
    readable_stream_default_reader_read(m_reader, read_request);

    // 6. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#ref-for-asynchronous-iterator-return
GC::Ref<WebIDL::Promise> ReadableStreamAsyncIterator::iterator_return(JS::Realm& realm, JS::Value arg)
{
    // 1. Let reader be iterator’s reader.
    // 2. Assert: reader.[[stream]] is not undefined.
    VERIFY(m_reader->stream());

    // 3. Assert: reader.[[readRequests]] is empty, as the async iterator machinery guarantees that any previous calls
    //    to next() have settled before this is called.
    VERIFY(m_reader->read_requests().is_empty());

    // 4. If iterator’s prevent cancel is false:
    if (!m_prevent_cancel) {
        // 1. Let result be ! ReadableStreamReaderGenericCancel(reader, arg).
        auto result = readable_stream_reader_generic_cancel(m_reader, arg);

        // 2. Perform ! ReadableStreamDefaultReaderRelease(reader).
        readable_stream_default_reader_release(m_reader);

        // 3. Return result.
        return result;
    }

    // 5. Perform ! ReadableStreamDefaultReaderRelease(reader).
    readable_stream_default_reader_release(m_reader);

    // 6. Return a promise resolved with undefined.
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

}
