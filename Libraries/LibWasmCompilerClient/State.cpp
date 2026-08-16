/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWasm/Types.h>
#include <LibWasmCompilerClient/State.h>
#include <LibWasmCompilerClient/ThreadedClient.h>

namespace WasmCompilerClient {

void CompilerState::install_compiler_callback()
{
    Wasm::set_cranelift_compile_callback([this](Core::AnonymousBuffer const& buffer) {
        RefPtr<ThreadedClient> client;
        {
            Sync::MutexLocker locker(m_mutex);
            client = m_client;
        }

        return client ? client->compile(buffer) : Core::AnonymousBuffer {};
    });
}

static ErrorOr<NonnullRefPtr<ThreadedClient>> connect_to_wasm_compiler(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    return ThreadedClient::create(move(transport));
}

void CompilerState::replace_connection(IPC::TransportHandle const& handle)
{
    RefPtr<ThreadedClient> new_client;

    if (auto result = connect_to_wasm_compiler(handle); result.is_error())
        dbgln("Failed to connect to WebAssembly compiler: {}", result.error());
    else
        new_client = result.release_value();

    RefPtr<ThreadedClient> old_client;
    {
        Sync::MutexLocker locker(m_mutex);
        old_client = move(m_client);
        m_client = move(new_client);
    }
}

CompilerState& compiler_state()
{
    static NeverDestroyed<CompilerState> state;
    return *state;
}

}
