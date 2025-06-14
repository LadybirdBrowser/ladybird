/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/TargetConfigurationActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/Actors/ThreadConfigurationActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<WatcherActor> WatcherActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new WatcherActor(devtools, move(name), move(tab)));
}

WatcherActor::WatcherActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

WatcherActor::~WatcherActor() = default;

void WatcherActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getParentBrowsingContextID"_sv) {
        auto browsing_context_id = get_required_parameter<u64>(message, "browsingContextID"_sv);
        if (!browsing_context_id.has_value())
            return;

        response.set("browsingContextID"_sv, *browsing_context_id);
        send_response(message, move(response));
        return;
    }

    if (message.type == "getTargetConfigurationActor"_sv) {
        if (!m_target_configuration)
            m_target_configuration = devtools().register_actor<TargetConfigurationActor>();

        response.set("configuration"_sv, m_target_configuration->serialize_configuration());
        send_response(message, move(response));
        return;
    }

    if (message.type == "getThreadConfigurationActor"_sv) {
        if (!m_thread_configuration)
            m_thread_configuration = devtools().register_actor<ThreadConfigurationActor>();

        response.set("configuration"_sv, m_thread_configuration->serialize_configuration());
        send_response(message, move(response));
        return;
    }

    if (message.type == "watchResources"_sv) {
        auto resource_types = get_required_parameter<JsonArray>(message, "resourceTypes"_sv);
        if (!resource_types.has_value())
            return;

        if constexpr (DEVTOOLS_DEBUG) {
            for (auto const& resource_type : resource_types->values()) {
                if (!resource_type.is_string())
                    continue;
                if (resource_type.as_string() != "console-message"_sv)
                    dbgln("Unrecognized `watchResources` resource type: '{}'", resource_type.as_string());
            }
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "watchTargets"_sv) {
        auto target_type = get_required_parameter<String>(message, "targetType"_sv);
        if (!target_type.has_value())
            return;

        if (target_type == "frame"_sv) {
            auto& css_properties = devtools().register_actor<CSSPropertiesActor>();
            auto& console = devtools().register_actor<ConsoleActor>(m_tab);
            auto& inspector = devtools().register_actor<InspectorActor>(m_tab);
            auto& style_sheets = devtools().register_actor<StyleSheetsActor>(m_tab);
            auto& thread = devtools().register_actor<ThreadActor>();

            auto& target = devtools().register_actor<FrameActor>(m_tab, css_properties, console, inspector, style_sheets, thread);
            m_target = target;

            response.set("type"_sv, "target-available-form"_sv);
            response.set("target"_sv, target.serialize_target());
            send_response(message, move(response));

            target.send_frame_update_message();

            send_message({});
            return;
        }
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject WatcherActor::serialize_description() const
{
    JsonObject resources;
    resources.set("Cache"_sv, false);
    resources.set("console-message"_sv, true);
    resources.set("cookies"_sv, false);
    resources.set("css-change"_sv, false);
    resources.set("css-message"_sv, false);
    resources.set("css-registered-properties"_sv, false);
    resources.set("document-event"_sv, false);
    resources.set("error-message"_sv, false);
    resources.set("extension-storage"_sv, false);
    resources.set("indexed-db"_sv, false);
    resources.set("jstracer-state"_sv, false);
    resources.set("jstracer-trace"_sv, false);
    resources.set("last-private-context-exit"_sv, false);
    resources.set("local-storage"_sv, false);
    resources.set("network-event"_sv, false);
    resources.set("network-event-stacktrace"_sv, false);
    resources.set("platform-message"_sv, false);
    resources.set("reflow"_sv, false);
    resources.set("server-sent-event"_sv, false);
    resources.set("session-storage"_sv, false);
    resources.set("source"_sv, false);
    resources.set("stylesheet"_sv, false);
    resources.set("thread-state"_sv, false);
    resources.set("websocket"_sv, false);

    JsonObject description;
    description.set("shared_worker"_sv, false);
    description.set("service_worker"_sv, false);
    description.set("frame"_sv, true);
    description.set("process"_sv, false);
    description.set("worker"_sv, false);
    description.set("resources"_sv, move(resources));

    return description;
}

}
