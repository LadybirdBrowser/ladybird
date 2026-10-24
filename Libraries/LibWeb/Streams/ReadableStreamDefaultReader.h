/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibGC/Function.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/ReadableStreamDefaultReader.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#read-request
class WEB_API ReadRequest : public JS::Cell {
    GC_CELL(ReadRequest, JS::Cell);

public:
    virtual ~ReadRequest() override = default;

    virtual void on_chunk(JS::Value chunk) = 0;
    virtual void on_close() = 0;
    virtual void on_error(JS::Value error) = 0;
};

// https://streams.spec.whatwg.org/#readablestreamdefaultreader
// The C++ side of a ReadableStreamDefaultReader, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/streams):
// what the specifications that LibWeb implements in C++ use to read streams.
class WEB_API ReadableStreamDefaultReader : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ReadableStreamDefaultReader, readable_stream_default_reader)

public:
    using SuccessSteps = GC::Function<void(ByteBuffer)>;
    using FailureSteps = GC::Function<void(JS::Value error)>;

    // https://streams.spec.whatwg.org/#default-reader-read
    GC::Ref<WebIDL::Promise> read();

    // https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-a-chunk
    void read_a_chunk(ReadRequest&);

    // https://streams.spec.whatwg.org/#readablestreamdefaultreader-read-all-bytes
    void read_all_bytes(GC::Ref<SuccessSteps>, GC::Ref<FailureSteps>);

    // FIXME: This function is a promise-based wrapper around "read all bytes". The spec changed this function to not use promises
    //        in https://github.com/whatwg/streams/commit/f894acdd417926a2121710803cef593e15127964 - however, it seems that the
    //        FileAPI blob specification has not been updated to match, see: https://github.com/w3c/FileAPI/issues/187.
    GC::Ref<WebIDL::Promise> read_all_bytes_deprecated();

    // https://streams.spec.whatwg.org/#generic-reader-cancel
    GC::Ref<WebIDL::Promise> cancel(JS::Value reason);

    // The realm of the reader's [[closedPromise]].
    JS::Realm& closed_promise_realm() const;
};

}
