/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/TargetConfigurationActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/Actors/ThreadConfigurationActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<WatcherActor> WatcherActor::create(DevToolsServer& devtools, ByteString name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new WatcherActor(devtools, move(name), move(tab)));
}

WatcherActor::WatcherActor(DevToolsServer& devtools, ByteString name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

WatcherActor::~WatcherActor() = default;

void WatcherActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "getParentBrowsingContextID"sv) {
        auto browsing_context_id = message.get_integer<u64>("browsingContextID"sv);
        if (!browsing_context_id.has_value()) {
            send_missing_parameter_error("browsingContextID"sv);
            return;
        }

        response.set("browsingContextID"sv, *browsing_context_id);
        send_message(move(response));
        return;
    }

    if (type == "getTargetConfigurationActor"sv) {
        if (!m_target_configuration)
            m_target_configuration = devtools().register_actor<TargetConfigurationActor>();

        response.set("configuration"sv, m_target_configuration->serialize_configuration());
        send_message(move(response));
        return;
    }

    if (type == "getThreadConfigurationActor"sv) {
        if (!m_thread_configuration)
            m_thread_configuration = devtools().register_actor<ThreadConfigurationActor>();

        response.set("configuration"sv, m_thread_configuration->serialize_configuration());
        send_message(move(response));
        return;
    }

    if (type == "watchTargets"sv) {
        auto target_type = message.get_byte_string("targetType"sv);
        if (!target_type.has_value()) {
            send_missing_parameter_error("targetType"sv);
            return;
        }

        if (target_type == "frame"sv) {
            auto& css_properties = devtools().register_actor<CSSPropertiesActor>();
            auto& inspector = devtools().register_actor<InspectorActor>(m_tab);
            auto& thread = devtools().register_actor<ThreadActor>();

            auto& target = devtools().register_actor<FrameActor>(m_tab, css_properties, inspector, thread);
            m_target = target;

            response.set("type"sv, "target-available-form"sv);
            response.set("target"sv, target.serialize_target());
            send_message(move(response));

            target.send_frame_update_message();

            JsonObject message;
            message.set("from", name());
            send_message(move(message));

            return;
        }
    }

    send_unrecognized_packet_type_error(type);
}

JsonObject WatcherActor::serialize_description() const
{
    JsonObject resources;
    resources.set("Cache"sv, false);
    resources.set("console-message"sv, false);
    resources.set("cookies"sv, false);
    resources.set("css-change"sv, false);
    resources.set("css-message"sv, false);
    resources.set("css-registered-properties"sv, false);
    resources.set("document-event"sv, false);
    resources.set("error-message"sv, false);
    resources.set("extension-storage"sv, false);
    resources.set("indexed-db"sv, false);
    resources.set("jstracer-state"sv, false);
    resources.set("jstracer-trace"sv, false);
    resources.set("last-private-context-exit"sv, false);
    resources.set("local-storage"sv, false);
    resources.set("network-event"sv, false);
    resources.set("network-event-stacktrace"sv, false);
    resources.set("platform-message"sv, false);
    resources.set("reflow"sv, false);
    resources.set("server-sent-event"sv, false);
    resources.set("session-storage"sv, false);
    resources.set("source"sv, false);
    resources.set("stylesheet"sv, false);
    resources.set("thread-state"sv, false);
    resources.set("websocket"sv, false);

    JsonObject description;
    description.set("shared_worker"sv, false);
    description.set("service_worker"sv, false);
    description.set("frame"sv, true);
    description.set("process"sv, false);
    description.set("worker"sv, false);
    description.set("resources"sv, move(resources));

    return description;
}

}
