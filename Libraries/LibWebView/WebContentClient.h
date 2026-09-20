/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/SourceLocation.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/SharedImage.h>
#include <LibHTTP/Header.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibRequests/CameFromCache.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/PageId.h>
#include <LibWeb/Page/ScreenWakeLockHandle.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Debugger.h>
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
    friend class WebContentPage;

public:
    using InitTransport = Messages::WebContentServer::InitTransport;

    template<CallableAs<IterationDecision, WebContentClient&> Callback>
    static void for_each_client(Callback callback);

    static size_t client_count() { return clients().size(); }
    static Optional<WebContentClient&> client_for_compositor_context_id(Web::Compositor::CompositorContextId);

    virtual Messages::WebContentClient::OpenSystemFontResponse open_system_font(u64 generation, u64 face_id) override;
    virtual Messages::WebContentClient::MatchSystemFontResponse match_system_font(String family, u16 weight, u16 width, u8 slope) override;
    virtual Messages::WebContentClient::MatchLocalFontResponse match_local_font(String name) override;
    virtual Messages::WebContentClient::MatchSystemFontForCodePointResponse match_system_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) override;
    virtual Messages::WebContentClient::ResolveGenericFontResponse resolve_generic_font(String family, u16 weight, u8 slope) override;
    virtual Messages::WebContentClient::DidAddBlobUrlEntryResponse did_add_blob_url_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry) override;
    virtual void did_remove_blob_url_entries(Vector<Utf16String> urls, URL::Origin origin) override;
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

    void assign_view(Badge<Application>, ViewImplementation&);
    void set_initial_top_level_history_entry(Badge<Application>, Web::HTML::SessionHistoryEntryDescriptor entry) { m_initial_top_level_history_entry = move(entry); }
    void register_view(Web::PageId page_id, ViewImplementation&);
    void unregister_view(Web::PageId page_id);

    void set_compositor_connection_id(Badge<Application>, i32);
    Optional<i32> compositor_connection_id(Badge<Application>) const { return m_compositor_connection_id; }

    void prepare_for_detached_close(Web::PageId page_id);
    void request_close(Web::PageId page_id);

    void web_ui_disconnected(Badge<WebUI>);
    void register_embedded_page(Web::PageId page_id, CanonicalTraversable&);
    void unregister_embedded_page(Web::PageId page_id);
    void keep_view_page_for_displaced_document(Web::PageId page_id, CanonicalTraversable&);
    Optional<Web::PageId> page_id_for_traversable(CanonicalTraversable const&) const;
    bool holds_part_of_a_tab_opened_by(CanonicalTraversable const&);
    void release_unneeded_opener_pages();

    WebContentPage* page(Web::PageId page_id) const;
    bool is_page_open(Web::PageId page_id) const { return !m_process_lost && page(page_id); }
    Optional<ViewImplementation&> display_view(Web::PageId page_id) const;
    template<CallableAs<IterationDecision, WebContentPage&> Callback>
    void for_each_page(Callback);

    Optional<CanonicalNavigable&> hosted_navigable(Web::HTML::CrossProcessId navigable_id);
    // True for every page ID the UI process has handed to this connection, closed pages included, since a
    // message the connection sent while it had the page can arrive after the page is gone.
    virtual bool may_act_for_page(Web::PageId page_id) const override;

    Optional<u64> exclusive_performance_owner() const;

    bool has_views() const;

    void notify_all_views_of_crash();
    ErrorOr<void> reconnect_to_compositor_process(Badge<Application>);
    ErrorOr<void> recreate_compositor_contexts(Badge<Application>);
    void replay_compositor_view_state_after_reconnect(Badge<Application>);
    void notify_compositor_process_reconnected(Badge<Application>);
    Web::Compositor::CompositorContextId compositor_context_id_for_page(Web::PageId page_id);
    Optional<Web::PageId> page_id_for_compositor_context_id(Web::Compositor::CompositorContextId) const;
    bool send_async_scroll_to_compositor(Web::PageId page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Web::WheelDeltaPrecision, Web::ScrollGesturePhase);
    bool handle_mouse_event_in_compositor(Web::PageId page_id, Web::MouseEvent const&);
    bool handle_mouse_event_in_compositor(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId>, Web::MouseEvent const&);
    bool handle_key_event_in_compositor(Web::PageId page_id, Web::KeyEvent const&);
    void dispatch_key_event_to_web_content(Web::PageId page_id, Web::KeyEvent const&);
    bool handle_pinch_event_in_compositor(Web::PageId page_id, Web::PinchEvent const&);
    void dispatch_mouse_event_to_web_content(Web::PageId page_id, Web::MouseEvent const&);
    void dispatch_mouse_event_to_web_content(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId>, Web::MouseEvent const&);
    void notify_presented_bitmap_ready_to_paint(Web::PageId page_id, i32 bitmap_id);
    void did_present_backing_stores(Web::PageId page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores);
    void did_present_bitmap(Web::PageId page_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id);
    void close_if_unused(Badge<CanonicalNavigable>) { close_server_if_unused(); }

    pid_t pid() const { return m_process_handle.pid; }
    void set_pid(pid_t pid) { m_process_handle.pid = pid; }

