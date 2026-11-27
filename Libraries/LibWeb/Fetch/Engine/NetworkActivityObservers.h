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
#include <LibWeb/Fetch/Engine/NetworkConfiguration.h>

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

    Atomic<size_t, AK::MemoryOrder::memory_order_relaxed> m_pages_with_devtools_client { 0 };
    RWLockProtected<RefPtr<NetworkConfiguration const>> m_network_configuration;
};

}
