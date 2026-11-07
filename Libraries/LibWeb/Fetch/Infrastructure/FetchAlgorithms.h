/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/BodyIntent.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-elsewhere-fetch
class WEB_API FetchAlgorithms : public JS::Cell {
    GC_CELL(FetchAlgorithms, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchAlgorithms);

public:
    struct ConsumeBodyFailureTag { };
    using BodyBytes = Variant<Empty, ConsumeBodyFailureTag, Core::ImmutableBytes>;

    using ProcessRequestBodyChunkLengthFunction = Function<void(u64)>;
    using ProcessRequestEndOfBodyFunction = Function<void()>;
    using ProcessEarlyHintsResponseFunction = Function<void(GC::Ref<Infrastructure::ResponseView>)>;
    using ProcessResponseFunction = Function<void(GC::Ref<Infrastructure::ResponseView>)>;
    using ProcessResponseEndOfBodyFunction = Function<void(GC::Ref<Infrastructure::ResponseView>)>;
    using ProcessResponseConsumeBodyFunction = Function<void(GC::Ref<Infrastructure::ResponseView>, BodyBytes)>;

    using ProcessRequestBodyChunkLengthHeapFunction = GC::Ptr<GC::Function<ProcessRequestBodyChunkLengthFunction::FunctionType>>;
    using ProcessRequestEndOfBodyHeapFunction = GC::Ptr<GC::Function<ProcessRequestEndOfBodyFunction::FunctionType>>;
    using ProcessEarlyHintsResponseHeapFunction = GC::Ptr<GC::Function<ProcessEarlyHintsResponseFunction::FunctionType>>;
    using ProcessResponseHeapFunction = GC::Ptr<GC::Function<ProcessResponseFunction::FunctionType>>;
    using ProcessResponseEndOfBodyHeapFunction = GC::Ptr<GC::Function<ProcessResponseEndOfBodyFunction::FunctionType>>;
    using ProcessResponseConsumeBodyHeapFunction = GC::Ptr<GC::Function<ProcessResponseConsumeBodyFunction::FunctionType>>;

    // A request body whose stream is the only place its bytes are, with the reader fetch() acquired for that stream.
    struct RequestBodyReader {
        GC::Ref<Body> body;
        GC::Ref<Streams::ReadableStreamDefaultReader> reader;
    };

    struct Input {
        explicit Input(Engine::BodyIntent body_intent)
            : body_intent(body_intent)
        {
        }

        Engine::BodyIntent body_intent;
        ProcessRequestBodyChunkLengthHeapFunction process_request_body_chunk_length;
        ProcessRequestEndOfBodyHeapFunction process_request_end_of_body;
        ProcessEarlyHintsResponseHeapFunction process_early_hints_response;
        ProcessResponseHeapFunction process_response;
        ProcessResponseEndOfBodyHeapFunction process_response_end_of_body;
        ProcessResponseConsumeBodyHeapFunction process_response_consume_body;
        Optional<RequestBodyReader> request_body_reader;
    };

    [[nodiscard]] static GC::Ref<FetchAlgorithms> create(Input);

    Engine::BodyIntent body_intent() const { return m_body_intent; }
    Optional<RequestBodyReader> const& request_body_reader() const { return m_request_body_reader; }
    ProcessRequestBodyChunkLengthHeapFunction process_request_body_chunk_length() const { return m_process_request_body_chunk_length; }
    ProcessRequestEndOfBodyHeapFunction process_request_end_of_body() const { return m_process_request_end_of_body; }
    ProcessEarlyHintsResponseHeapFunction process_early_hints_response() const { return m_process_early_hints_response; }
    ProcessResponseHeapFunction process_response() const { return m_process_response; }
    ProcessResponseEndOfBodyHeapFunction process_response_end_of_body() const { return m_process_response_end_of_body; }
    ProcessResponseConsumeBodyHeapFunction process_response_consume_body() const { return m_process_response_consume_body; }

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    FetchAlgorithms(
        Engine::BodyIntent,
        ProcessRequestBodyChunkLengthHeapFunction process_request_body_chunk_length,
        ProcessRequestEndOfBodyHeapFunction process_request_end_of_body,
        ProcessEarlyHintsResponseHeapFunction process_early_hints_response,
        ProcessResponseHeapFunction process_response,
        ProcessResponseEndOfBodyHeapFunction process_response_end_of_body,
        ProcessResponseConsumeBodyHeapFunction process_response_consume_body,
        Optional<RequestBodyReader> request_body_reader);

    Engine::BodyIntent m_body_intent;
    ProcessRequestBodyChunkLengthHeapFunction m_process_request_body_chunk_length;
    ProcessRequestEndOfBodyHeapFunction m_process_request_end_of_body;
    ProcessEarlyHintsResponseHeapFunction m_process_early_hints_response;
    ProcessResponseHeapFunction m_process_response;
    ProcessResponseEndOfBodyHeapFunction m_process_response_end_of_body;
    ProcessResponseConsumeBodyHeapFunction m_process_response_consume_body;
    Optional<RequestBodyReader> m_request_body_reader;
};

}
