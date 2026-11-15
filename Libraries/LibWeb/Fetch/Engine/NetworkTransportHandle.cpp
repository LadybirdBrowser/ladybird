/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>

namespace Web::Fetch::Engine {

NonnullRefPtr<NetworkTransportHandle> NetworkTransportHandle::create(NonnullRefPtr<Requests::Request> request)
{
    return adopt_ref(*new NetworkTransportHandle(move(request)));
}

NetworkTransportHandle::NetworkTransportHandle(NonnullRefPtr<Requests::Request> request)
    : m_request(move(request))
{
}

NetworkTransportHandle::~NetworkTransportHandle()
{
    ASSERT(is_on_owner_thread());
}

bool NetworkTransportHandle::has_file_backed_response_body() const
{
    ASSERT(is_on_owner_thread());
    return m_request->has_file_backed_response_body();
}

void NetworkTransportHandle::resume_body_delivery_up_to(u64 byte_count)
{
    ASSERT(is_on_owner_thread());
    m_request->resume_body_delivery_up_to(byte_count);
}

void NetworkTransportHandle::pause_body_delivery()
{
    ASSERT(is_on_owner_thread());
    m_request->set_body_delivery_paused(true);
}

void NetworkTransportHandle::stop()
{
    ASSERT(is_on_owner_thread());
    m_request->stop();
}

void NetworkTransportHandle::release_transfer_lease()
{
    ASSERT(is_on_owner_thread());
    m_request->release_transfer_lease();
}

void NetworkTransportHandle::post_transport_control(FetchExecutor& executor, auto operation)
{
    Command::TransportControl control {
        .request_id = request_id(),
        .transport_epoch = transport_epoch(),
        .operation = move(operation),
    };
    executor.post(move(control));
}

void NetworkTransportHandle::post_stop(FetchExecutor& executor)
{
    NonnullRefPtr protector = *this;
    ASSERT(is_on_owner_thread());
    m_stop_requested = true;
    if (auto on_stop_requested = exchange(m_on_stop_requested, nullptr))
        on_stop_requested();
    post_transport_control(executor, Command::TransportControl::StopTransport {});
}

void NetworkTransportHandle::post_release_navigation_hold(FetchExecutor& executor)
{
    ASSERT(is_on_owner_thread());
    post_transport_control(executor, Command::TransportControl::ReleaseNavigationHold {});
}

void NetworkTransportHandle::post_release_navigation_hold_up_to(FetchExecutor& executor, u64 byte_count)
{
    ASSERT(is_on_owner_thread());
    post_transport_control(executor, Command::TransportControl::ReleaseNavigationHoldUpTo { .byte_count = byte_count });
}

void NetworkTransportHandle::set_on_stop_requested(Function<void()> on_stop_requested)
{
    ASSERT(is_on_owner_thread());
    m_on_stop_requested = move(on_stop_requested);
}

Function<void()> NetworkTransportHandle::take_on_stop_requested()
{
    ASSERT(is_on_owner_thread());
    return exchange(m_on_stop_requested, nullptr);
}

void NetworkTransportHandle::set_on_requires_network(Function<void()> on_requires_network)
{
    ASSERT(is_on_owner_thread());
    m_request->on_requires_network = move(on_requires_network);
}

void NetworkTransportHandle::set_on_upload_progress(Function<void(u64 bytes_sent)> on_upload_progress)
{
    ASSERT(is_on_owner_thread());
    m_request->on_upload_progress = move(on_upload_progress);
}

void NetworkTransportHandle::set_on_teardown(Function<void(Requests::Request::TeardownReason)> on_teardown)
{
    ASSERT(is_on_owner_thread());
    m_request->on_teardown = move(on_teardown);
}

}
