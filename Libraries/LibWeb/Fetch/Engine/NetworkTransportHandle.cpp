/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

bool NetworkTransportHandle::has_file_backed_response_body() const
{
    return m_request->has_file_backed_response_body();
}

void NetworkTransportHandle::resume_body_delivery_up_to(u64 byte_count)
{
    m_request->resume_body_delivery_up_to(byte_count);
}

void NetworkTransportHandle::pause_body_delivery()
{
    m_request->set_body_delivery_paused(true);
}

void NetworkTransportHandle::stop()
{
    m_request->stop();
}

void NetworkTransportHandle::release_transfer_lease()
{
    m_request->release_transfer_lease();
}

void NetworkTransportHandle::set_on_requires_network(Function<void()> on_requires_network)
{
    m_request->on_requires_network = move(on_requires_network);
}

void NetworkTransportHandle::set_on_teardown(Function<void(Requests::Request::TeardownReason)> on_teardown)
{
    m_request->on_teardown = move(on_teardown);
}

}
