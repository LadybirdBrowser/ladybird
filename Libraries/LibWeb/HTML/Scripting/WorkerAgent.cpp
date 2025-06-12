/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/WorkerAgent.h>

namespace Web::HTML {

NonnullOwnPtr<WorkerAgent> WorkerAgent::create(GC::Heap& heap, CanBlock can_block)
{
    auto agent = adopt_own(*new WorkerAgent(can_block));
    agent->event_loop = heap.allocate<HTML::EventLoop>(HTML::EventLoop::Type::Worker);
    return agent;
}

}
