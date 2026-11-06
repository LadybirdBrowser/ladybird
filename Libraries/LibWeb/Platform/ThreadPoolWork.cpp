/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/Platform/ThreadPoolWork.h>

namespace Web::Platform {

static thread_local NeverDestroyed<HashMap<u64, ThreadPoolCompletion>> s_completions;
static thread_local u64 s_next_completion_id { 1 };

u64 add_thread_pool_completion(ThreadPoolCompletion completion)
{
    auto id = s_next_completion_id++;
    s_completions->set(id, move(completion));
    return id;
}

ThreadPoolCompletion take_thread_pool_completion(u64 id)
{
    return s_completions->take(id).value_or(nullptr);
}

void discard_thread_pool_completions()
{
    s_completions->clear();
}

}
