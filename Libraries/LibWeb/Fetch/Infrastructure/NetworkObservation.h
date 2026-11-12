/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGC/Forward.h>

namespace Web::Fetch::Infrastructure {

// What a network observer keeps for one request, opaque to Fetch.
class NetworkObserverState : public RefCounted<NetworkObserverState> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    virtual ~NetworkObserverState() = default;

protected:
    NetworkObserverState() = default;
};

// The network observer state of a request, which the request shares with the controller of each of its fetches.
class NetworkObservation final : public RefCounted<NetworkObservation> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    [[nodiscard]] static NonnullRefPtr<NetworkObservation> create() { return adopt_ref(*new NetworkObservation); }

    [[nodiscard]] RefPtr<NetworkObserverState> const& observer_state() const { return m_observer_state; }
    void set_observer_state(NonnullRefPtr<NetworkObserverState> observer_state) { m_observer_state = move(observer_state); }

private:
    NetworkObservation() = default;

    RefPtr<NetworkObserverState> m_observer_state;
};

}
