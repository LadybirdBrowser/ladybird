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
    controller.set_network_observation({}, m_request->ensure_network_observation());
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
    , m_engine_record(params.m_engine_record)
    , m_is_cors_preflight(params.m_is_cors_preflight)
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

Engine::StartFetchInput FetchParams::start_fetch_input(Engine::FetchId fetch_id, NonnullOwnPtr<Engine::EngineEventSink> event_sink) const
{
    return Engine::StartFetchInput { {}, fetch_id, m_control_block->body_intent(), m_blob_url_entry, move(event_sink) };
}

void FetchParams::set_body_transport(Engine::FetchBodyDeliveryGate::Transport& transport) const
{
    VERIFY(m_engine_record);
    if (m_is_cors_preflight)
        m_engine_record->set_cors_preflight_body_transport(transport);
    else
        m_engine_record->set_body_transport(transport);
}

void FetchParams::stop_body_transport() const
{
    if (!m_engine_record)
        return;
    if (m_is_cors_preflight)
        m_engine_record->stop_cors_preflight_body_transport();
    else
        m_engine_record->stop_body_transport();
}

void FetchParams::discard_response_body(Response const& response) const
{
    response.release_request_transfer_lease();
    stop_body_transport();
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
    return m_control_block->is_aborted();
}

// https://fetch.spec.whatwg.org/#fetch-params-canceled
bool FetchParams::is_canceled() const
{
    return m_control_block->is_canceled();
}

}
