/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-body
class Body final : public JS::Cell {
    JS_CELL(Body, JS::Cell);
    JS_DECLARE_ALLOCATOR(Body);

public:
    using SourceType = Variant<Empty, ByteBuffer, JS::Handle<FileAPI::Blob>>;
    // processBody must be an algorithm accepting a byte sequence.
    using ProcessBodyCallback = JS::NonnullGCPtr<JS::HeapFunction<void(ByteBuffer)>>;
    // processBodyError must be an algorithm optionally accepting an exception.
    using ProcessBodyErrorCallback = JS::NonnullGCPtr<JS::HeapFunction<void(JS::Value)>>;
    // processBodyChunk must be an algorithm accepting a byte sequence.
    using ProcessBodyChunkCallback = JS::NonnullGCPtr<JS::HeapFunction<void(ByteBuffer)>>;
    // processEndOfBody must be an algorithm accepting no arguments
    using ProcessEndOfBodyCallback = JS::NonnullGCPtr<JS::HeapFunction<void()>>;

    [[nodiscard]] static JS::NonnullGCPtr<Body> create(JS::VM&, JS::NonnullGCPtr<Streams::ReadableStream>);
    [[nodiscard]] static JS::NonnullGCPtr<Body> create(JS::VM&, JS::NonnullGCPtr<Streams::ReadableStream>, SourceType, Optional<u64>);

    [[nodiscard]] JS::NonnullGCPtr<Streams::ReadableStream> stream() const { return *m_stream; }
    void set_stream(JS::NonnullGCPtr<Streams::ReadableStream> value) { m_stream = value; }
    [[nodiscard]] SourceType const& source() const { return m_source; }
    [[nodiscard]] Optional<u64> const& length() const { return m_length; }

    [[nodiscard]] JS::NonnullGCPtr<Body> clone(JS::Realm&);

    void fully_read(JS::Realm&, ProcessBodyCallback process_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination) const;
    void incrementally_read(ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination);
    void incrementally_read_loop(Streams::ReadableStreamDefaultReader& reader, JS::NonnullGCPtr<JS::Object> task_destination, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error);

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    explicit Body(JS::NonnullGCPtr<Streams::ReadableStream>);
    Body(JS::NonnullGCPtr<Streams::ReadableStream>, SourceType, Optional<u64>);

    // https://fetch.spec.whatwg.org/#concept-body-stream
    // A stream (a ReadableStream object).
    JS::NonnullGCPtr<Streams::ReadableStream> m_stream;

    // https://fetch.spec.whatwg.org/#concept-body-source
    // A source (null, a byte sequence, a Blob object, or a FormData object), initially null.
    SourceType m_source;

    // https://fetch.spec.whatwg.org/#concept-body-total-bytes
    // A length (null or an integer), initially null.
    Optional<u64> m_length;
};

// https://fetch.spec.whatwg.org/#body-with-type
// A body with type is a tuple that consists of a body (a body) and a type (a header value or null).
struct BodyWithType {
    JS::NonnullGCPtr<Body> body;
    Optional<ByteBuffer> type;
};

WebIDL::ExceptionOr<JS::NonnullGCPtr<Body>> byte_sequence_as_body(JS::Realm&, ReadonlyBytes);

}
