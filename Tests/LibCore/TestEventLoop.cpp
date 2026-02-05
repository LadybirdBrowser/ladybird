/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestEventLoop.h"
#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>

void install_thread_local_event_loop()
{
    thread_local OwnPtr<Core::EventLoop> s_thread_local_event_loop = nullptr;
    if (!s_thread_local_event_loop)
        s_thread_local_event_loop = make<Core::EventLoop>();
}
