/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RWLockProtected.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibWeb/Fetch/Engine/NetworkConfiguration.h>
#include <LibWeb/Fetch/Engine/NetworkInterception.h>

namespace Web::Fetch::Engine {

// Which observers of network activity an agent has, and the network configuration of its requests. The agent writes
// it, and its fetch engine reads it as its fetches go on.
class NetworkActivityObservers final : public AtomicRefCounted<NetworkActivityObservers> {
public:
    [[nodiscard]] static NonnullRefPtr<NetworkActivityObservers> create() { return adopt_ref(*new NetworkActivityObservers); }

    // Whether a page of the agent has a DevTools client.
    [[nodiscard]] bool has_devtools_client() const { return m_pages_with_devtools_client.load() > 0; }
    void page_did_gain_devtools_client() { m_pages_with_devtools_client.fetch_add(1); }
    void page_did_lose_devtools_client()
    {
        auto previous_count = m_pages_with_devtools_client.fetch_sub(1);
        VERIFY(previous_count > 0);
    }

    [[nodiscard]] bool observes_fetches() const { return m_fetch_observation.load() & observes_fetches_bit; }
    [[nodiscard]] bool intercepts(NetworkPhase phase) const { return m_fetch_observation.load() & intercepts_bit(phase); }
    void set_fetch_observation(bool observes_fetches, ReadonlySpan<NetworkPhase> intercepted_phases)
    {
        u8 fetch_observation = observes_fetches ? observes_fetches_bit : 0;
        for (auto phase : intercepted_phases)
            fetch_observation |= intercepts_bit(phase);
        m_fetch_observation.store(fetch_observation);
    }

    [[nodiscard]] RefPtr<NetworkConfiguration const> network_configuration() const
    {
        return m_network_configuration.with_read_locked([](auto const& configuration) { return configuration; });
    }
    void set_network_configuration(RefPtr<NetworkConfiguration const> configuration)
    {
        m_network_configuration.with_write_locked([&](auto& current) { current = move(configuration); });
    }

private:
    NetworkActivityObservers() = default;

    static constexpr u8 observes_fetches_bit = 1;
    static constexpr u8 intercepts_bit(NetworkPhase phase) { return 2 << to_underlying(phase); }

    Atomic<size_t, AK::MemoryOrder::memory_order_relaxed> m_pages_with_devtools_client { 0 };
    Atomic<u8> m_fetch_observation { 0 };
    RWLockProtected<RefPtr<NetworkConfiguration const>> m_network_configuration;
};

}
