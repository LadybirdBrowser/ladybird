/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>

namespace Compositor {

// Blocks submitted to the main dispatch queue run on this thread's event loop from then on. Only a run loop drains the
// queue otherwise, and AVFoundation shows nothing until its blocks there have run. Must be called on the main thread,
// once its event loop exists.
ErrorOr<void> drain_main_dispatch_queue_in_event_loop();

}
