/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibThreading/BackgroundAction.h>
#include <WebAudioWorker/WebAudioConnection.h>
#include <WebAudioWorker/WebAudioWorkerConnection.h>

namespace WebAudioWorker {

static HashMap<int, RefPtr<WebAudioWorkerConnection>> s_connections;
static IDAllocator s_client_ids;

WebAudioWorkerConnection::WebAudioWorkerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebAudioWorkerClientEndpoint, WebAudioWorkerServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

WebAudioWorkerConnection::~WebAudioWorkerConnection() = default;

bool WebAudioWorkerConnection::has_any_connection()
{
    return !s_connections.is_empty();
}

void WebAudioWorkerConnection::maybe_quit_event_loop_if_unused()
{
    if (WebAudioWorkerConnection::has_any_connection())
        return;
    if (WebAudioConnection::has_any_connection())
        return;

    Threading::quit_background_thread();
    Core::EventLoop::current().quit(0);
}

void WebAudioWorkerConnection::die()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    maybe_quit_event_loop_if_unused();
}

Messages::WebAudioWorkerServer::InitTransportResponse WebAudioWorkerConnection::init_transport([[maybe_unused]] int peer_pid)
{
    VERIFY_NOT_REACHED();
}

ErrorOr<IPC::File> WebAudioWorkerConnection::connect_new_client()
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
    auto client = adopt_ref(*new WebAudioWorkerConnection(make<IPC::Transport>(move(client_socket))));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::WebAudioWorkerServer::ConnectNewWebaudioClientResponse WebAudioWorkerConnection::connect_new_webaudio_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error()) {
        warnln("WebAudioWorkerConnection::connect_new_webaudio_client: socketpair failed: {}", err.error());
        return { IPC::File {} };
    }
    auto webaudio_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (webaudio_socket_or_error.is_error()) {
        (void)Core::System::close(socket_fds[0]);
        (void)Core::System::close(socket_fds[1]);
        warnln("WebAudioWorkerConnection::connect_new_webaudio_client: adopt_fd failed: {}", webaudio_socket_or_error.error());
        return { IPC::File {} };
    }

    auto webaudio_socket = webaudio_socket_or_error.release_value();
    auto client = WebAudioConnection::construct(make<IPC::Transport>(move(webaudio_socket)), client_id());

    return { IPC::File::adopt_fd(socket_fds[1]) };
}

Messages::WebAudioWorkerServer::ConnectNewClientsResponse WebAudioWorkerConnection::connect_new_clients(size_t count)
{
    Vector<IPC::File> files;
    files.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto file_or_error = connect_new_client();
        if (file_or_error.is_error()) {
            dbgln("WebAudio client connection failed: {}", file_or_error.error());
            return Vector<IPC::File> {};
        }
        files.unchecked_append(file_or_error.release_value());
    }

    return files;
}

}
