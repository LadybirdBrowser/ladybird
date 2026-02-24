/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibThreading/BackgroundAction.h>
#include <WebAudioWorker/MainEventLoop.h>
#include <WebAudioWorker/WebAudioBrokerConnection.h>
#include <WebAudioWorker/WebAudioSessionConnection.h>

namespace Web::WebAudio {

static HashMap<int, RefPtr<WebAudioBrokerConnection>> s_connections;
static IDAllocator s_client_ids;

WebAudioBrokerConnection::WebAudioBrokerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToBrokerFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromBrokerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

WebAudioBrokerConnection::~WebAudioBrokerConnection() = default;

bool WebAudioBrokerConnection::has_any_connection()
{
    return !s_connections.is_empty();
}

void WebAudioBrokerConnection::maybe_quit_event_loop_if_unused()
{
    // Broker connections are long-lived control channels from the browser process.
    // They should not keep the worker alive once all session connections are gone.
    if (WebAudioSessionConnection::has_any_connection())
        return;

    Threading::quit_background_thread();
    auto event_loop = Web::WebAudio::main_event_loop();
    VERIFY(event_loop);
    if (auto strong_loop = event_loop->take(); strong_loop)
        strong_loop->quit(0);
}

void WebAudioBrokerConnection::shutdown()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    maybe_quit_event_loop_if_unused();
}

Messages::ToWebAudioWorkerFromBroker::InitTransportResponse WebAudioBrokerConnection::init_transport([[maybe_unused]] int peer_pid)
{
    VERIFY_NOT_REACHED();
}

ErrorOr<IPC::File> WebAudioBrokerConnection::connect_new_client()
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
    auto client = adopt_ref(*new WebAudioBrokerConnection(make<IPC::Transport>(move(client_socket))));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::ToWebAudioWorkerFromBroker::ConnectNewWebaudioClientResponse WebAudioBrokerConnection::connect_new_webaudio_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error()) {
        warnln("WebAudioBrokerConnection::connect_new_webaudio_client: socketpair failed: {}", err.error());
        return { IPC::File {} };
    }
    auto webaudio_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (webaudio_socket_or_error.is_error()) {
        (void)Core::System::close(socket_fds[0]);
        (void)Core::System::close(socket_fds[1]);
        warnln("WebAudioBrokerConnection::connect_new_webaudio_client: adopt_fd failed: {}", webaudio_socket_or_error.error());
        return { IPC::File {} };
    }

    auto webaudio_socket = webaudio_socket_or_error.release_value();
    auto client = WebAudioSessionConnection::construct(make<IPC::Transport>(move(webaudio_socket)), client_id());

    return { IPC::File::adopt_fd(socket_fds[1]) };
}

Messages::ToWebAudioWorkerFromBroker::ConnectNewClientsResponse WebAudioBrokerConnection::connect_new_clients(size_t count)
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
