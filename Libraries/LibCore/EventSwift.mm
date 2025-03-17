/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/EventSwift.h>

namespace Core {
void deferred_invoke_block(EventLoop& event_loop, void (^invokee)(void))
{
    event_loop.deferred_invoke(invokee);
}

}
