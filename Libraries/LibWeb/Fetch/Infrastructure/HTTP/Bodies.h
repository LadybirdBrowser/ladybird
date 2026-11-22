/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/FetchBody.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Infrastructure {

// https://mimesniff.spec.whatwg.org/#reading-the-resource-header
static constexpr size_t MAX_SNIFF_BYTES = 1445;

// https://fetch.spec.whatwg.org/#concept-body
class WEB_API Body final : public JS::Cell {
    GC_CELL(Body, JS::Cell);
    GC_DECLARE_ALLOCATOR(Body);

public:
    // processBody must be an algorithm accepting a byte sequence.
    using ProcessBodyCallback = GC::Ref<GC::Function<void(ByteBuffer)>>;
    // processBodyError must be an algorithm optionally accepting an exception.
    using ProcessBodyErrorCallback = GC::Ref<GC::Function<void(JS::Value)>>;
    // processBodyChunk must be an algorithm accepting a byte sequence.
    using ProcessBodyChunkCallback = GC::Ref<GC::Function<void(ByteBuffer)>>;
    // processEndOfBody must be an algorithm accepting no arguments
    using ProcessEndOfBodyCallback = GC::Ref<GC::Function<void()>>;

    // https://mimesniff.spec.whatwg.org/#reading-the-resource-header
    // The resource header is the first 1445 bytes, or all of a shorter body; a body that errored first is Failed.
    enum class SniffOutcome : u8 {
        Complete,
        Failed,
    };
    struct SniffBytes {
        ReadonlyBytes bytes;
        SniffOutcome outcome { SniffOutcome::Complete };
    };
    using SniffBytesCallback = GC::Ref<GC::Function<void(SniffBytes)>>;
    using ResourceHeaderCallback = GC::Ref<GC::Function<void(Core::ImmutableBytes, SniffOutcome)>>;
    using ResourceHeaderReader = GC::Ref<GC::Function<void(ResourceHeaderCallback)>>;

    // A body that extraction made keeps its source and length, and is never sniffed.
    [[nodiscard]] static GC::Ref<Body> create(GC::Ref<Streams::ReadableStream>, FetchBody = {});
    [[nodiscard]] static GC::Ref<Body> create_over_bytes(JS::Realm&, Core::ImmutableBytes);
    // The reader reads the resource header from where the body's bytes arrive.
    [[nodiscard]] static GC::Ref<Body> create_over_arriving_bytes(GC::Ref<Streams::ReadableStream>, ResourceHeaderReader);

    [[nodiscard]] GC::Ref<Streams::ReadableStream> stream() const { return *m_stream; }
    void set_stream(GC::Ref<Streams::ReadableStream> value) { m_stream = value; }
    [[nodiscard]] FetchBody const& fetch_body() const { return m_fetch_body; }
    [[nodiscard]] Optional<Core::ImmutableBytes> const& source() const { return m_fetch_body.source(); }
    [[nodiscard]] Optional<u64> const& length() const { return m_fetch_body.length(); }

    [[nodiscard]] Optional<SniffBytes> sniff_bytes_if_available() const;
    // A waiter runs at once if the resource header is available, and otherwise once, from a networking task.
    void wait_for_sniff_bytes(SniffBytesCallback on_ready);

    [[nodiscard]] GC::Ref<Body> clone(JS::Realm&);

    void fully_read(JS::Realm&, ProcessBodyCallback process_body, ProcessBodyErrorCallback process_body_error, TaskDestination) const;
    GC::Ref<Streams::ReadableStreamDefaultReader> incrementally_read(JS::Realm&, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination);
    GC::Ref<Streams::ReadableStreamDefaultReader> incrementally_read(ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination);
    void incrementally_read_loop(Streams::ReadableStreamDefaultReader& reader, TaskDestination, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error);

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    struct NeverSniffed { };
    struct UnreadResourceHeader {
        ResourceHeaderReader reader;
    };
    struct ReadingResourceHeader { };
    struct ReadResourceHeader {
        Core::ImmutableBytes bytes;
        SniffOutcome outcome { SniffOutcome::Complete };
    };
    using ResourceHeader = Variant<NeverSniffed, UnreadResourceHeader, ReadingResourceHeader, ReadResourceHeader>;

    Body(GC::Ref<Streams::ReadableStream>, FetchBody, ResourceHeader);

    void did_read_resource_header(Core::ImmutableBytes, SniffOutcome);

    // https://fetch.spec.whatwg.org/#concept-body-stream
    // A stream (a ReadableStream object).
    GC::Ref<Streams::ReadableStream> m_stream;

    FetchBody m_fetch_body;

    ResourceHeader m_resource_header;
    GC::Ptr<GC::Function<void(SniffBytes)>> m_sniff_bytes_callback;
};

// https://fetch.spec.whatwg.org/#body-with-type
// A body with type is a tuple that consists of a body (a body) and a type (a header value or null).
struct BodyWithType {
    GC::Ref<Body> body;
    Optional<Utf16String> type;
};

WEB_API GC::Ref<Body> byte_sequence_as_body(JS::Realm&, ReadonlyBytes);
WEB_API void cancel_incremental_read(Streams::ReadableStreamDefaultReader&);

}
