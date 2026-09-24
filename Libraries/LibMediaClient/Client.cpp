/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/System.h>
#include <LibMediaClient/Client.h>

namespace MediaClient {

static NeverDestroyed<Client::TransportFactory> s_transport_factory;
static NeverDestroyed<RefPtr<Client>> s_client;

void Client::set_transport_factory(TransportFactory factory)
{
    *s_transport_factory = move(factory);
}

ErrorOr<NonnullRefPtr<Client>> Client::acquire()
{
    if (*s_client)
        return NonnullRefPtr(**s_client);
    if (!*s_transport_factory)
        return Error::from_string_literal("No media server transport factory is installed");

    auto transport = TRY((*s_transport_factory)());
    auto client = TRY(try_make_ref_counted<Client>(move(transport)));
#ifdef AK_OS_WINDOWS
    auto response = client->send_sync<InitTransport>(Core::System::getpid());
    client->transport().set_peer_pid(response->peer_pid());
#endif
    *s_client = client;
    return client;
}

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<MediaClientEndpoint, MediaServerEndpoint>(*this, move(transport))
{
}

Client::~Client() = default;

void Client::die()
{
    verify_event_loop();
    if (*s_client == this)
        *s_client = nullptr;
    if (on_death)
        on_death();
}

void Client::verify_event_loop() const
{
    if (Core::EventLoop::is_running())
        VERIFY(&Core::EventLoop::current() == m_creation_event_loop);
}

}
