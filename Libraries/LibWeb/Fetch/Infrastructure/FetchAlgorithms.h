/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-elsewhere-fetch
class FetchAlgorithms : public JS::Cell {
    GC_CELL(FetchAlgorithms, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchAlgorithms);

public:
    struct ConsumeBodyFailureTag { };
    using BodyBytes = Variant<Empty, ConsumeBodyFailureTag, ByteBuffer>;

    using ProcessRequestBodyChunkLengthFunction = Function<void(u64)>;
    using ProcessRequestEndOfBodyFunction = Function<void()>;
    using ProcessEarlyHintsResponseFunction = Function<void(GC::Ref<Infrastructure::Response>)>;
    using ProcessResponseFunction = Function<void(GC::Ref<Infrastructure::Response>)>;
    using ProcessResponseEndOfBodyFunction = Function<void(GC::Ref<Infrastructure::Response>)>;
    using ProcessResponseConsumeBodyFunction = Function<void(GC::Ref<Infrastructure::Response>, BodyBytes)>;

    using ProcessRequestBodyChunkLengthHeapFunction = GC::Ref<GC::Function<ProcessRequestBodyChunkLengthFunction::FunctionType>>;
    using ProcessRequestEndOfBodyHeapFunction = GC::Ref<GC::Function<ProcessRequestEndOfBodyFunction::FunctionType>>;
    using ProcessEarlyHintsResponseHeapFunction = GC::Ref<GC::Function<ProcessEarlyHintsResponseFunction::FunctionType>>;
    using ProcessResponseHeapFunction = GC::Ref<GC::Function<ProcessResponseFunction::FunctionType>>;
    using ProcessResponseEndOfBodyHeapFunction = GC::Ref<GC::Function<ProcessResponseEndOfBodyFunction::FunctionType>>;
    using ProcessResponseConsumeBodyHeapFunction = GC::Ref<GC::Function<ProcessResponseConsumeBodyFunction::FunctionType>>;

    struct Input {
        ProcessRequestBodyChunkLengthFunction process_request_body_chunk_length;
        ProcessRequestEndOfBodyFunction process_request_end_of_body;
        ProcessEarlyHintsResponseFunction process_early_hints_response;
        ProcessResponseFunction process_response;
        ProcessResponseEndOfBodyFunction process_response_end_of_body;
        ProcessResponseConsumeBodyFunction process_response_consume_body;
    };

    [[nodiscard]] static GC::Ref<FetchAlgorithms> create(JS::VM&, Input);

    ProcessRequestBodyChunkLengthFunction const& process_request_body_chunk_length() const { return m_process_request_body_chunk_length->function(); }
    ProcessRequestEndOfBodyFunction const& process_request_end_of_body() const { return m_process_request_end_of_body->function(); }
    ProcessEarlyHintsResponseFunction const& process_early_hints_response() const { return m_process_early_hints_response->function(); }
    ProcessResponseFunction const& process_response() const { return m_process_response->function(); }
    ProcessResponseEndOfBodyFunction const& process_response_end_of_body() const { return m_process_response_end_of_body->function(); }
    ProcessResponseConsumeBodyFunction const& process_response_consume_body() const { return m_process_response_consume_body->function(); }

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    explicit FetchAlgorithms(
        ProcessRequestBodyChunkLengthHeapFunction process_request_body_chunk_length,
        ProcessRequestEndOfBodyHeapFunction process_request_end_of_body,
        ProcessEarlyHintsResponseHeapFunction process_early_hints_response,
        ProcessResponseHeapFunction process_response,
        ProcessResponseEndOfBodyHeapFunction process_response_end_of_body,
        ProcessResponseConsumeBodyHeapFunction process_response_consume_body);

    ProcessRequestBodyChunkLengthHeapFunction m_process_request_body_chunk_length;
    ProcessRequestEndOfBodyHeapFunction m_process_request_end_of_body;
    ProcessEarlyHintsResponseHeapFunction m_process_early_hints_response;
    ProcessResponseHeapFunction m_process_response;
    ProcessResponseEndOfBodyHeapFunction m_process_response_end_of_body;
    ProcessResponseConsumeBodyHeapFunction m_process_response_consume_body;
};

}
