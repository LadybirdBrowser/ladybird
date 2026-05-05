/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibThreading/BackgroundAction.h>
#include <WebAudioWorker/MainEventLoop.h>
#include <WebAudioWorker/WebAudioBrokerConnection.h>
#include <WebAudioWorker/WebAudioSessionConnection.h>

namespace Web::WebAudio {

static ErrorOr<void> setup_audio_server(IPC::TransportHandle const& handle, ByteString const& grant_id)
{
    auto transport = TRY(handle.create_transport());
    auto new_client = TRY(try_make_ref_counted<Audio::SessionClientOfAudioServer>(move(transport)));
#ifdef AK_OS_WINDOWS
    auto response = new_client->send_sync<Messages::ToAudioServerFromSessionClient::InitTransport>(Core::System::getpid());
    new_client->transport().set_peer_pid(response->peer_pid());
#endif
    NonnullRefPtr<Audio::SessionClientOfAudioServer> client = *new_client;
    if (!grant_id.is_empty())
        client->set_grant_id(grant_id);
    client->on_devices_changed = [] { };
    Audio::SessionClientOfAudioServer::set_default_client(client);
    return {};
}

static HashMap<int, RefPtr<WebAudioBrokerConnection>> s_connections;
static IDAllocator s_client_ids;

WebAudioBrokerConnection::WebAudioBrokerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToBrokerFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromBrokerEndpoint>(
          *this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

WebAudioBrokerConnection::~WebAudioBrokerConnection() = default;

bool WebAudioBrokerConnection::has_any_connection() { return !s_connections.is_empty(); }

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

Messages::ToWebAudioWorkerFromBroker::InitTransportResponse
WebAudioBrokerConnection::init_transport([[maybe_unused]] int peer_pid)
{
    VERIFY_NOT_REACHED();
}

void WebAudioBrokerConnection::connect_to_audio_server(IPC::TransportHandle handle, ByteString grant_id)
{
    if (auto result = setup_audio_server(handle, grant_id); result.is_error()) {
        warnln("WebAudioBrokerConnection::connect_to_audio_server: {}", result.error());
        WebAudioSessionConnection::audio_server_did_fail_to_connect(ByteString::formatted("{}", result.error()));
        return;
    }

    WebAudioSessionConnection::audio_server_did_become_ready();
}

ErrorOr<IPC::TransportHandle> WebAudioBrokerConnection::connect_new_client()
{
    auto paired = TRY(IPC::Transport::create_paired());
    auto handle = move(paired.remote_handle);
    [[maybe_unused]] auto client = adopt_ref(*new WebAudioBrokerConnection(move(paired.local)));
    return handle;
}

Messages::ToWebAudioWorkerFromBroker::ConnectNewWebaudioClientResponse
WebAudioBrokerConnection::connect_new_webaudio_client()
{
    auto paired_or_error = IPC::Transport::create_paired();
    if (paired_or_error.is_error()) {
        warnln("WebAudioBrokerConnection::connect_new_webaudio_client: create_paired failed: {}",
            paired_or_error.error());
        return { IPC::TransportHandle {} };
    }

    auto paired = paired_or_error.release_value();
    auto handle = move(paired.remote_handle);
    [[maybe_unused]] auto client = WebAudioSessionConnection::construct(move(paired.local), client_id());

    return { move(handle) };
}

Messages::ToWebAudioWorkerFromBroker::ConnectNewClientsResponse
WebAudioBrokerConnection::connect_new_clients(size_t count)
{
    Vector<IPC::TransportHandle> handles;
    handles.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto handle_or_error = connect_new_client();
        if (handle_or_error.is_error()) {
            dbgln("WebAudio client connection failed: {}", handle_or_error.error());
            return Vector<IPC::TransportHandle> {};
        }
        handles.unchecked_append(handle_or_error.release_value());
    }

    return handles;
}

} // namespace Web::WebAudio
