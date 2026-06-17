/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/Actors/AccessibilityActor.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/CookiesActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/IndexedDBActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/NetworkParentActor.h>
#include <LibDevTools/Actors/StorageActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/TargetConfigurationActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/Actors/ThreadConfigurationActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsDelegate.h>
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

WatcherActor::~WatcherActor()
{
    if (!m_is_watching_frame_targets)
        return;

    if (auto tab = m_tab.strong_ref())
        devtools().delegate().did_disconnect_devtools_client(tab->description());
}

void WatcherActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getNetworkParentActor"sv) {
        if (!m_network_parent)
            m_network_parent = devtools().register_actor<NetworkParentActor>();

        response.set("network"sv, m_network_parent->name());
        send_response(message, move(response));
        return;
    }

    if (message.type == "getParentBrowsingContextID"sv) {
        auto browsing_context_id = get_required_parameter<u64>(message, "browsingContextID"sv);
        if (!browsing_context_id.has_value())
            return;

        response.set("browsingContextID"sv, *browsing_context_id);
        send_response(message, move(response));
        return;
    }

    if (message.type == "getTargetConfigurationActor"sv) {
        if (!m_target_configuration)
            m_target_configuration = devtools().register_actor<TargetConfigurationActor>();

        response.set("configuration"sv, m_target_configuration->serialize_configuration());
        send_response(message, move(response));
        return;
    }

    if (message.type == "getThreadConfigurationActor"sv) {
        if (!m_thread_configuration)
            m_thread_configuration = devtools().register_actor<ThreadConfigurationActor>();

        response.set("configuration"sv, m_thread_configuration->serialize_configuration());
        send_response(message, move(response));
        return;
    }
    if (message.type == "watchResources"sv) {
        auto resource_types = get_required_parameter<JsonArray>(message, "resourceTypes"sv);
        if (!resource_types.has_value())
            return;

        bool should_send_cookie_resources = false;
        bool should_send_indexed_db_resources = false;
        bool should_send_local_storage_resources = false;
        bool should_send_session_storage_resources = false;
        if constexpr (DEVTOOLS_DEBUG) {
            for (auto const& resource_type : resource_types->values()) {
                if (!resource_type.is_string())
                    continue;
                if (!first_is_one_of(resource_type.as_string(), "console-message"sv, "cookies"sv, "indexed-db"sv, "local-storage"sv, "session-storage"sv))
                    dbgln("Unrecognized `watchResources` resource type: '{}'", resource_type.as_string());
            }
        }
        for (auto const& resource_type : resource_types->values()) {
            if (!resource_type.is_string())
                continue;
            if (resource_type.as_string() == "cookies"sv) {
                m_is_watching_cookie_resources = true;
                should_send_cookie_resources = true;
            } else if (resource_type.as_string() == "indexed-db"sv) {
                m_is_watching_indexed_db_resources = true;
                should_send_indexed_db_resources = true;
            } else if (resource_type.as_string() == "local-storage"sv) {
                m_is_watching_local_storage_resources = true;
                should_send_local_storage_resources = true;
            } else if (resource_type.as_string() == "session-storage"sv) {
                m_is_watching_session_storage_resources = true;
                should_send_session_storage_resources = true;
            }
        }

        send_response(message, move(response));
        if (should_send_cookie_resources)
            send_cookies_resource_available_message();
        if (should_send_indexed_db_resources)
            send_indexed_db_resource_available_message();
        if (should_send_local_storage_resources)
            send_storage_resource_available_message(local_storage_actor());
        if (should_send_session_storage_resources)
            send_storage_resource_available_message(session_storage_actor());
        return;
    }

    if (message.type == "watchTargets"sv) {
        auto target_type = get_required_parameter<String>(message, "targetType"sv);
        if (!target_type.has_value())
            return;

        if (target_type == "frame"sv) {
            if (!m_is_watching_frame_targets) {
                if (auto tab = m_tab.strong_ref())
                    devtools().delegate().did_connect_devtools_client(tab->description());
                m_is_watching_frame_targets = true;
            }

            auto& target = create_frame_target();

            send_response(message, move(response));

            send_frame_target_available_message(target);
            target.send_frame_update_message();
            return;
        }
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject WatcherActor::serialize_description() const
{
    JsonObject resources;
    resources.set("Cache"sv, false);
    resources.set("console-message"sv, true);
    resources.set("cookies"sv, true);
    resources.set("css-change"sv, false);
    resources.set("css-message"sv, false);
    resources.set("css-registered-properties"sv, false);
    resources.set("document-event"sv, true);
    resources.set("error-message"sv, false);
    resources.set("extension-storage"sv, false);
    resources.set("indexed-db"sv, true);
    resources.set("jstracer-state"sv, false);
    resources.set("jstracer-trace"sv, false);
    resources.set("last-private-context-exit"sv, false);
    resources.set("local-storage"sv, true);
    resources.set("network-event"sv, true);
    resources.set("network-event-stacktrace"sv, false);
    resources.set("platform-message"sv, false);
    resources.set("reflow"sv, false);
    resources.set("server-sent-event"sv, false);
    resources.set("session-storage"sv, true);
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

FrameActor& WatcherActor::create_frame_target()
{
    auto& css_properties = devtools().register_actor<CSSPropertiesActor>();
    auto& console = devtools().register_actor<ConsoleActor>(m_tab);
    auto& style_sheets = devtools().register_actor<StyleSheetsActor>(m_tab);
    auto& inspector = devtools().register_actor<InspectorActor>(m_tab, style_sheets);
    auto& thread = devtools().register_actor<ThreadActor>();
    auto& accessibility = devtools().register_actor<AccessibilityActor>(m_tab);

    auto& target = devtools().register_actor<FrameActor>(m_tab, make_weak_ptr<WatcherActor>(), css_properties, console, inspector, style_sheets, thread, accessibility);
    m_target = target;
    return target;
}

void WatcherActor::send_frame_target_available_message()
{
    auto& target = create_frame_target();
    send_frame_target_available_message(target);
}

void WatcherActor::switch_frame_target(FrameActor& previous_target, String const& url, String const& title)
{
    auto previous_target_ref = m_target.strong_ref();
    if (!previous_target_ref || previous_target_ref.ptr() != &previous_target)
        return;

    send_frame_target_destroyed_message(*previous_target_ref);
    previous_target_ref->stop_listening();
    devtools().unregister_actor(previous_target_ref->name());

    auto& target = create_frame_target();
    send_frame_target_available_message(target);
    target.send_frame_update_message();
    target.set_pending_navigation_document_events_after_target_switch(url, title);
    if (m_is_watching_cookie_resources)
        send_cookies_resource_available_message();
    if (m_is_watching_indexed_db_resources)
        send_indexed_db_resource_available_message();
    if (m_is_watching_local_storage_resources)
        send_storage_resource_available_message(local_storage_actor());
    if (m_is_watching_session_storage_resources)
        send_storage_resource_available_message(session_storage_actor());
}

void WatcherActor::send_frame_target_available_message(FrameActor& target)
{
    JsonObject message;
    message.set("type"sv, "target-available-form"sv);
    message.set("target"sv, target.serialize_target());
    send_message(move(message));
}

void WatcherActor::send_frame_target_destroyed_message(FrameActor& target)
{
    JsonObject options;
    options.set("isTargetSwitching"sv, true);
    options.set("shouldDestroyTargetFront"sv, true);

    JsonObject message;
    message.set("type"sv, "target-destroyed-form"sv);
    message.set("target"sv, target.serialize_target());
    message.set("options"sv, move(options));
    send_message(move(message));
}

CookiesActor& WatcherActor::cookies_actor()
{
    if (auto cookies = m_cookies.strong_ref())
        return *cookies;

    m_cookies = devtools().register_actor<CookiesActor>(m_tab);
    return *m_cookies.strong_ref();
}

IndexedDBActor& WatcherActor::indexed_db_actor()
{
    if (auto indexed_db = m_indexed_db.strong_ref())
        return *indexed_db;

    m_indexed_db = devtools().register_actor<IndexedDBActor>(m_tab);
    return *m_indexed_db.strong_ref();
}

void WatcherActor::send_cookies_resource_available_message()
{
    JsonArray cookies;
    cookies.must_append(cookies_actor().serialize_storage());

    JsonArray cookie_resources;
    cookie_resources.must_append("cookies"sv);
    cookie_resources.must_append(move(cookies));

    JsonArray array;
    array.must_append(move(cookie_resources));

    JsonObject message;
    message.set("type"sv, "resources-available-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));
}

StorageActor& WatcherActor::local_storage_actor()
{
    if (auto storage = m_local_storage.strong_ref())
        return *storage;

    m_local_storage = devtools().register_actor<StorageActor>(m_tab, Web::StorageAPI::StorageEndpointType::LocalStorage);
    return *m_local_storage.strong_ref();
}

StorageActor& WatcherActor::session_storage_actor()
{
    if (auto storage = m_session_storage.strong_ref())
        return *storage;

    m_session_storage = devtools().register_actor<StorageActor>(m_tab, Web::StorageAPI::StorageEndpointType::SessionStorage);
    return *m_session_storage.strong_ref();
}

void WatcherActor::send_storage_resource_available_message(StorageActor& storage)
{
    JsonArray resources;
    resources.must_append(storage.serialize_storage());

    JsonArray typed_resources;
    typed_resources.must_append(storage.resource_type());
    typed_resources.must_append(move(resources));

    JsonArray array;
    array.must_append(move(typed_resources));

    JsonObject message;
    message.set("type"sv, "resources-available-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));
}

void WatcherActor::send_indexed_db_resource_available_message()
{
    indexed_db_actor().get_storage_resource([weak_self = make_weak_ptr<WatcherActor>()](JsonObject indexed_db) mutable {
        auto self = weak_self.strong_ref();
        if (!self)
            return;

        JsonArray indexed_databases;
        indexed_databases.must_append(move(indexed_db));

        JsonArray indexed_database_resources;
        indexed_database_resources.must_append("indexed-db"sv);
        indexed_database_resources.must_append(move(indexed_databases));

        JsonArray array;
        array.must_append(move(indexed_database_resources));

        JsonObject message;
        message.set("type"sv, "resources-available-array"sv);
        message.set("array"sv, move(array));
        self->send_message(move(message));
    });
}

}
