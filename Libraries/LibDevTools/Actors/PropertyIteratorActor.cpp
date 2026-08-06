/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/Actors/PropertyIteratorActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<PropertyIteratorActor> PropertyIteratorActor::create(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, Vector<Property> properties)
{
    return adopt_ref(*new PropertyIteratorActor(devtools, move(name), move(thread), move(properties)));
}

PropertyIteratorActor::PropertyIteratorActor(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, Vector<Property> properties)
    : Actor(devtools, move(name))
    , m_thread(move(thread))
    , m_properties(move(properties))
{
}

PropertyIteratorActor::~PropertyIteratorActor() = default;

JsonObject PropertyIteratorActor::form() const
{
    JsonObject form;
    form.set("type"sv, "propertyIterator"sv);
    form.set("actor"sv, name());
    form.set("count"sv, m_properties.size());
    return form;
}

JsonObject PropertyIteratorActor::serialize_properties(size_t start, size_t count) const
{
    JsonObject own_properties;
    start = min(start, m_properties.size());
    auto end = start + min(count, m_properties.size() - start);
    for (size_t index = start; index < end; ++index)
        own_properties.set(m_properties[index].name, m_properties[index].descriptor);

    JsonObject response;
    response.set("ownProperties"sv, move(own_properties));
    return response;
}

void PropertyIteratorActor::handle_message(Message const& message)
{
    if (message.type == "names"sv) {
        auto indexes = get_required_parameter<JsonArray>(message, "indexes"sv);
        if (!indexes.has_value())
            return;

        JsonArray names;
        indexes->for_each([&](JsonValue const& value) {
            auto index = value.get_integer<size_t>();
            if (index.has_value() && *index < m_properties.size())
                names.must_append(m_properties[*index].name);
        });
        JsonObject response;
        response.set("names"sv, move(names));
        send_response(message, move(response));
        return;
    }

    if (message.type == "slice"sv) {
        auto start = message.data.get_integer<size_t>("start"sv).value_or(0);
        auto count = message.data.get_integer<size_t>("count"sv).value_or(m_properties.size());
        send_response(message, serialize_properties(start, count));
        return;
    }

    if (message.type == "all"sv) {
        send_response(message, serialize_properties(0, m_properties.size()));
        return;
    }

    if (message.type == "release"sv) {
        send_response(message, {});
        if (auto thread = m_thread.strong_ref()) {
            thread->release_pause_actor(*this);
        } else {
            devtools().unregister_actor(name());
        }
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
