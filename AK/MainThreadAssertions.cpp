/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MainThreadAssertions.h>
#include <AK/Optional.h>

#include <thread>

namespace AK {

static Optional<std::thread::id> s_main_thread;

void initialize_main_thread()
{
    s_main_thread = std::this_thread::get_id();
}

bool is_main_thread()
{
    // We may have been called from static intialization before the main thread has been set.
    if (!s_main_thread.has_value())
        return true;

    return std::this_thread::get_id() == s_main_thread.value();
}

}
