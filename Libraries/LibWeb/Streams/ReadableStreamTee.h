/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

namespace Web::Streams::Detail {

struct ReadableStreamTeeParams final : public JS::Cell {
    GC_CELL(ReadableStreamTeeParams, JS::Cell);
    GC_DECLARE_ALLOCATOR(ReadableStreamTeeParams);

    virtual void visit_edges(Visitor& visitor) override;

    bool reading { false };
    bool read_again { false };
    bool canceled1 { false };
    bool canceled2 { false };
    JS::Value reason1 { JS::js_undefined() };
    JS::Value reason2 { JS::js_undefined() };
    GC::Ptr<ReadableStream> branch1;
    GC::Ptr<ReadableStream> branch2;
    GC::Ptr<PullAlgorithm> pull_algorithm;
};

// https://streams.spec.whatwg.org/#ref-for-read-request③
class ReadableStreamTeeReadRequest final : public ReadRequest {
    GC_CELL(ReadableStreamTeeReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(ReadableStreamTeeReadRequest);

private:
    ReadableStreamTeeReadRequest(
        JS::Realm& realm,
        GC::Ref<ReadableStream> stream,
        GC::Ref<ReadableStreamTeeParams> params,
        GC::Ref<WebIDL::Promise> cancel_promise,
        bool clone_for_branch2);

    virtual void visit_edges(Visitor& visitor) override;

    virtual void on_chunk(JS::Value chunk) override;
    virtual void on_close() override;
    virtual void on_error(JS::Value) override;

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<ReadableStream> m_stream;
    GC::Ref<ReadableStreamTeeParams> m_params;
    GC::Ref<WebIDL::Promise> m_cancel_promise;
    bool m_clone_for_branch2 { false };
};

struct ReadableByteStreamTeeParams final : public JS::Cell {
    GC_CELL(ReadableByteStreamTeeParams, JS::Cell);
    GC_DECLARE_ALLOCATOR(ReadableByteStreamTeeParams);

    explicit ReadableByteStreamTeeParams(ReadableStreamReader reader);

    virtual void visit_edges(Visitor& visitor) override;

    bool reading { false };
    bool read_again_for_branch1 { false };
    bool read_again_for_branch2 { false };
    bool canceled1 { false };
    bool canceled2 { false };
    JS::Value reason1 { JS::js_undefined() };
    JS::Value reason2 { JS::js_undefined() };
    GC::Ptr<ReadableStream> branch1;
    GC::Ptr<ReadableStream> branch2;
    GC::Ptr<PullAlgorithm> pull1_algorithm;
    GC::Ptr<PullAlgorithm> pull2_algorithm;
    ReadableStreamReader reader;
};

// https://streams.spec.whatwg.org/#ref-for-read-request④
class ReadableByteStreamTeeDefaultReadRequest final : public ReadRequest {
    GC_CELL(ReadableByteStreamTeeDefaultReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(ReadableByteStreamTeeDefaultReadRequest);

private:
    ReadableByteStreamTeeDefaultReadRequest(
        JS::Realm& realm,
        GC::Ref<ReadableStream> stream,
        GC::Ref<ReadableByteStreamTeeParams> params,
        GC::Ref<WebIDL::Promise> cancel_promise);

    virtual void visit_edges(Visitor& visitor) override;

    virtual void on_chunk(JS::Value chunk) override;
    virtual void on_close() override;
    virtual void on_error(JS::Value) override;

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<ReadableStream> m_stream;
    GC::Ref<ReadableByteStreamTeeParams> m_params;
    GC::Ref<WebIDL::Promise> m_cancel_promise;
};

// https://streams.spec.whatwg.org/#ref-for-read-into-request②
class ReadableByteStreamTeeBYOBReadRequest final : public ReadIntoRequest {
    GC_CELL(ReadableByteStreamTeeBYOBReadRequest, ReadIntoRequest);
    GC_DECLARE_ALLOCATOR(ReadableByteStreamTeeBYOBReadRequest);

private:
    ReadableByteStreamTeeBYOBReadRequest(
        JS::Realm& realm,
        GC::Ref<ReadableStream> stream,
        GC::Ref<ReadableByteStreamTeeParams> params,
        GC::Ref<WebIDL::Promise> cancel_promise,
        GC::Ref<ReadableStream> byob_branch,
        GC::Ref<ReadableStream> other_branch,
        bool for_branch2);

    virtual void visit_edges(Visitor& visitor) override;

    virtual void on_chunk(JS::Value chunk) override;
    virtual void on_close(JS::Value chunk) override;
    virtual void on_error(JS::Value) override;

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<ReadableStream> m_stream;
    GC::Ref<ReadableByteStreamTeeParams> m_params;
    GC::Ref<WebIDL::Promise> m_cancel_promise;
    GC::Ref<ReadableStream> m_byob_branch;
    GC::Ref<ReadableStream> m_other_branch;
    bool m_for_branch2 { false };
};

}
