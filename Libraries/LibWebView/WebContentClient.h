/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/SourceLocation.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/WeakPtr.h>
#include <LibCompositing/Types.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/SharedImage.h>
#include <LibHTTP/Header.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWebCommon/Bindings/Navigation.h>
#include <LibWebCommon/CSS/StyleSheetIdentifier.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebCommon/HTML/ApplyHistoryStep.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/HistoryHandlingBehavior.h>
#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebCommon/HTML/SameDocumentNavigationEntry.h>
#include <LibWebCommon/HTML/Scripting/ScriptRegistryTypes.h>
#include <LibWebCommon/HTML/SelectItem.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/HTML/WebViewHints.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <LibWebCommon/Page/EventResult.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Page/ScreenWakeLockState.h>
#include <LibWebCommon/Page/ViewportIsFullscreen.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>
#include <LibWebCommon/WebView/Debugger.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebContentPage.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace WebView {

class ViewImplementation;

class WEBVIEW_API WebContentClient final
    : public WebContentClientPageRoutingStub<IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>>
    , public WebContentClientEndpoint {
    C_OBJECT_ABSTRACT(WebContentClient);

    friend class WebContentTestClient;

public:
    using InitTransport = Messages::WebContentServer::InitTransport;

    template<CallableAs<IterationDecision, WebContentClient&> Callback>
    static void for_each_client(Callback callback);

    static size_t client_count() { return clients().size(); }
    static Optional<WebContentClient&> client_for_compositor_context_id(Web::CompositorContextId);

    virtual Messages::WebContentClient::DidAddBlobUrlEntryResponse did_add_blob_url_entry(Web::PageId page_id, Web::HTML::EnvironmentId environment_id, Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry) override;
    virtual void did_retain_blob_url_token(Web::HTML::CrossProcessId navigable_id, URL::BlobURLEntry::Token token) override;
    virtual Messages::WebContentClient::DidRequestBlobUrlEntryResponse did_request_blob_url_entry(Utf16String url, Optional<URL::BlobURLEntry::Token> token) override;

    WebContentClient(NonnullOwnPtr<IPC::Transport>, IsPrivate, Web::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id);
    ~WebContentClient();

    IsPrivate is_private() const { return m_is_private; }
    BrowsingSession& session() const { return *m_session; }
    void remove_blob_url_entries();

    void connect_test_endpoint(NonnullOwnPtr<IPC::Transport>);
    // Null outside test mode: the test endpoint is only connected when the UI process runs tests.
    WebContentTestClient* test_connection() { return m_test_connection; }

    void set_initial_top_level_history_entry(Badge<Application>, Web::HTML::SessionHistoryEntryDescriptor entry) { m_initial_top_level_history_entry = move(entry); }
    WebContentPage& open_initial_page_for_new_top_level_traversable();
    WebContentPage& open_page_for_new_top_level_traversable(Web::PageId, CanonicalTraversable&);
    void discard_page_of_undisplayed_top_level_traversable(Web::PageId);
    void close_page_of_closed_tab(Web::PageId page_id);

    void set_compositor_connection_id(Badge<Application>, i32);
    Optional<i32> compositor_connection_id(Badge<Application>) const { return m_compositor_connection_id; }

    void web_ui_disconnected(Badge<WebUI>);
    void set_web_ui(RefPtr<WebUI>);
    virtual void did_misbehave(StringView message_name, StringView reason) override;
    static bool renderers_may_access_cookies_like_http();
    bool hosts_an_environment_that_may_use_cookies_of(URL::URL const&) const;
    void register_embedded_page(Web::PageId page_id, CanonicalTraversable&);
    void unregister_embedded_page(Web::PageId page_id);
    Optional<Web::PageId> page_id_for_traversable(CanonicalTraversable const&) const;
    bool holds_part_of_a_tab_in_the_group_of(CanonicalTraversable const&);
    void release_unneeded_representing_pages();
    bool hosts_an_environment_with_storage_key(Web::StorageAPI::StorageKey const&);
    Optional<CanonicalEnvironmentSettingsObject const&> hosted_environment(Web::HTML::EnvironmentId const& environment_id);

    WebContentPage* page(Web::PageId page_id) const;
    template<CallableAs<IterationDecision, WebContentPage&> Callback>
    void for_each_page(Callback);

    Optional<CanonicalNavigable&> hosted_navigable(Web::HTML::CrossProcessId navigable_id);
    // True for every page ID the UI process has handed to this connection, closed pages included, since a
    // message the connection sent while it had the page can arrive after the page is gone.
    virtual bool may_act_for_page(Web::PageId page_id) const override;

    Optional<u64> exclusive_performance_owner() const;

    void did_lose_process();
    void did_save_crash_report(ByteString const& report_name);
    // Whether a tab was told this process was lost, and so shows its crash.
    bool has_crashed_views() const { return !m_crashed_view_ids.is_empty(); }
    ErrorOr<void> reconnect_to_compositor_process(Badge<Application>);
    ErrorOr<void> recreate_compositor_contexts(Badge<Application>);
    void replay_compositor_view_state_after_reconnect(Badge<Application>);
    void notify_compositor_process_reconnected(Badge<Application>);
    Web::CompositorContextId compositor_context_id_for_page(WebContentPage const&);
    Web::CompositorContextId allocate_compositor_context(Web::PageId page_id, Web::PagePresentationRegistration);
    Optional<Web::PageId> page_id_for_compositor_context_id(Web::CompositorContextId) const;
    void close_if_unused(Badge<CanonicalNavigable>) { close_server_if_unused(); }
    bool has_requested_close() const { return m_requested_close; }

    pid_t pid() const { return m_process_handle.pid; }
    void set_pid(pid_t pid) { m_process_handle.pid = pid; }

private:
    void close_server_if_unused();
    bool forget_compositor_context(Web::CompositorContextId);
    void destroy_all_compositor_contexts();
    void cancel_navigation_transactions();

    virtual WebContentClientPageStub* page_stub(Web::PageId const& page_id) override { return page(page_id); }

    virtual void die() override;

    virtual Messages::WebContentClient::AllocateCompositorContextIdResponse allocate_compositor_context_id(Web::PageId page_id, Web::PagePresentationRegistration) override;
    virtual void did_destroy_compositor_context(Web::CompositorContextId) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesWebdriverResponse did_request_all_cookies_webdriver(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse did_request_all_cookies_cookiestore(Web::PageId page_id, URL::URL) override;
    virtual Messages::WebContentClient::DidRequestNamedCookieResponse did_request_named_cookie(URL::URL, String) override;
    virtual Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(Web::PageId page_id, URL::URL, HTTP::Cookie::Source) override;
    virtual void did_close_browsing_context(Web::PageId page_id) override;
    virtual Messages::WebContentClient::DidSetStorageItemResponse did_set_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, Web::HTML::EnvironmentId environment_id, Utf16String bottle_key, Utf16String value) override;
    virtual Messages::WebContentClient::DidRequestStorageItemResponse did_request_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, Web::HTML::EnvironmentId environment_id, Utf16String bottle_key) override;
    virtual Messages::WebContentClient::DidRequestStorageKeysResponse did_request_storage_keys(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, Web::HTML::EnvironmentId environment_id) override;
    virtual Messages::WebContentClient::DidRequestStorageUsageResponse did_request_storage_usage(Web::PageId page_id, Web::HTML::EnvironmentId environment_id) override;
    virtual Messages::WebContentClient::DidStartDownloadWithoutRequestResponse did_start_download_without_request(Web::PageId page_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size) override;
    virtual Messages::WebContentClient::DidStartDownloadResponse did_start_download(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data) override;
    virtual Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(Web::PageId page_id, Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name, Web::HTML::SandboxingFlagSet popup_sandboxing_flag_set) override;
    virtual Messages::WebContentClient::StartWorkerAgentResponse start_worker_agent(Web::PageId page_id, Web::HTML::WorkerAgentStartRequest request) override;
    virtual void did_update_cookie(HTTP::Cookie::Cookie) override;
    virtual Messages::WebContentClient::DidIsKnownHstsHostResponse did_is_known_hsts_host(String) override;
    virtual Messages::WebContentClient::DidLoseRequestServerConnectionResponse did_lose_request_server_connection() override;
    virtual Messages::WebContentClient::RequestMediaServerConnectionResponse request_media_server_connection() override;
    virtual void did_start_using_gamepads() override;
    virtual void gamepad_play_effect(Web::Gamepad::GamepadHandle handle, Web::Gamepad::GamepadEffect effect) override;
    virtual void gamepad_stop_effects(Web::Gamepad::GamepadHandle handle) override;

    void remember_compositor_context(Web::CompositorContextId, Optional<Web::PageId> page_id);
    void fail_renderer_owned_downloads();

    RefPtr<WebContentTestClient> m_test_connection;

    IsPrivate m_is_private { IsPrivate::No };
    RefPtr<BrowsingSession> m_session;
    bool m_requested_close { false };
    bool m_rejected_ipc { false };
    Vector<u64> m_crashed_view_ids;

    WebContentPage& open_page(Web::PageId, CanonicalTraversable&);
    WebContentPage* find_page(Web::PageId) const;

    // Every page ID the UI process has handed to this connection. A page stays in the map once it closes,
    // because messages the connection sent while it had the page can arrive after the page is gone.
    HashMap<Web::PageId, NonnullRefPtr<WebContentPage>> m_pages;
    HashMap<Web::CompositorContextId, Optional<Web::PageId>> m_compositor_contexts;
    Optional<i32> m_compositor_connection_id;
    Optional<Web::PageId> m_unassigned_initial_page_id;
    Web::HTML::CrossProcessId m_root_navigable_id;
    Optional<Web::HTML::SessionHistoryEntryDescriptor> m_initial_top_level_history_entry;

    ProcessHandle m_process_handle;

    // The controller connection to the MediaServer spawned for this process, from its first media use until it exits.
    RefPtr<MediaClient::Client> m_media_server_client;
    RefPtr<Core::Timer> m_detached_page_close_timer;

    RefPtr<WebUI> m_web_ui;

    static HashTable<WebContentClient*>& clients();
};

template<CallableAs<IterationDecision, WebContentPage&> Callback>
void WebContentClient::for_each_page(Callback callback)
{
    for (auto const& [page_id, page] : m_pages) {
        if (!page->is_open())
            continue;
        if (callback(*page) == IterationDecision::Break)
            return;
    }
}

template<CallableAs<IterationDecision, WebContentClient&> Callback>
void WebContentClient::for_each_client(Callback callback)
{
    for (auto& it : clients()) {
        if (callback(*it) == IterationDecision::Break)
            return;
    }
}

}
