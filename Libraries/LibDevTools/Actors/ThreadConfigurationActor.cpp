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

void ThreadConfigurationActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "updateConfiguration"sv) {
        auto configuration = message.get_object("configuration"sv);
        if (!configuration.has_value()) {
            send_missing_parameter_error("configuration"sv);
            return;
        }

        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

JsonObject ThreadConfigurationActor::serialize_configuration() const
{
    JsonObject target;
    target.set("actor"sv, name());

    return target;
}

}
