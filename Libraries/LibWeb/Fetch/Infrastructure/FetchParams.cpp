/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchParams);

FetchParams::FetchParams(NonnullRefPtr<Request> request, GC::Ref<FetchAlgorithms const> algorithms, GC::Ref<FetchController> controller, NonnullRefPtr<FetchTimingInfo> timing_info, NonnullRefPtr<LoaderConfigSnapshot const> loader_config)
    : m_request(move(request))
    , m_body_intent(algorithms->body_intent())
    , m_controller(controller)
    , m_timing_info(move(timing_info))
    , m_loader_config(move(loader_config))
{
    m_controller->set_fetch_params({}, *this);
    m_controller->set_algorithms({}, algorithms);
}

FetchParams::FetchParams(FetchParams const& params)
    : m_request(params.m_request)
    , m_body_intent(params.m_body_intent)
    , m_cross_origin_isolated_capability(params.m_cross_origin_isolated_capability)
    , m_controller(params.m_controller)
    , m_timing_info(params.m_timing_info)
    , m_preloaded_response_candidate(params.m_preloaded_response_candidate)
    , m_has_response_body_transfer_lease(params.m_has_response_body_transfer_lease)
    , m_blob_url_entry(params.m_blob_url_entry)
    , m_loader_config(params.m_loader_config)
{
}

GC::Ref<FetchParams> FetchParams::create(FetchController& controller, NonnullRefPtr<Request> request, GC::Ref<FetchAlgorithms const> algorithms, NonnullRefPtr<FetchTimingInfo> timing_info, NonnullRefPtr<LoaderConfigSnapshot const> loader_config)
{
    return GC::Heap::the().allocate<FetchParams>(move(request), algorithms, controller, move(timing_info), move(loader_config));
}

GC::Ref<FetchParams> FetchParams::copy(FetchParams const& params)
{
    return GC::Heap::the().allocate<FetchParams>(params);
}

void FetchParams::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_controller);
}

// https://fetch.spec.whatwg.org/#fetch-params-aborted
bool FetchParams::is_aborted() const
{
    // A fetch params fetchParams is aborted if its controller’s state is "aborted".
    return m_controller->state() == FetchController::State::Aborted;
}

// https://fetch.spec.whatwg.org/#fetch-params-canceled
bool FetchParams::is_canceled() const
{
    // A fetch params fetchParams is canceled if its controller’s state is "aborted" or "terminated".
    return m_controller->state() == FetchController::State::Aborted || m_controller->state() == FetchController::State::Terminated;
}

}