private:
    void close_server_if_unused();
    bool forget_compositor_context(Web::Compositor::CompositorContextId);
    void destroy_all_compositor_contexts();
    void cancel_navigation_transactions();
    static bool renderers_may_access_cookies_like_http();

    virtual WebContentClientPageStub* page_stub(Web::PageId const& page_id) override { return page(page_id); }
    virtual void did_misbehave(StringView message_name, StringView reason) override;

    virtual void die() override;

    virtual Messages::WebContentClient::AllocateCompositorContextIdResponse allocate_compositor_context_id(Web::PageId page_id, Web::Compositor::PagePresentationRegistration) override;
    virtual void did_destroy_compositor_context(Web::Compositor::CompositorContextId) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesWebdriverResponse did_request_all_cookies_webdriver(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse did_request_all_cookies_cookiestore(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestNamedCookieResponse did_request_named_cookie(URL::URL, String) override;
    virtual Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(Web::PageId page_id, URL::URL, HTTP::Cookie::Source) override;
    virtual void did_close_browsing_context(Web::PageId page_id) override;
    virtual Messages::WebContentClient::DidSetStorageItemResponse did_set_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, String storage_key, Utf16String bottle_key, Utf16String value) override;
    virtual Messages::WebContentClient::DidRequestStorageItemResponse did_request_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, String storage_key, Utf16String bottle_key) override;
    virtual Messages::WebContentClient::DidRequestStorageKeysResponse did_request_storage_keys(Web::PageId page_id, Web::StorageAPI::StorageEndpointType, String storage_key) override;
    virtual Messages::WebContentClient::DidRequestStorageUsageResponse did_request_storage_usage(Web::PageId page_id, String storage_key) override;
    virtual Messages::WebContentClient::DidStartDownloadWithoutRequestResponse did_start_download_without_request(Web::PageId page_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size) override;
    virtual Messages::WebContentClient::DidStartDownloadResponse did_start_download(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data) override;
    virtual Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(Web::PageId page_id, Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name) override;
    virtual Messages::WebContentClient::StartWorkerAgentResponse start_worker_agent(Web::PageId page_id, Web::HTML::WorkerAgentStartRequest request) override;
    virtual void did_set_cookie(URL::URL, HTTP::Cookie::ParsedCookie, HTTP::Cookie::Source) override;
    virtual void did_update_cookie(HTTP::Cookie::Cookie) override;
    virtual Messages::WebContentClient::DidIsKnownHstsHostResponse did_is_known_hsts_host(String) override;
    virtual Messages::WebContentClient::DidLoseRequestServerConnectionResponse did_lose_request_server_connection() override;

    void remember_compositor_context(Web::Compositor::CompositorContextId, Optional<Web::PageId> page_id);
    void remember_renderer_owned_download(u64 download_id, Web::PageId page_id) { m_renderer_owned_downloads.set(download_id, page_id); }
    bool is_renderer_owned_download(Web::PageId page_id, u64 download_id) const;
    void forget_renderer_owned_download(u64 download_id);
    void fail_renderer_owned_downloads();

    RefPtr<WebContentTestClient> m_test_connection;

    IsPrivate m_is_private { IsPrivate::No };
    RefPtr<BrowsingSession> m_session;
    bool m_process_lost { false };
    bool m_rejected_ipc { false };

    WebContentPage& open_page(Web::PageId, CanonicalTraversable&);
    WebContentPage* find_page(Web::PageId) const;

    // Every page ID the UI process has handed to this connection. A page stays in the map once it closes,
    // because messages the connection sent while it had the page can arrive after the page is gone.
    HashMap<Web::PageId, NonnullRefPtr<WebContentPage>> m_pages;
    HashMap<Web::Compositor::CompositorContextId, Optional<Web::PageId>> m_compositor_contexts;
    HashMap<u64, Web::PageId> m_renderer_owned_downloads;
    Optional<i32> m_compositor_connection_id;
    Optional<Web::PageId> m_unassigned_initial_page_id;
    Web::HTML::CrossProcessId m_root_navigable_id;
    Optional<Web::HTML::SessionHistoryEntryDescriptor> m_initial_top_level_history_entry;

    ProcessHandle m_process_handle;
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
