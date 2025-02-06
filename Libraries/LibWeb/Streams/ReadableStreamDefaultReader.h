/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>

namespace Web::Streams {

struct ReadableStreamReadResult {
    JS::Value value;
    bool done;
};

class ReadRequest : public JS::Cell {
    GC_CELL(ReadRequest, JS::Cell);

public:
    virtual ~ReadRequest() = default;

    virtual void on_chunk(JS::Value chunk) = 0;
    virtual void on_close() = 0;
    virtual void on_error(JS::Value error) = 0;
};

class ReadLoopReadRequest final : public ReadRequest {
    GC_CELL(ReadLoopReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(ReadLoopReadRequest);

public:
    // successSteps, which is an algorithm accepting a byte sequence
    using SuccessSteps = GC::Function<void(ByteBuffer)>;

    // failureSteps, which is an algorithm accepting a JavaScript value
    using FailureSteps = GC::Function<void(JS::Value error)>;

    // AD-HOC: callback triggered on every chunk received from the stream.
    using ChunkSteps = GC::Function<void(ByteBuffer)>;

    ReadLoopReadRequest(JS::VM& vm, JS::Realm& realm, ReadableStreamDefaultReader& reader, GC::Ref<SuccessSteps> success_steps, GC::Ref<FailureSteps> failure_steps, GC::Ptr<ChunkSteps> chunk_steps = {});

    virtual void on_chunk(JS::Value chunk) override;

    virtual void on_close() override;

    virtual void on_error(JS::Value error) override;

private:
    virtual void visit_edges(Visitor&) override;

    JS::VM& m_vm;
    GC::Ref<JS::Realm> m_realm;
    GC::Ref<ReadableStreamDefaultReader> m_reader;
    ByteBuffer m_bytes;
    GC::Ref<SuccessSteps> m_success_steps;
    GC::Ref<FailureSteps> m_failure_steps;
    GC::Ptr<ChunkSteps> m_chunk_steps;
};

// https://streams.spec.whatwg.org/#readablestreamdefaultreader
class ReadableStreamDefaultReader final
    : public Bindings::PlatformObject
    , public ReadableStreamGenericReaderMixin {
    WEB_PLATFORM_OBJECT(ReadableStreamDefaultReader, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ReadableStreamDefaultReader);

public:
    static WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> construct_impl(JS::Realm&, GC::Ref<ReadableStream>);

    // AD-HOC: Callback functions for read_all_chunks
    // successSteps, which is an algorithm accepting a JavaScript value
    using ReadAllOnSuccessSteps = GC::Function<void()>;

    // failureSteps, which is an algorithm accepting a JavaScript value
    using ReadAllOnFailureSteps = GC::Function<void(JS::Value error)>;

    // AD-HOC: callback triggered on every chunk received from the stream.
    using ReadAllOnChunkSteps = GC::Function<void(JS::Value chunk)>;

    virtual ~ReadableStreamDefaultReader() override = default;

    GC::Ref<WebIDL::Promise> read();

    void read_a_chunk(Fetch::Infrastructure::IncrementalReadLoopReadRequest& read_request);
    void read_all_bytes(GC::Ref<ReadLoopReadRequest::SuccessSteps>, GC::Ref<ReadLoopReadRequest::FailureSteps>);
    void read_all_chunks(GC::Ref<ReadAllOnChunkSteps>, GC::Ref<ReadAllOnSuccessSteps>, GC::Ref<ReadAllOnFailureSteps>);
    GC::Ref<WebIDL::Promise> read_all_bytes_deprecated();

    void release_lock();

    SinglyLinkedList<GC::Ref<ReadRequest>>& read_requests() { return m_read_requests; }

private:
    explicit ReadableStreamDefaultReader(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    SinglyLinkedList<GC::Ref<ReadRequest>> m_read_requests;
};

}
