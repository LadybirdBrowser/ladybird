/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Connection.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

Actor::Actor(DevToolsServer& devtools, String name)
    : m_devtools(devtools)
    , m_name(move(name))
{
}

Actor::~Actor() = default;

void Actor::message_received(StringView type, JsonObject message)
{
    auto message_id = m_next_message_id++;
    m_pending_responses.empend(message_id, OptionalNone {});

    handle_message({ type, move(message), message_id });
}

void Actor::send_response(Message const& message, JsonObject response)
{
    auto& connection = devtools().connection();
    if (!connection)
        return;

    response.set("from"sv, name());

    for (auto const& [i, pending_response] : enumerate(m_pending_responses)) {
        if (pending_response.id != message.id)
            continue;

        pending_response.response = move(response);

        if (i != 0)
            return;
    }

    size_t number_of_sent_messages = 0;

    for (auto const& pending_response : m_pending_responses) {
        if (!pending_response.response.has_value())
            break;

        connection->send_message(*pending_response.response);
        ++number_of_sent_messages;
    }

    m_pending_responses.remove(0, number_of_sent_messages);
}

void Actor::send_message(JsonObject message)
{
    auto& connection = devtools().connection();
    if (!connection)
        return;

    message.set("from"sv, name());

    if (m_pending_responses.is_empty()) {
        connection->send_message(message);
        return;
    }

    m_pending_responses.empend(OptionalNone {}, move(message));
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#error-packets
void Actor::send_missing_parameter_error(Optional<Message const&> message, StringView parameter)
{
    JsonObject error;
    error.set("error"sv, "missingParameter"sv);
    error.set("message"sv, MUST(String::formatted("Missing parameter: '{}'", parameter)));

    if (message.has_value())
        send_response(*message, move(error));
    else
        send_message(move(error));
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#error-packets
void Actor::send_unrecognized_packet_type_error(Message const& message)
{
    JsonObject error;
    error.set("error"sv, "unrecognizedPacketType"sv);
    error.set("message"sv, MUST(String::formatted("Unrecognized packet type: '{}'", message.type)));
    send_response(message, move(error));
}

// https://github.com/mozilla/gecko-dev/blob/master/devtools/server/actors/object.js
// This error is not documented, but is used by Firefox nonetheless.
void Actor::send_unknown_actor_error(Optional<Message const&> message, StringView actor)
{
    JsonObject error;
    error.set("error"sv, "unknownActor"sv);
    error.set("message"sv, MUST(String::formatted("Unknown actor: '{}'", actor)));

    if (message.has_value())
        send_response(*message, move(error));
    else
        send_message(move(error));
}

}
