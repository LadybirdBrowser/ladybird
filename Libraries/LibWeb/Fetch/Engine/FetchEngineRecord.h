/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>

namespace Web::Fetch::Engine {

// What the fetch engine keeps of a fetch it has started.
class FetchEngineRecord final : public RefCounted<FetchEngineRecord> {
public:
    [[nodiscard]] static NonnullRefPtr<FetchEngineRecord> create(NonnullOwnPtr<EngineEventSink> event_sink)
    {
        return adopt_ref(*new FetchEngineRecord(move(event_sink)));
    }

    [[nodiscard]] EngineEventSink& event_sink() { return *m_event_sink; }

private:
    explicit FetchEngineRecord(NonnullOwnPtr<EngineEventSink> event_sink)
        : m_event_sink(move(event_sink))
    {
    }

    NonnullOwnPtr<EngineEventSink> m_event_sink;
};

}
