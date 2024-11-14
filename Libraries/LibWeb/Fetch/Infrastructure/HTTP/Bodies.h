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
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-body
class Body final : public JS::Cell {
    GC_CELL(Body, JS::Cell);
    GC_DECLARE_ALLOCATOR(Body);

public:
    using SourceType = Variant<Empty, ByteBuffer, GC::Root<FileAPI::Blob>>;
    // processBody must be an algorithm accepting a byte sequence.
    using ProcessBodyCallback = GC::Ref<GC::Function<void(ByteBuffer)>>;
    // processBodyError must be an algorithm optionally accepting an exception.
    using ProcessBodyErrorCallback = GC::Ref<GC::Function<void(JS::Value)>>;
    // processBodyChunk must be an algorithm accepting a byte sequence.
    using ProcessBodyChunkCallback = GC::Ref<GC::Function<void(ByteBuffer)>>;
    // processEndOfBody must be an algorithm accepting no arguments
    using ProcessEndOfBodyCallback = GC::Ref<GC::Function<void()>>;

    [[nodiscard]] static GC::Ref<Body> create(JS::VM&, GC::Ref<Streams::ReadableStream>);
    [[nodiscard]] static GC::Ref<Body> create(JS::VM&, GC::Ref<Streams::ReadableStream>, SourceType, Optional<u64>);

    [[nodiscard]] GC::Ref<Streams::ReadableStream> stream() const { return *m_stream; }
    void set_stream(GC::Ref<Streams::ReadableStream> value) { m_stream = value; }
    [[nodiscard]] SourceType const& source() const { return m_source; }
    [[nodiscard]] Optional<u64> const& length() const { return m_length; }

    [[nodiscard]] GC::Ref<Body> clone(JS::Realm&);

    void fully_read(JS::Realm&, ProcessBodyCallback process_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination) const;
    void incrementally_read(ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination);
    void incrementally_read_loop(Streams::ReadableStreamDefaultReader& reader, GC::Ref<JS::Object> task_destination, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error);

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    explicit Body(GC::Ref<Streams::ReadableStream>);
    Body(GC::Ref<Streams::ReadableStream>, SourceType, Optional<u64>);

    // https://fetch.spec.whatwg.org/#concept-body-stream
    // A stream (a ReadableStream object).
    GC::Ref<Streams::ReadableStream> m_stream;

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
    GC::Ref<Body> body;
    Optional<ByteBuffer> type;
};

WebIDL::ExceptionOr<GC::Ref<Body>> byte_sequence_as_body(JS::Realm&, ReadonlyBytes);

}
