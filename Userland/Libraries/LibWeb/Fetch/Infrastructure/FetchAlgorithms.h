/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-elsewhere-fetch
class FetchAlgorithms : public JS::Cell {
    JS_CELL(FetchAlgorithms, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchAlgorithms);

public:
    struct ConsumeBodyFailureTag { };
    using BodyBytes = Variant<Empty, ConsumeBodyFailureTag, ByteBuffer>;

    using ProcessRequestBodyChunkLengthFunction = Function<void(u64)>;
    using ProcessRequestEndOfBodyFunction = Function<void()>;
    using ProcessEarlyHintsResponseFunction = Function<void(JS::NonnullGCPtr<Infrastructure::Response>)>;
    using ProcessResponseFunction = Function<void(JS::NonnullGCPtr<Infrastructure::Response>)>;
    using ProcessResponseEndOfBodyFunction = Function<void(JS::NonnullGCPtr<Infrastructure::Response>)>;
    using ProcessResponseConsumeBodyFunction = Function<void(JS::NonnullGCPtr<Infrastructure::Response>, BodyBytes)>;

    using ProcessRequestBodyChunkLengthHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessRequestBodyChunkLengthFunction::FunctionType>>;
    using ProcessRequestEndOfBodyHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessRequestEndOfBodyFunction::FunctionType>>;
    using ProcessEarlyHintsResponseHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessEarlyHintsResponseFunction::FunctionType>>;
    using ProcessResponseHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessResponseFunction::FunctionType>>;
    using ProcessResponseEndOfBodyHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessResponseEndOfBodyFunction::FunctionType>>;
    using ProcessResponseConsumeBodyHeapFunction = JS::NonnullGCPtr<JS::HeapFunction<ProcessResponseConsumeBodyFunction::FunctionType>>;

    struct Input {
        ProcessRequestBodyChunkLengthFunction process_request_body_chunk_length;
        ProcessRequestEndOfBodyFunction process_request_end_of_body;
        ProcessEarlyHintsResponseFunction process_early_hints_response;
        ProcessResponseFunction process_response;
        ProcessResponseEndOfBodyFunction process_response_end_of_body;
        ProcessResponseConsumeBodyFunction process_response_consume_body;
    };

    [[nodiscard]] static JS::NonnullGCPtr<FetchAlgorithms> create(JS::VM&, Input);

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
