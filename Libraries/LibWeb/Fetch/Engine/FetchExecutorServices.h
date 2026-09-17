/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibCore/EventLoop.h>

namespace Web::Fetch::Engine {

struct FetchExecutorServices {
    // Commands are posted to it; one posted after its death is dropped.
    NonnullRefPtr<Core::WeakEventLoopReference> event_loop;
};

}
