/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-params
class FetchParams : public JS::Cell {
    GC_CELL(FetchParams, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchParams);

public:
    struct PreloadedResponseCandidatePendingTag { };
    using PreloadedResponseCandidate = Variant<Empty, PreloadedResponseCandidatePendingTag, GC::Ref<Response>>;

    [[nodiscard]] static GC::Ref<FetchParams> create(JS::VM&, GC::Ref<Request>, GC::Ref<FetchTimingInfo>);

    [[nodiscard]] GC::Ref<Request> request() const { return m_request; }
    [[nodiscard]] GC::Ref<FetchController> controller() const { return m_controller; }
    [[nodiscard]] GC::Ref<FetchTimingInfo> timing_info() const { return m_timing_info; }

    [[nodiscard]] GC::Ref<FetchAlgorithms const> algorithms() const { return m_algorithms; }
    void set_algorithms(GC::Ref<FetchAlgorithms const> algorithms) { m_algorithms = algorithms; }

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
    FetchParams(GC::Ref<Request>, GC::Ref<FetchAlgorithms>, GC::Ref<FetchController>, GC::Ref<FetchTimingInfo>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#fetch-params-request
    // request
    //     A request.
    GC::Ref<Request> m_request;

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
    GC::Ref<FetchAlgorithms const> m_algorithms;

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
    GC::Ref<FetchController> m_controller;

    // https://fetch.spec.whatwg.org/#fetch-params-timing-info
    // timing info
    //     A fetch timing info.
    GC::Ref<FetchTimingInfo> m_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-params-preloaded-response-candidate
    // preloaded response candidate (default null)
    //     Null, "pending", or a response.
    PreloadedResponseCandidate m_preloaded_response_candidate;
};

}
