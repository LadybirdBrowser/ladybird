/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <LibAudioServer/BrokerOfAudioServer.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibCore/Process.h>
#include <LibIPC/TransportHandle.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/MachBootstrapListener.h>
#    include <LibIPC/TransportBootstrapMach.h>
#endif

class AudioServerExampleBootstrap {
    AK_MAKE_NONCOPYABLE(AudioServerExampleBootstrap);

public:
#if defined(AK_OS_MACOS)
    explicit AudioServerExampleBootstrap(ByteString server_name);

    bool is_initialized() const;
    IPC::TransportBootstrapMachServer& transport_bootstrap_server();
#else
    AudioServerExampleBootstrap() = default;
#endif

private:
#if defined(AK_OS_MACOS)
    OwnPtr<IPC::MachBootstrapListener> m_mach_port_listener;
    IPC::TransportBootstrapMachServer m_transport_bootstrap_server;
#endif
};

struct SpawnedAudioServerForExample {
    Core::Process process;
    NonnullRefPtr<Audio::BrokerOfAudioServer> broker;
    OwnPtr<AudioServerExampleBootstrap> bootstrap;
};

ErrorOr<SpawnedAudioServerForExample> launch_audioserver_for_example(StringView server_name);
ErrorOr<NonnullRefPtr<Audio::SessionClientOfAudioServer>> create_session_client_for_example(IPC::TransportHandle const& handle);
