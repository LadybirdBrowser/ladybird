/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibGC/HeapAccess.h>
#include <LibThreading/ThreadPool.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

// What waits for the thread pool on the thread that submitted the work. It stays on that thread, as it may hold GC roots,
// and the pool's thread only ever sees its ID.
using ThreadPoolCompletion = Function<void(void* result)>;

WEB_API u64 add_thread_pool_completion(ThreadPoolCompletion);
WEB_API ThreadPoolCompletion take_thread_pool_completion(u64 id);

// Forgets every completion that is still waiting on this thread. An agent that goes away does this, so that nothing
// keeps a root into its heap, and work that finishes later finds nothing to run.
WEB_API void discard_thread_pool_completions();

// Runs `work` on the thread pool and passes its result to `on_complete` on the event loop of this thread, unless that
// event loop or the agent of this thread has gone away by then.
template<typename Result, typename Work>
void run_on_thread_pool(Work work, Function<void(Result)> on_complete)
{
    auto completion_id = add_thread_pool_completion([on_complete = move(on_complete)](void* result) mutable {
        on_complete(move(*static_cast<Result*>(result)));
    });

    Threading::ThreadPool::the().submit([work = move(work), completion_id, event_loop = Core::EventLoop::current_weak()]() mutable {
        GC::forbid_heap_access_on_this_thread();
        Result result = work();

        auto origin_event_loop = event_loop->take();
        if (!origin_event_loop.is_alive())
            return;
        origin_event_loop->deferred_invoke([completion_id, result = move(result)]() mutable {
            if (auto completion = take_thread_pool_completion(completion_id))
                completion(&result);
        });
    });
}

}
