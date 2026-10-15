/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Infrastructure {

FetchParams::FetchParams(NonnullRefPtr<Request> request, GC::Ref<FetchAlgorithms const> algorithms, FetchController& controller, NonnullRefPtr<FetchTimingInfo> timing_info, NonnullRefPtr<LoaderConfigSnapshot const> loader_config)
    : m_request(move(request))
    , m_controller_token(controller.token())
    , m_controller_registry(controller.registry())
    , m_control_block(controller.control_block())
    , m_timing_info(move(timing_info))
    , m_loader_config(move(loader_config))
{
    controller.set_algorithms({}, algorithms);
    controller.set_original_fetch_params({}, *this);
}

FetchParams::FetchParams(FetchParams const& params)
    : RefCounted()
    , m_request(params.m_request)
    , m_cross_origin_isolated_capability(params.m_cross_origin_isolated_capability)
    , m_controller_token(params.m_controller_token)
    , m_controller_registry(params.m_controller_registry)
    , m_control_block(params.m_control_block)
    , m_timing_info(params.m_timing_info)
    , m_preloaded_response_candidate(params.m_preloaded_response_candidate)
    , m_has_response_body_transfer_lease(params.m_has_response_body_transfer_lease)
    , m_blob_url_entry(params.m_blob_url_entry)
    , m_loader_config(params.m_loader_config)
{
}

NonnullRefPtr<FetchParams> FetchParams::create(FetchController& controller, NonnullRefPtr<Request> request, GC::Ref<FetchAlgorithms const> algorithms, NonnullRefPtr<FetchTimingInfo> timing_info, NonnullRefPtr<LoaderConfigSnapshot const> loader_config)
{
    return adopt_ref(*new FetchParams(move(request), algorithms, controller, move(timing_info), move(loader_config)));
}

NonnullRefPtr<FetchParams> FetchParams::copy(FetchParams const& params)
{
    return adopt_ref(*new FetchParams(params));
}

GC::Ref<FetchController> FetchParams::controller() const
{
    VERIFY(m_controller_registry);
    auto controller = m_controller_registry->find(m_controller_token);
    VERIFY(controller);
    return *controller;
}

// https://fetch.spec.whatwg.org/#fetch-params-aborted
bool FetchParams::is_aborted() const
{
    // A fetch params fetchParams is aborted if its controller’s state is "aborted".
    return m_control_block->state() == FetchControlBlock::State::Aborted;
}

// https://fetch.spec.whatwg.org/#fetch-params-canceled
bool FetchParams::is_canceled() const
{
    // A fetch params fetchParams is canceled if its controller’s state is "aborted" or "terminated".
    auto state = m_control_block->state();
    return state == FetchControlBlock::State::Aborted || state == FetchControlBlock::State::Terminated;
}

}
