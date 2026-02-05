/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/ThreadActor.h>

namespace DevTools {

NonnullRefPtr<ThreadActor> ThreadActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new ThreadActor(devtools, move(name)));
}

ThreadActor::ThreadActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

ThreadActor::~ThreadActor() = default;

void ThreadActor::handle_message(Message const& message)
{
    send_unrecognized_packet_type_error(message);
}

}
