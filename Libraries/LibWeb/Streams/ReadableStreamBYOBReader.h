/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ReadableStreamBYOBReader.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Streams {

using ReadableStreamBYOBReaderReadOptions = Bindings::ReadableStreamBYOBReaderReadOptions;

// https://streams.spec.whatwg.org/#read-into-request
class ReadIntoRequest : public JS::Cell {
    GC_CELL(ReadIntoRequest, JS::Cell);

public:
    virtual ~ReadIntoRequest() = default;

    // An algorithm taking a chunk, called when a chunk is available for reading
    virtual void on_chunk(JS::Value chunk) = 0;

    // An algorithm taking a chunk or undefined, called when no chunks are available because the stream is closed
    virtual void on_close(JS::Value chunk_or_undefined) = 0;

    // An algorithm taking a JavaScript value, called when no chunks are available because the stream is errored
    virtual void on_error(JS::Value error) = 0;
};

// https://streams.spec.whatwg.org/#readablestreambyobreader
class ReadableStreamBYOBReader final
    : public Bindings::Wrappable
    , public ReadableStreamGenericReaderMixin {
    WEB_WRAPPABLE(ReadableStreamBYOBReader, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ReadableStreamBYOBReader);

public:
    static WebIDL::ExceptionOr<GC::Ref<ReadableStreamBYOBReader>> create(JS::Realm&, GC::Ref<ReadableStream>);

    virtual ~ReadableStreamBYOBReader() override = default;

    GC::Ref<WebIDL::Promise> read(WebIDL::ArrayBufferView, ReadableStreamBYOBReaderReadOptions options = {});

    void release_lock();

    Vector<GC::Ref<ReadIntoRequest>>& read_into_requests() { return m_read_into_requests; }

private:
    ReadableStreamBYOBReader();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#readablestreambyobreader-readintorequests
    // A list of read-into requests, used when a consumer requests chunks sooner than they are available
    Vector<GC::Ref<ReadIntoRequest>> m_read_into_requests;
};

}
