/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>
#include <WasmCompiler/Forward.h>
#include <WasmCompiler/WasmCompilerClientEndpoint.h>
#include <WasmCompiler/WasmCompilerServerEndpoint.h>

namespace WasmCompiler {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<WasmCompilerClientEndpoint, WasmCompilerServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    enum class Role {
        Controller,
        Renderer,
    };

    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, Compiler&, Role);

private:
    virtual void die() override;
    virtual Messages::WasmCompilerServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::WasmCompilerServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;
    virtual void compile(Core::AnonymousBuffer, u64 request_id) override;

    ErrorOr<IPC::TransportHandle> create_client();

    Compiler& m_compiler;
    Role m_role { Role::Renderer };
};

}
