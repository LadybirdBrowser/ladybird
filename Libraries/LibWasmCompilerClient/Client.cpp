/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWasmCompilerClient/Client.h>

namespace WasmCompilerClient {

static constexpr auto COMPILE_TIMEOUT = AK::Duration::from_seconds(70);

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WasmCompilerClientEndpoint, WasmCompilerServerEndpoint>(*this, move(transport))
{
}

Core::AnonymousBuffer Client::compile(Core::AnonymousBuffer const& buffer)
{
    auto request_id = m_next_request_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    auto pending_compilation = adopt_ref(*new PendingCompilation);
    {
        Sync::MutexLocker locker(m_pending_compilations_mutex);
        if (!is_open())
            return {};
        m_pending_compilations.set(request_id, pending_compilation);
    }

    async_compile(buffer, request_id);

    ScopeGuard remove_pending_compilation = [&]() {
        Sync::MutexLocker locker(m_pending_compilations_mutex);
        m_pending_compilations.remove(request_id);
    };

    Sync::MutexLocker locker(pending_compilation->mutex);
    auto timeout_at = MonotonicTime::now() + COMPILE_TIMEOUT;

    while (!pending_compilation->result.has_value()) {
        auto now = MonotonicTime::now();
        if (now >= timeout_at)
            break;
        pending_compilation->condition.wait_for(timeout_at - now);
    }

    if (!pending_compilation->result.has_value())
        return {};
    return pending_compilation->result.release_value();
}

void Client::die()
{
    HashMap<u64, NonnullRefPtr<PendingCompilation>> pending_compilations;
    {
        Sync::MutexLocker locker(m_pending_compilations_mutex);
        pending_compilations = move(m_pending_compilations);
    }

    for (auto& pending_compilation : pending_compilations) {
        Sync::MutexLocker locker(pending_compilation.value->mutex);
        pending_compilation.value->result = Core::AnonymousBuffer {};
        pending_compilation.value->condition.broadcast();
    }
}

void Client::did_compile(u64 request_id, Core::AnonymousBuffer output)
{
    Optional<NonnullRefPtr<PendingCompilation>> pending_compilation;
    {
        Sync::MutexLocker locker(m_pending_compilations_mutex);
        pending_compilation = m_pending_compilations.take(request_id);
    }

    if (!pending_compilation.has_value())
        return;

    Sync::MutexLocker locker(pending_compilation.value()->mutex);
    pending_compilation.value()->result = move(output);
    pending_compilation.value()->condition.broadcast();
}

}
