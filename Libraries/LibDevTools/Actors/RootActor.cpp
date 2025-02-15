/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/DeviceActor.h>
#include <LibDevTools/Actors/PreferenceActor.h>
#include <LibDevTools/Actors/ProcessActor.h>
#include <LibDevTools/Actors/RootActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-root-actor
NonnullRefPtr<RootActor> RootActor::create(DevToolsServer& devtools, ByteString name)
{
    auto actor = adopt_ref(*new RootActor(devtools, move(name)));

    JsonObject traits;
    traits.set("sources"sv, false);
    traits.set("highlightable"sv, true);
    traits.set("customHighlighters"sv, true);
    traits.set("networkMonitor"sv, false);

    JsonObject message;
    message.set("from"sv, actor->name());
    message.set("applicationType"sv, "browser"sv);
    message.set("traits"sv, move(traits));
    actor->send_message(move(message));

    return actor;
}

RootActor::RootActor(DevToolsServer& devtools, ByteString name)
    : Actor(devtools, move(name))
{
}

RootActor::~RootActor() = default;

void RootActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "connect") {
        send_message(move(response));
        return;
    }

    if (type == "getRoot"sv) {
        response.set("selected"sv, 0);

        for (auto const& actor : devtools().actor_registry()) {
            if (is<DeviceActor>(*actor.value))
                response.set("deviceActor"sv, actor.key);
            else if (is<PreferenceActor>(*actor.value))
                response.set("preferenceActor"sv, actor.key);
        }

        send_message(move(response));
        return;
    }

    if (type == "getProcess"sv) {
        auto id = message.get_integer<u64>("id"sv);
        if (!id.has_value()) {
            send_missing_parameter_error("id"sv);
            return;
        }

        for (auto const& actor : devtools().actor_registry()) {
            auto const* process_actor = as_if<ProcessActor>(*actor.value);
            if (!process_actor)
                continue;
            if (process_actor->description().id != *id)
                continue;

            response.set("processDescriptor"sv, process_actor->serialize_description());
            break;
        }

        send_message(move(response));
        return;
    }

    if (type == "getTab"sv) {
        response.set("tab"sv, JsonObject {});
        send_message(move(response));
        return;
    }

    if (type == "listAddons"sv) {
        response.set("addons"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    if (type == "listProcesses"sv) {
        JsonArray processes;

        for (auto const& actor : devtools().actor_registry()) {
            if (auto const* process_actor = as_if<ProcessActor>(*actor.value))
                processes.must_append(process_actor->serialize_description());
        }

        response.set("processes"sv, move(processes));
        send_message(move(response));
        return;
    }

    if (type == "listServiceWorkerRegistrations"sv) {
        response.set("registrations"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    if (type == "listTabs"sv) {
        response.set("tabs"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    if (type == "listWorkers"sv) {
        response.set("workers"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

}
