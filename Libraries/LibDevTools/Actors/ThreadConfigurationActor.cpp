/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/ThreadConfigurationActor.h>

namespace DevTools {

NonnullRefPtr<ThreadConfigurationActor> ThreadConfigurationActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new ThreadConfigurationActor(devtools, move(name)));
}

ThreadConfigurationActor::ThreadConfigurationActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

ThreadConfigurationActor::~ThreadConfigurationActor() = default;

void ThreadConfigurationActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "updateConfiguration"sv) {
        auto configuration = get_required_parameter<JsonObject>(message, "configuration"sv);
        if (!configuration.has_value())
            return;

        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject ThreadConfigurationActor::serialize_configuration() const
{
    JsonObject target;
    target.set("actor"sv, name());

    return target;
}

}
