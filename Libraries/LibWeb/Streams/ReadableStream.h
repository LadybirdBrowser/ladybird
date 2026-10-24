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
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

struct ReadableStreamPair {
    GC::Ref<ReadableStream> first;
    GC::Ref<ReadableStream> second;
};

// https://streams.spec.whatwg.org/#readablestream
// The C++ side of a ReadableStream, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/streams): what
// the specifications that LibWeb implements in C++ use to set up and read streams,
// https://streams.spec.whatwg.org/#other-specs-rs
class WEB_API ReadableStream : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ReadableStream, readable_stream)

public:
    // A new ReadableStream in the realm, which set_up_with_byte_reading_support() then sets up.
    [[nodiscard]] static GC::Ref<ReadableStream> create(JS::Realm&);

    // https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
    void set_up_with_byte_reading_support(JS::Realm&, GC::Ptr<PullAlgorithm> = {}, GC::Ptr<CancelAlgorithm> = {}, double high_water_mark = 0);

    // https://streams.spec.whatwg.org/#readablestream-readable
    bool is_readable() const;

    // https://streams.spec.whatwg.org/#readablestream-closed
    bool is_closed() const;

    // https://streams.spec.whatwg.org/#readablestream-errored
    bool is_errored() const;

    // https://streams.spec.whatwg.org/#readablestream-locked
    bool is_locked() const;

    // https://streams.spec.whatwg.org/#is-readable-stream-disturbed
    bool is_disturbed() const;

    // Whether the stream's controller has been asked to close.
    bool close_requested() const;

    // https://streams.spec.whatwg.org/#readablestream-enqueue
    WebIDL::ExceptionOr<void> enqueue(JS::Value chunk);

    // ReadableByteStreamControllerEnqueue on the stream's controller, a ReadableByteStreamController, with a chunk of
    // the bytes.
    WebIDL::ExceptionOr<void> enqueue_bytes(ReadonlyBytes);

    // https://streams.spec.whatwg.org/#readablestream-close
    void close();

    // https://streams.spec.whatwg.org/#readablestream-error
    void error(JS::Value);

    // https://streams.spec.whatwg.org/#readable-stream-cancel
    GC::Ref<WebIDL::Promise> cancel(JS::Value reason);

    // https://streams.spec.whatwg.org/#readablestream-tee
    WebIDL::ExceptionOr<ReadableStreamPair> tee();

    // https://streams.spec.whatwg.org/#readablestream-get-a-reader
    WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> get_a_reader();

    // https://streams.spec.whatwg.org/#readablestream-pipe-through
    GC::Ref<ReadableStream> piped_through(GC::Ref<TransformStream>, bool prevent_close = false, bool prevent_abort = false, bool prevent_cancel = false);
};

}
