/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebLocks {

// The id under which the lock managers of this process know the agent of the current thread.
WEB_API u64 lock_agent_id();

// https://w3c.github.io/web-locks/#termination-of-locks
// Any thread can do this for an agent, so that an agent that is stuck in script still lets go of its locks.
WEB_API void terminate_remaining_locks_and_requests(u64 lock_agent_id);

// https://w3c.github.io/web-locks/#termination-of-locks
void run_unloading_cleanup_steps(DOM::Document&);

}
