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
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-root-actor
NonnullRefPtr<RootActor> RootActor::create(DevToolsServer& devtools, String name)
{
    auto actor = adopt_ref(*new RootActor(devtools, move(name)));

    JsonObject traits;
    traits.set("sources"_sv, false);
    traits.set("highlightable"_sv, true);
    traits.set("customHighlighters"_sv, true);
    traits.set("networkMonitor"_sv, false);

    JsonObject message;
    message.set("applicationType"_sv, "browser"_sv);
    message.set("traits"_sv, move(traits));
    actor->send_message(move(message));

    return actor;
}

RootActor::RootActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

RootActor::~RootActor() = default;

void RootActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "connect") {
        send_response(message, move(response));
        return;
    }

    if (message.type == "getRoot"_sv) {
        response.set("selected"_sv, 0);

        for (auto const& actor : devtools().actor_registry()) {
            if (is<DeviceActor>(*actor.value))
                response.set("deviceActor"_sv, actor.key);
            else if (is<PreferenceActor>(*actor.value))
                response.set("preferenceActor"_sv, actor.key);
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "getProcess"_sv) {
        auto id = get_required_parameter<u64>(message, "id"_sv);
        if (!id.has_value())
            return;

        for (auto const& actor : devtools().actor_registry()) {
            auto const* process_actor = as_if<ProcessActor>(*actor.value);
            if (!process_actor)
                continue;
            if (process_actor->description().id != *id)
                continue;

            response.set("processDescriptor"_sv, process_actor->serialize_description());
            break;
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "getTab"_sv) {
        auto browser_id = get_required_parameter<u64>(message, "browserId"_sv);
        if (!browser_id.has_value())
            return;

        for (auto const& actor : devtools().actor_registry()) {
            auto const* tab_actor = as_if<TabActor>(*actor.value);
            if (!tab_actor)
                continue;
            if (tab_actor->description().id != *browser_id)
                continue;

            response.set("tab"_sv, tab_actor->serialize_description());
            break;
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "listAddons"_sv) {
        response.set("addons"_sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "listProcesses"_sv) {
        JsonArray processes;

        for (auto const& actor : devtools().actor_registry()) {
            if (auto const* process_actor = as_if<ProcessActor>(*actor.value))
                processes.must_append(process_actor->serialize_description());
        }

        response.set("processes"_sv, move(processes));
        send_response(message, move(response));
        return;
    }

    if (message.type == "listServiceWorkerRegistrations"_sv) {
        response.set("registrations"_sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "listTabs"_sv) {
        m_has_sent_tab_list_changed_since_last_list_tabs_request = false;

        JsonArray tabs;

        for (auto& tab_description : devtools().delegate().tab_list()) {
            auto& actor = devtools().register_actor<TabActor>(move(tab_description));
            tabs.must_append(actor.serialize_description());
        }

        response.set("tabs"_sv, move(tabs));
        send_response(message, move(response));
        return;
    }

    if (message.type == "listWorkers"_sv) {
        response.set("workers"_sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void RootActor::send_tab_list_changed_message()
{
    if (m_has_sent_tab_list_changed_since_last_list_tabs_request)
        return;

    JsonObject message;
    message.set("type"_sv, "tabListChanged"_sv);
    send_message(move(message));

    m_has_sent_tab_list_changed_since_last_list_tabs_request = true;
}

}
