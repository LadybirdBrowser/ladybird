/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <WasmCompiler/Compiler.h>
#include <WasmCompiler/ConnectionFromClient.h>

namespace WasmCompiler {

static NeverDestroyed<HashMap<int, NonnullRefPtr<ConnectionFromClient>>> s_connections;
static int s_next_client_id { 1 };

static int allocate_client_id()
{
    VERIFY(s_next_client_id != NumericLimits<int>::max());
    return s_next_client_id++;
}

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, Compiler& compiler, Role role)
    : IPC::ConnectionFromClient<WasmCompilerClientEndpoint, WasmCompilerServerEndpoint>(*this, move(transport), allocate_client_id())
    , m_compiler(compiler)
    , m_role(role)
{
    s_connections->set(client_id(), *this);
}

void ConnectionFromClient::die()
{
    auto terminate = m_role == Role::Controller;
    s_connections->remove(client_id());

    if (terminate)
        Core::Process::terminate_immediately(0);
}

Messages::WasmCompilerServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected WebAssembly compiler transport initialization");
    return 0;
#endif
}

ErrorOr<IPC::TransportHandle> ConnectionFromClient::create_client()
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    // The static connection map owns this connection until its peer disconnects.
    auto client = adopt_ref(*new ConnectionFromClient(move(paired_transports.local), m_compiler, Role::Renderer));

    return handle;
}

Messages::WasmCompilerServer::ConnectNewClientsResponse ConnectionFromClient::connect_new_clients(size_t count)
{
    if (m_role != Role::Controller)
        return Vector<IPC::TransportHandle> {};

    Vector<IPC::TransportHandle> handles;
    handles.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto handle = create_client();
        if (handle.is_error()) {
            dbgln("Failed to connect a WebAssembly compiler client: {}", handle.error());
            return Vector<IPC::TransportHandle> {};
        }
        handles.unchecked_append(handle.release_value());
    }

    return handles;
}

void ConnectionFromClient::compile(Core::AnonymousBuffer input, u64 request_id)
{
    if (m_role != Role::Renderer) {
        async_did_compile(request_id, {});
        return;
    }

    m_compiler.compile(*this, request_id, input);
}

}
