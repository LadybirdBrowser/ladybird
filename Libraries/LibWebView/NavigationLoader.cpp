/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWebView/Application.h>
#include <LibWebView/NavigationLoader.h>

namespace WebView {

static Web::HTML::NavigationResponseBodyHandle* response_body_handle(Web::HTML::NavigationPopulationResult& result)
{
    if (!result.navigation_params.has<Web::HTML::NavigationParamsDescriptor>())
        return nullptr;
    auto& response = result.navigation_params.get<Web::HTML::NavigationParamsDescriptor>().response;
    return response.body.get_pointer<Web::HTML::NavigationResponseBodyHandle>();
}

NavigationLoader::~NavigationLoader()
{
    release_response_body();
}

void NavigationLoader::did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult result)
{
    VERIFY(!m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    Web::HTML::apply_navigation_population_result(m_request, result);
    m_result = move(result);
}

void NavigationLoader::acquire_response_body(Function<void(bool)> completion_steps)
{
    VERIFY(m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    m_completion_steps = move(completion_steps);

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle) {
        did_acquire(true);
        return;
    }

    auto& request_client = Application::request_server_client(m_is_private);
    auto request = request_client.adopt_request(
        body_handle->request_server_client_id,
        body_handle->request_server_request_id,
        Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    m_response_body_request_server_client_id = body_handle->request_server_client_id;
    m_response_body_request_server_request_id = body_handle->request_server_request_id;

    request->set_body_delivery_paused(true);
    m_response_body_request = request;

    auto weak_this = make_weak_ptr();
    request->set_unbuffered_request_callbacks(
        [weak_this](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(true);
        },
        [](Requests::ResponseData) {},
        [](Core::ImmutableBytes) {},
        [weak_this](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError>) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(false);
        });
}

Web::HTML::NavigationPopulationResult NavigationLoader::take_result()
{
    VERIFY(m_result.has_value());
    if (m_response_body_request)
        m_response_body_was_handed_off = true;
    return m_result.release_value();
}

bool NavigationLoader::response_body_matches(int request_server_client_id, u64 request_server_request_id) const
{
    return m_response_body_request_server_client_id == request_server_client_id
        && m_response_body_request_server_request_id == request_server_request_id;
}

void NavigationLoader::reclaim_response_body_after_failed_handoff()
{
    if (!m_response_body_was_handed_off)
        return;
    m_response_body_was_handed_off = false;
    release_response_body();
}

Web::HTML::NavigationPopulationResult const& NavigationLoader::result() const
{
    VERIFY(m_result.has_value());
    return *m_result;
}

void NavigationLoader::discard(IsPrivate is_private, Web::HTML::NavigationPopulationResult& result)
{
    auto* body_handle = response_body_handle(result);
    if (!body_handle)
        return;

    auto request = Application::request_server_client(is_private).adopt_request(body_handle->request_server_client_id, body_handle->request_server_request_id);
    if (request)
        request->stop();
}

void NavigationLoader::did_acquire(bool succeeded)
{
    if (!m_completion_steps)
        return;

    auto completion_steps = move(m_completion_steps);
    completion_steps(succeeded);
}

void NavigationLoader::release_response_body()
{
    if (!m_response_body_request)
        return;

    // Once the descriptor has been sent, the destination is responsible for adopting the request. Dropping our
    // local reference before that IPC is handled must not release the lease or cancel the request being transferred.
    if (m_response_body_was_handed_off) {
        m_response_body_request = nullptr;
        return;
    }

    m_response_body_request->release_transfer_lease();
    m_response_body_request->stop();
    m_response_body_request = nullptr;
}

}
