/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AudioServerExampleHelper.h"

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/MachPort.h>
#else
#    include <LibIPC/TransportSocket.h>
#endif

#if defined(AK_OS_MACOS)
AudioServerExampleBootstrap::AudioServerExampleBootstrap(ByteString server_name)
    : m_mach_port_listener(make<IPC::MachBootstrapListener>(move(server_name)))
{
    // Real clients do not do this. The browser process already acts as the
    // broker and owns the bootstrap server. These examples launch AudioServer
    // directly, so they must temporarily provide the missing parent side of
    // the initial IPC handshake themselves.
    m_mach_port_listener->on_bootstrap_request = [this](IPC::MachBootstrapListener::BootstrapRequest request) {
        auto registration_result = m_transport_bootstrap_server.handle_bootstrap_request(request.pid, move(request.reply_port));
        if (registration_result.is_error()) {
            dbgln("Failed to bootstrap AudioServer transport for pid {}: {}", request.pid, registration_result.error());
            VERIFY_NOT_REACHED();
        }
        if (registration_result.release_value().has<IPC::TransportBootstrapMachServer::OnDemandTransport>()) {
            dbgln("No");
            VERIFY_NOT_REACHED();
        }
    };
}

bool AudioServerExampleBootstrap::is_initialized() const
{
    return m_mach_port_listener && m_mach_port_listener->is_initialized();
}

IPC::TransportBootstrapMachServer& AudioServerExampleBootstrap::transport_bootstrap_server()
{
    return m_transport_bootstrap_server;
}
#endif

static ErrorOr<ByteString> find_audioserver_executable_path()
{
    ByteString current_executable_path = TRY(Core::System::current_executable_path());

    LexicalPath current_executable_lexical_path(current_executable_path);
    ByteString current_dir = current_executable_lexical_path.dirname();

    Vector<ByteString> candidates;
    candidates.append(LexicalPath::join(current_dir, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, "Ladybird.app"sv, "Contents"sv, "MacOS"sv, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, ".."sv, "libexec"sv, "AudioServer"sv).string());

    for (ByteString& candidate : candidates) {
        ByteString path = LexicalPath::canonicalized_path(move(candidate));
        if (!Core::System::access(path, X_OK).is_error())
            return path;
    }

    return Error::from_string_literal("Failed to locate AudioServer executable");
}

ErrorOr<SpawnedAudioServerForExample> launch_audioserver_for_example(StringView server_name)
{
    ByteString audio_server_path = TRY(find_audioserver_executable_path());
    Vector<ByteString> arguments;

    // This helper exists only because the examples have no browser process.
    // Typical clients do not launch AudioServer or build this broker transport.
    // They just ask the existing broker for a new client handle and use that.
#if defined(AK_OS_MACOS)
    auto bootstrap = make<AudioServerExampleBootstrap>(ByteString { server_name });
    if (!bootstrap->is_initialized())
        return Error::from_string_literal("Failed to initialize AudioServer Mach bootstrap server");

    // On macOS, passing --mach-server-name does not mean AudioServer will
    // register itself under that name. It means AudioServer expects a parent
    // process to already be listening there and to hand it the initial
    // transport ports. In the real app that parent is the broker. Here we
    // fake just enough of that setup for the example to start.
    Core::MachPort port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    Core::MachPort port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));
    Core::MachPort port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    Core::MachPort port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    arguments.append("--mach-server-name"sv);
    arguments.append(server_name);

    Core::ProcessSpawnOptions options {
        .name = "AudioServer"sv,
        .executable = audio_server_path,
        .search_for_executable_in_path = false,
        .arguments = arguments,
    };

    Core::Process process = TRY(Core::Process::spawn(options));
    bootstrap->transport_bootstrap_server().register_child_transport(process.pid(), IPC::TransportBootstrapMachPorts { move(port_b_recv), move(port_a_send) });

    NonnullOwnPtr<IPC::Transport> transport = make<IPC::Transport>(move(port_a_recv), move(port_b_send));
    auto broker = adopt_ref(*new Audio::BrokerOfAudioServer(move(transport)));
    return SpawnedAudioServerForExample { move(process), move(broker), move(bootstrap) };
#else
    // Same idea on non-macOS, just with a socket takeover instead of Mach
    // ports. This is startup scaffolding for the standalone example, not the
    // interesting part of normal client code.
    (void)server_name;
    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));

    ArmedScopeGuard close_broker_fd = [&] { MUST(Core::System::close(socket_fds[0])); };
    ArmedScopeGuard close_child_fd = [&] { MUST(Core::System::close(socket_fds[1])); };

    ByteString takeover_string = ByteString::formatted("example:{}", socket_fds[1]);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    Core::ProcessSpawnOptions options {
        .name = "AudioServer"sv,
        .executable = audio_server_path,
        .search_for_executable_in_path = false,
        .arguments = arguments,
    };

    Core::Process process = TRY(Core::Process::spawn(options));

    TRY(Core::System::close(socket_fds[1]));
    close_child_fd.disarm();

    NonnullOwnPtr<Core::LocalSocket> socket = TRY(Core::LocalSocket::adopt_fd(socket_fds[0]));
    close_broker_fd.disarm();
    TRY(socket->set_blocking(true));

    NonnullOwnPtr<IPC::Transport> transport = TRY(IPC::TransportSocket::from_socket(move(socket)));
    auto broker = adopt_ref(*new Audio::BrokerOfAudioServer(move(transport)));
    return SpawnedAudioServerForExample { move(process), move(broker), nullptr };
#endif
}

ErrorOr<NonnullRefPtr<Audio::SessionClientOfAudioServer>> create_session_client_for_example(IPC::TransportHandle const& handle)
{
    // This part is the normal client-side flow. Once the broker hands us a
    // transport handle, the platform-specific bootstrap code above is no
    // longer relevant.
    NonnullOwnPtr<IPC::Transport> transport = TRY(handle.create_transport());
    return adopt_ref(*new Audio::SessionClientOfAudioServer(move(transport)));
}
