/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>

namespace Web::Fetch::Engine {

// Receives every event for a fetch on its agent's thread, in sending order.
class EngineEventSink {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~EngineEventSink() = default;

    virtual void handle_event(EngineEvent) = 0;
};

}
