/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-params
class FetchParams : public JS::Cell {
    JS_CELL(FetchParams, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchParams);

public:
    struct PreloadedResponseCandidatePendingTag { };
    using PreloadedResponseCandidate = Variant<Empty, PreloadedResponseCandidatePendingTag, JS::NonnullGCPtr<Response>>;

    [[nodiscard]] static JS::NonnullGCPtr<FetchParams> create(JS::VM&, JS::NonnullGCPtr<Request>, JS::NonnullGCPtr<FetchTimingInfo>);

    [[nodiscard]] JS::NonnullGCPtr<Request> request() const { return m_request; }
    [[nodiscard]] JS::NonnullGCPtr<FetchController> controller() const { return m_controller; }
    [[nodiscard]] JS::NonnullGCPtr<FetchTimingInfo> timing_info() const { return m_timing_info; }

    [[nodiscard]] JS::NonnullGCPtr<FetchAlgorithms const> algorithms() const { return m_algorithms; }
    void set_algorithms(JS::NonnullGCPtr<FetchAlgorithms const> algorithms) { m_algorithms = algorithms; }

    [[nodiscard]] TaskDestination& task_destination() { return m_task_destination; }
    [[nodiscard]] TaskDestination const& task_destination() const { return m_task_destination; }
    void set_task_destination(TaskDestination task_destination) { m_task_destination = move(task_destination); }

    [[nodiscard]] HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const { return m_cross_origin_isolated_capability; }
    void set_cross_origin_isolated_capability(HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability) { m_cross_origin_isolated_capability = cross_origin_isolated_capability; }

    [[nodiscard]] PreloadedResponseCandidate& preloaded_response_candidate() { return m_preloaded_response_candidate; }
    [[nodiscard]] PreloadedResponseCandidate const& preloaded_response_candidate() const { return m_preloaded_response_candidate; }
    void set_preloaded_response_candidate(PreloadedResponseCandidate preloaded_response_candidate) { m_preloaded_response_candidate = move(preloaded_response_candidate); }

    [[nodiscard]] bool is_aborted() const;
    [[nodiscard]] bool is_canceled() const;

private:
    FetchParams(JS::NonnullGCPtr<Request>, JS::NonnullGCPtr<FetchAlgorithms>, JS::NonnullGCPtr<FetchController>, JS::NonnullGCPtr<FetchTimingInfo>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#fetch-params-request
    // request
    //     A request.
    JS::NonnullGCPtr<Request> m_request;

    // https://fetch.spec.whatwg.org/#fetch-params-process-request-body
    // process request body chunk length (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-request-end-of-body
    // process request end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-early-hints-response
    // process early hints response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response
    // process response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-end-of-body
    // process response end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-consume-body
    // process response consume body (default null)
    //     Null or an algorithm.
    JS::NonnullGCPtr<FetchAlgorithms const> m_algorithms;

    // https://fetch.spec.whatwg.org/#fetch-params-task-destination
    // task destination (default null)
    //     Null, a global object, or a parallel queue.
    TaskDestination m_task_destination;

    // https://fetch.spec.whatwg.org/#fetch-params-cross-origin-isolated-capability
    // cross-origin isolated capability (default false)
    //     A boolean.
    HTML::CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };

    // https://fetch.spec.whatwg.org/#fetch-params-controller
    // controller (default a new fetch controller)
    //     A fetch controller.
    JS::NonnullGCPtr<FetchController> m_controller;

    // https://fetch.spec.whatwg.org/#fetch-params-timing-info
    // timing info
    //     A fetch timing info.
    JS::NonnullGCPtr<FetchTimingInfo> m_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-params-preloaded-response-candidate
    // preloaded response candidate (default null)
    //     Null, "pending", or a response.
    PreloadedResponseCandidate m_preloaded_response_candidate;
};

}
