/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h>
#include <LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h>

namespace Web::WebAudio {

class BrokerOfWebAudioWorker final
    : public IPC::ConnectionToServer<ToBrokerFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromBrokerEndpoint>
    , public ToBrokerFromWebAudioWorkerEndpoint {
    C_OBJECT_ABSTRACT(BrokerOfWebAudioWorker);

public:
    using InitTransport = Messages::ToWebAudioWorkerFromBroker::InitTransport;

    explicit BrokerOfWebAudioWorker(NonnullOwnPtr<IPC::Transport>);

    ErrorOr<IPC::File> connect_new_webaudio_client_socket();

    pid_t pid() const { return m_pid; }
    void set_pid(pid_t pid) { m_pid = pid; }

    Function<void()> on_death;

private:
    void die() override { shutdown(); }
    void shutdown();

    pid_t m_pid { -1 };
};

}
