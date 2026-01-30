/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <WebAudioWorker/WebAudioWorkerClientEndpoint.h>
#include <WebAudioWorker/WebAudioWorkerServerEndpoint.h>

namespace WebAudioWorkerClient {

class Client final
    : public IPC::ConnectionToServer<WebAudioWorkerClientEndpoint, WebAudioWorkerServerEndpoint>
    , public WebAudioWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::WebAudioWorkerServer::InitTransport;

    explicit Client(NonnullOwnPtr<IPC::Transport>);

    ErrorOr<IPC::File> connect_new_webaudio_client_socket();

    pid_t pid() const { return m_pid; }
    void set_pid(pid_t pid) { m_pid = pid; }

    Function<void()> on_death;

private:
    void die() override;

    pid_t m_pid { -1 };
};

}
