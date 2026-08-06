/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API WatcherActor final : public Actor {
public:
    static constexpr auto base_name = "watcher"sv;

    static NonnullRefPtr<WatcherActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~WatcherActor() override;
    virtual void connection_closed() override;

    JsonObject serialize_description() const;
    void send_frame_target_available_message();
    void switch_frame_target(FrameActor&, String const& url, String const& title);
    void start_watching_thread_state_resources();
    void send_thread_state_available_message(JsonObject);
    void update_debugger_blackboxing(String const&, Vector<WebView::DebuggerBlackboxRange>, WebView::DebuggerBlackboxingOperation);
    bool is_source_fully_blackboxed(StringView) const;
    bool is_paused_in_source(Web::HTML::ScriptRegistry::Identifier) const;

private:
    WatcherActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    FrameActor& create_frame_target();
    void send_frame_target_available_message(FrameActor&);
    void send_frame_target_destroyed_message(FrameActor&);
    BlackboxingActor& blackboxing_actor();
    BreakpointListActor& breakpoint_list_actor();
    CookiesActor& cookies_actor();
    void send_cookies_resource_available_message();
    void start_watching_source_resources();
    void stop_watching_source_resources();
    void send_source_resource_available_message();
    void send_source_resource_available_message(Web::HTML::ScriptRegistry::Description const&);
    void attach_debugger_if_possible();
    void stop_watching_thread_state_resources();
    StorageActor& local_storage_actor();
    StorageActor& session_storage_actor();
    void send_storage_resource_available_message(StorageActor&);
    IndexedDBActor& indexed_db_actor();
    void send_indexed_db_resource_available_message();

    WeakPtr<TabActor> m_tab;
    WeakPtr<FrameActor> m_target;
    WeakPtr<BlackboxingActor> m_blackboxing;
    WeakPtr<BreakpointListActor> m_breakpoint_list;
    WeakPtr<CookiesActor> m_cookies;
    WeakPtr<IndexedDBActor> m_indexed_db;
    WeakPtr<StorageActor> m_local_storage;
    WeakPtr<StorageActor> m_session_storage;
    WeakPtr<ThreadActor> m_thread;
    WeakPtr<TargetConfigurationActor> m_target_configuration;
    WeakPtr<ThreadConfigurationActor> m_thread_configuration;
    WeakPtr<NetworkParentActor> m_network_parent;
    HashMap<String, WebView::DebuggerBlackboxState> m_blackboxed_sources;
    bool m_is_watching_frame_targets { false };
    bool m_is_watching_cookie_resources { false };
    bool m_is_watching_indexed_db_resources { false };
    bool m_is_watching_local_storage_resources { false };
    bool m_is_watching_session_storage_resources { false };
    bool m_is_watching_source_resources { false };
    bool m_is_watching_thread_state_resources { false };
    bool m_debugger_is_attached { false };
    bool m_connection_closed { false };
};

}
