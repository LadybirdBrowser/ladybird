/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(FetchAlgorithms);

JS::NonnullGCPtr<FetchAlgorithms> FetchAlgorithms::create(JS::VM& vm, Input input)
{
    auto process_request_body_chunk_length = JS::create_heap_function(vm.heap(), move(input.process_request_body_chunk_length));
    auto process_request_end_of_body = JS::create_heap_function(vm.heap(), move(input.process_request_end_of_body));
    auto process_early_hints_response = JS::create_heap_function(vm.heap(), move(input.process_early_hints_response));
    auto process_response = JS::create_heap_function(vm.heap(), move(input.process_response));
    auto process_response_end_of_body = JS::create_heap_function(vm.heap(), move(input.process_response_end_of_body));
    auto process_response_consume_body = JS::create_heap_function(vm.heap(), move(input.process_response_consume_body));
    return vm.heap().allocate_without_realm<FetchAlgorithms>(
        process_request_body_chunk_length,
        process_request_end_of_body,
        process_early_hints_response,
        process_response,
        process_response_end_of_body,
        process_response_consume_body);
}

FetchAlgorithms::FetchAlgorithms(
    ProcessRequestBodyChunkLengthHeapFunction process_request_body_chunk_length,
    ProcessRequestEndOfBodyHeapFunction process_request_end_of_body,
    ProcessEarlyHintsResponseHeapFunction process_early_hints_response,
    ProcessResponseHeapFunction process_response,
    ProcessResponseEndOfBodyHeapFunction process_response_end_of_body,
    ProcessResponseConsumeBodyHeapFunction process_response_consume_body)
    : m_process_request_body_chunk_length(process_request_body_chunk_length)
    , m_process_request_end_of_body(process_request_end_of_body)
    , m_process_early_hints_response(process_early_hints_response)
    , m_process_response(process_response)
    , m_process_response_end_of_body(process_response_end_of_body)
    , m_process_response_consume_body(process_response_consume_body)
{
}

void FetchAlgorithms::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_process_request_body_chunk_length);
    visitor.visit(m_process_request_end_of_body);
    visitor.visit(m_process_early_hints_response);
    visitor.visit(m_process_response);
    visitor.visit(m_process_response_end_of_body);
    visitor.visit(m_process_response_consume_body);
}

}
