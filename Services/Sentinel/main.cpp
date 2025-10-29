/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SentinelServer.h"
#include <LibCore/EventLoop.h>
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments)
{
    AK::set_rich_debug_enabled(true);

    Core::EventLoop event_loop;

    auto server = TRY(Sentinel::SentinelServer::create());

    dbgln("Sentinel: Security daemon started");
    dbgln("Sentinel: Listening on socket: /tmp/sentinel.sock");

    return event_loop.exec();
}
