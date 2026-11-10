/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>

namespace Web::Fetch::Engine {

// Which observers of network activity an agent has. The agent writes it, and its fetch engine reads it before it
// copies a response's body bytes for them.
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

private:
    NetworkActivityObservers() = default;

    Atomic<size_t, AK::MemoryOrder::memory_order_relaxed> m_pages_with_devtools_client { 0 };
};

}
