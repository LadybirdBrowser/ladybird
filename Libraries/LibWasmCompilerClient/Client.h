/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <WasmCompiler/WasmCompilerClientEndpoint.h>
#include <WasmCompiler/WasmCompilerServerEndpoint.h>

namespace WasmCompilerClient {

class Client final
    : public IPC::ConnectionToServer<WasmCompilerClientEndpoint, WasmCompilerServerEndpoint>
    , public WasmCompilerClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::WasmCompilerServer::InitTransport;

    explicit Client(NonnullOwnPtr<IPC::Transport>);

    Core::AnonymousBuffer compile(Core::AnonymousBuffer const&);

    Function<void()> on_death;

private:
    struct PendingCompilation final : public AtomicRefCounted<PendingCompilation> {
        Sync::Mutex mutex;
        Sync::ConditionVariable condition { mutex };
        Optional<Core::AnonymousBuffer> result;
    };

    virtual void die() override;
    virtual void did_compile(u64 request_id, Core::AnonymousBuffer output) override;

    Atomic<u64> m_next_request_id { 0 };

    Sync::Mutex m_pending_compilations_mutex;
    HashMap<u64, NonnullRefPtr<PendingCompilation>> m_pending_compilations;
};

}
