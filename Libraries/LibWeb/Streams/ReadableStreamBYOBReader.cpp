/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamBYOBReaderPrototype.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStreamBYOBReader);

ReadableStreamBYOBReader::ReadableStreamBYOBReader(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
    , ReadableStreamGenericReaderMixin(realm)
{
}

void ReadableStreamBYOBReader::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStreamBYOBReader);
}

// https://streams.spec.whatwg.org/#byob-reader-constructor
WebIDL::ExceptionOr<GC::Ref<ReadableStreamBYOBReader>> ReadableStreamBYOBReader::construct_impl(JS::Realm& realm, GC::Ref<ReadableStream> stream)
{
    auto reader = realm.create<ReadableStreamBYOBReader>(realm);

    // 1. Perform ? SetUpReadableStreamBYOBReader(this, stream).
    TRY(set_up_readable_stream_byob_reader(reader, *stream));

    return reader;
}

// https://streams.spec.whatwg.org/#byob-reader-release-lock
void ReadableStreamBYOBReader::release_lock()
{
    // 1. If this.[[stream]] is undefined, return.
    if (!m_stream)
        return;

    // 2. Perform ! ReadableStreamBYOBReaderRelease(this).
    readable_stream_byob_reader_release(*this);
}

void ReadableStreamBYOBReader::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    ReadableStreamGenericReaderMixin::visit_edges(visitor);
    visitor.visit(m_read_into_requests);
}

class BYOBReaderReadIntoRequest : public ReadIntoRequest {
    GC_CELL(BYOBReaderReadIntoRequest, ReadIntoRequest);
    GC_DECLARE_ALLOCATOR(BYOBReaderReadIntoRequest);

public:
    BYOBReaderReadIntoRequest(JS::Realm& realm, WebIDL::Promise& promise)
        : m_realm(realm)
        , m_promise(promise)
    {
    }

    // chunk steps, given chunk
    virtual void on_chunk(JS::Value chunk) override
    {
        // 1. Resolve promise with «[ "value" → chunk, "done" → false ]».
        WebIDL::resolve_promise(m_realm, m_promise, JS::create_iterator_result_object(m_realm->vm(), chunk, false));
    }

    // close steps, given chunk
    virtual void on_close(JS::Value chunk) override
    {
        // 1. Resolve promise with «[ "value" → chunk, "done" → true ]».
        WebIDL::resolve_promise(m_realm, m_promise, JS::create_iterator_result_object(m_realm->vm(), chunk, true));
    }

    // error steps, given e
    virtual void on_error(JS::Value error) override
    {
        // 1. Reject promise with e.
        WebIDL::reject_promise(m_realm, m_promise, error);
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_promise);
    }

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<WebIDL::Promise> m_promise;
};

GC_DEFINE_ALLOCATOR(BYOBReaderReadIntoRequest);

// https://streams.spec.whatwg.org/#byob-reader-read
GC::Ref<WebIDL::Promise> ReadableStreamBYOBReader::read(GC::Root<WebIDL::ArrayBufferView>& view, ReadableStreamBYOBReaderReadOptions options)
{
    auto& realm = this->realm();

    // 1. If view.[[ByteLength]] is 0, return a promise rejected with a TypeError exception.
    if (view->byte_length() == 0) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Cannot read in an empty buffer"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 2. If view.[[ViewedArrayBuffer]].[[ArrayBufferByteLength]] is 0, return a promise rejected with a TypeError exception.
    if (view->viewed_array_buffer()->byte_length() == 0) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Cannot read in an empty buffer"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 3. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, return a promise rejected with a TypeError exception.
    if (view->viewed_array_buffer()->is_detached()) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Cannot read in a detached buffer"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 4. If options["min"] is 0, return a promise rejected with a TypeError exception.
    if (options.min == 0) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "options[\"min\'] cannot have a value of 0."sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 5. If view has a [[TypedArrayName]] internal slot,
    if (view->is_typed_array_base()) {
        auto const& typed_array = *view->bufferable_object().get<GC::Ref<JS::TypedArrayBase>>();

        // 1. If options["min"] > view.[[ArrayLength]], return a promise rejected with a RangeError exception.
        if (options.min > typed_array.array_length().length()) {
            WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::RangeError, "options[\"min\"] cannot be larger than the length of the view."sv };
            return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
        }
    }

    // 6. Otherwise (i.e., it is a DataView),
    if (view->is_data_view()) {
        // 1. If options["min"] > view.[[ByteLength]], return a promise rejected with a RangeError exception.
        if (options.min > view->byte_length()) {
            WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::RangeError, "options[\"min\"] cannot be larger than the length of the view."sv };
            return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
        }
    }

    // 7. If this.[[stream]] is undefined, return a promise rejected with a TypeError exception.
    if (!m_stream) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Cannot read from an empty stream"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 8. Let promise be a new promise.
    auto promise_capability = WebIDL::create_promise(realm);

    // 9. Let readIntoRequest be a new read-into request with the following items:
    //    chunk steps, given chunk
    //        Resolve promise with «[ "value" → chunk, "done" → false ]».
    //    close steps, given chunk
    //        Resolve promise with «[ "value" → chunk, "done" → true ]».
    //    error steps, given e
    //        Reject promise with e.
    auto read_into_request = heap().allocate<BYOBReaderReadIntoRequest>(realm, promise_capability);

    // 10. Perform ! ReadableStreamBYOBReaderRead(this, view, options["min"], readIntoRequest).
    readable_stream_byob_reader_read(*this, *view, options.min, *read_into_request);

    // 11. Return promise.
    return promise_capability;
}
}
