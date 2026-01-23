/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibCore/System.h>
#include <MediaServer/ConnectionFromClient.h>

namespace MediaServer {

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<MediaServerClientEndpoint, MediaServerServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

void ConnectionFromClient::die()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    if (s_connections.is_empty())
        Core::EventLoop::current().quit(0);
}

Messages::MediaServerServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

Messages::MediaServerServer::CreateSharedSingleProducerCircularBufferResponse ConnectionFromClient::create_shared_single_producer_circular_buffer(size_t capacity)
{
    auto shared_buffer_or_error = Core::SharedSingleProducerCircularBuffer::create(capacity);
    if (shared_buffer_or_error.is_error()) {
        dbgln("MediaServer: failed to create shared circular buffer: {}", shared_buffer_or_error.error());
        return { Core::AnonymousBuffer {} };
    }

    return { shared_buffer_or_error.release_value().anonymous_buffer() };
}

ErrorOr<IPC::File> ConnectionFromClient::connect_new_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error())
        return err.release_error();

    auto client_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket_or_error.is_error()) {
        (void)Core::System::close(socket_fds[0]);
        (void)Core::System::close(socket_fds[1]);
        return client_socket_or_error.release_error();
    }

    auto client_socket = client_socket_or_error.release_value();
    (void)adopt_ref(*new ConnectionFromClient(make<IPC::Transport>(move(client_socket))));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::MediaServerServer::ConnectNewClientsResponse ConnectionFromClient::connect_new_clients(size_t count)
{
    Vector<IPC::File> files;
    files.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto file_or_error = connect_new_client();
        if (file_or_error.is_error()) {
            dbgln("MediaServer: failed to connect new client: {}", file_or_error.error());
            return Vector<IPC::File> {};
        }
        files.unchecked_append(file_or_error.release_value());
    }

    return files;
}

}
