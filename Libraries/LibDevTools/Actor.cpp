/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    handle_message({ type, move(message) });
}

void Actor::send_message(JsonObject message, Optional<BlockToken> block_token)
{
    if (m_block_responses && !block_token.has_value()) {
        m_blocked_responses.append(move(message));
        return;
    }

    message.set("from"sv, name());

    if (auto& connection = devtools().connection())
        connection->send_message(move(message));
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#error-packets
void Actor::send_missing_parameter_error(StringView parameter)
{
    JsonObject error;
    error.set("error"sv, "missingParameter"sv);
    error.set("message"sv, MUST(String::formatted("Missing parameter: '{}'", parameter)));
    send_message(move(error));
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#error-packets
void Actor::send_unrecognized_packet_type_error(Message const& message)
{
    JsonObject error;
    error.set("error"sv, "unrecognizedPacketType"sv);
    error.set("message"sv, MUST(String::formatted("Unrecognized packet type: '{}'", message.type)));
    send_message(move(error));
}

// https://github.com/mozilla/gecko-dev/blob/master/devtools/server/actors/object.js
// This error is not documented, but is used by Firefox nonetheless.
void Actor::send_unknown_actor_error(StringView actor)
{
    JsonObject error;
    error.set("error"sv, "unknownActor"sv);
    error.set("message"sv, MUST(String::formatted("Unknown actor: '{}'", actor)));
    send_message(move(error));
}

Actor::BlockToken Actor::block_responses()
{
    // https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-request-reply-pattern
    // The actor processes packets in the order they are received, and the client can trust that the i’th reply
    // corresponds to the i’th request.

    // The above requirement gets tricky for actors which require an async implementation. For example, the "getWalker"
    // message sent to the InspectorActor results in the server fetching the DOM tree as JSON from the WebContent process.
    // We cannot reply to the message until that is received. However, we will likely receive more messages from the
    // client in that time. We cannot reply to those messages until we've replied to the "getWalker" message. Thus, we
    // use this token to queue responses from the actor until that reply can be sent.
    return { {}, *this };
}

Actor::BlockToken::BlockToken(Badge<Actor>, Actor& actor)
    : m_actor(actor)
{
    // If we end up in a situtation where an actor has multiple async handlers at once, we will need to come up with a
    // more sophisticated blocking mechanism.
    VERIFY(!actor.m_block_responses);
    actor.m_block_responses = true;
}

Actor::BlockToken::BlockToken(BlockToken&& other)
    : m_actor(move(other.m_actor))
{
}

Actor::BlockToken& Actor::BlockToken::operator=(BlockToken&& other)
{
    m_actor = move(other.m_actor);
    return *this;
}

Actor::BlockToken::~BlockToken()
{
    auto actor = m_actor.strong_ref();
    if (!actor)
        return;

    auto blocked_responses = move(actor->m_blocked_responses);
    actor->m_block_responses = false;

    for (auto& message : blocked_responses)
        actor->send_message(move(message));
}

}
