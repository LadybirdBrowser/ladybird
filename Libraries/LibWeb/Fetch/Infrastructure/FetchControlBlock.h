/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Badge.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Fetch/Engine/BodyIntent.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>

namespace Web::Fetch::Infrastructure {

class FetchController;

// The state of a fetch that its controller and its fetch params share.
class FetchControlBlock : public AtomicRefCounted<FetchControlBlock> {
public:
    enum class State : u8 {
        Ongoing,
        Terminated,
        Aborted,
        Stopped,
    };

    [[nodiscard]] static NonnullRefPtr<FetchControlBlock> create() { return adopt_ref(*new FetchControlBlock); }

    [[nodiscard]] State state() const { return m_state; }
    void set_state(Badge<FetchController>, State state) { m_state = state; }

    [[nodiscard]] RefPtr<FetchTimingInfo> full_timing_info() const { return m_full_timing_info; }
    void set_full_timing_info(NonnullRefPtr<FetchTimingInfo> full_timing_info) { m_full_timing_info = move(full_timing_info); }

    [[nodiscard]] Engine::BodyIntent body_intent() const { return m_body_intent; }
    void set_body_intent(Badge<FetchController>, Engine::BodyIntent body_intent)
    {
        VERIFY(!m_response_arrived);
        m_body_intent = body_intent;
    }

    // AD-HOC: Whether this fetch's response has arrived, from the network or at main fetch. From then on,
    //         consume_a_preloaded_resource() no longer changes the fetch's body intent or task destination.
    void set_response_arrived() { m_response_arrived = true; }
    [[nodiscard]] bool response_arrived() const { return m_response_arrived; }

    [[nodiscard]] bool has_started_request() const { return m_has_started_request; }
    void set_has_started_request(Badge<FetchController>) { m_has_started_request = true; }

    [[nodiscard]] bool requires_network() const { return m_requires_network; }
    void set_requires_network() { m_requires_network = true; }

private:
    FetchControlBlock() = default;

    // https://fetch.spec.whatwg.org/#fetch-controller-state
    // state (default "ongoing")
    //    "ongoing", "terminated", or "aborted"
    State m_state { State::Ongoing };

    // https://fetch.spec.whatwg.org/#fetch-controller-full-timing-info
    // full timing info (default null)
    //    Null or a fetch timing info.
    RefPtr<FetchTimingInfo> m_full_timing_info;

    Engine::BodyIntent m_body_intent { Engine::BodyIntent::DrainAndDiscard };
    bool m_response_arrived { false };
    bool m_has_started_request { false };
    bool m_requires_network { false };
};

}
