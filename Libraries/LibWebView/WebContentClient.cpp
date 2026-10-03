/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/WeakPtr.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibRequests/Request.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/WebDriver/Error.h>
#include <LibWebCommon/WebView/ProcessHandle.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentTestClient.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

Messages::WebContentClient::DidAddBlobUrlEntryResponse WebContentClient::did_add_blob_url_entry(Web::PageId page_id, Web::HTML::EnvironmentId environment_id, Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry)
{
    if (auto* page = this->page(page_id))
        return page->did_add_blob_url_entry(move(environment_id), move(url), move(entry));

    return URL::BlobURLEntry::Token { 0 };
}

bool WebContentClient::hosts_an_environment_with_storage_key(Web::StorageAPI::StorageKey const& storage_key)
{
    bool hosts_one = false;
    for_each_page([&](WebContentPage& page) {
        hosts_one = page.hosts_an_environment_with_storage_key(storage_key);
        return hosts_one ? IterationDecision::Break : IterationDecision::Continue;
    });
    return hosts_one;
}

Optional<CanonicalEnvironmentSettingsObject const&> WebContentClient::hosted_environment(Web::HTML::EnvironmentId const& environment_id)
{
    Optional<CanonicalEnvironmentSettingsObject const&> environment;
    for_each_page([&](WebContentPage& page) {
        environment = page.hosted_environment(environment_id);
        return environment.has_value() ? IterationDecision::Break : IterationDecision::Continue;
    });
    return environment;
}

void WebContentClient::did_retain_blob_url_token(Web::HTML::CrossProcessId navigable_id, URL::BlobURLEntry::Token token)
{
    if (auto navigable = hosted_navigable(navigable_id); navigable.has_value())
        navigable->retain_blob_url_token(token);
}

Messages::WebContentClient::DidRequestBlobUrlEntryResponse WebContentClient::did_request_blob_url_entry(Utf16String url, Optional<URL::BlobURLEntry::Token> token)
{
    return m_session->blob_url_store->resolve(url, token);
}

void WebContentClient::connect_test_endpoint(NonnullOwnPtr<IPC::Transport> transport)
{
    m_test_connection = make_ref_counted<WebContentTestClient>(move(transport), *this);
}

void WebContentClient::remove_blob_url_entries()
{
    m_session->blob_url_store->remove_entries_added_by(WeakPtr<WebContentClient> { *this });
}

HashTable<WebContentClient*>& WebContentClient::clients()
{
    static NeverDestroyed<HashTable<WebContentClient*>> clients;
    return *clients;
}

static constexpr auto detached_page_close_timeout_ms = 1000;
static constexpr auto close_server_exit_timeout_ms = 5000;
static constexpr auto detached_page_forced_exit_timeout_ms = detached_page_close_timeout_ms + close_server_exit_timeout_ms;

WebContentClient::WebContentClient(NonnullOwnPtr<IPC::Transport> transport, IsPrivate is_private, Web::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id)
    : WebContentClientPageRoutingStub(*this, move(transport))
    , m_is_private(is_private)
    , m_session(Application::existing_session(is_private))
    , m_unassigned_initial_page_id(initial_page_id)
    , m_root_navigable_id(root_navigable_id)
{
    VERIFY(initial_page_id > 0);
    VERIFY(m_session);
    clients().set(this);
}

WebContentClient::~WebContentClient()
{
    // The tree can still hold a page of a process that is gone; it reads as closed from now on.
    for (auto& page : m_pages)
        page.value->close();
    cancel_navigation_transactions();
    remove_blob_url_entries();
    WorkerProcessManager::the().remove_web_content_owner(*this);
    GamepadManager::the().client_disconnected(*this);
    clients().remove(this);
}

Optional<WebContentClient&> WebContentClient::client_for_compositor_context_id(Web::CompositorContextId context_id)
{
    Optional<WebContentClient&> client;
    for_each_client([&](auto& candidate) {
        if (!candidate.page_id_for_compositor_context_id(context_id).has_value())
            return IterationDecision::Continue;
        client = candidate;
        return IterationDecision::Break;
    });
    return client;
}

void WebContentClient::did_start_using_gamepads()
{
    GamepadManager::the().client_did_start_using_gamepads(*this);
}

void WebContentClient::gamepad_play_effect(Web::Gamepad::GamepadHandle handle, Web::Gamepad::GamepadEffect effect)
{
    GamepadManager::the().play_effect(*this, handle, effect);
}

void WebContentClient::gamepad_stop_effects(Web::Gamepad::GamepadHandle handle)
{
    GamepadManager::the().stop_effects(*this, handle);
}

void WebContentClient::die()
{
    did_lose_process();
}

void WebContentClient::did_misbehave(StringView message_name, StringView reason)
{
    m_rejected_ipc = true;
    dbgln("WebContentClient: terminating helper process {}: {} rejected: {}", pid(), message_name, reason);
    if (should_terminate_pid(pid()))
        (void)Core::Process::terminate_process(pid(), Core::Process::TerminationMode::Forceful);
    shutdown();
}

// Only an open page registers its context: the UI process forgets a closed page's context while the process still
// holds it, and a request the process sent before the close can arrive after it.
Web::CompositorContextId WebContentClient::compositor_context_id_for_page(WebContentPage const& page)
{
    VERIFY(page.is_open());
    auto page_id = page.id();
    auto context_id = Web::compositor_context_id_for_page(page_id);
    if (auto registered_page_id = m_compositor_contexts.get(context_id); registered_page_id.has_value()) {
        if (!registered_page_id->has_value() || **registered_page_id != page_id) {
            did_misbehave("allocate_compositor_context_id"sv, "page ID collides with an existing compositor context"sv);
            return context_id;
        }
        return context_id;
    }

    remember_compositor_context(context_id, page_id);
    Application::the().register_compositor_context(*this, context_id, page_id);
    return context_id;
}

Optional<Web::PageId> WebContentClient::page_id_for_compositor_context_id(Web::CompositorContextId context_id) const
{
    auto page_id = m_compositor_contexts.get(context_id);
    if (!page_id.has_value())
        return {};
    return *page_id;
}

Messages::WebContentClient::AllocateCompositorContextIdResponse WebContentClient::allocate_compositor_context_id(Web::PageId page_id, Web::PagePresentationRegistration page_presentation_registration)
{
    return allocate_compositor_context(page_id, page_presentation_registration);
}

Web::CompositorContextId WebContentClient::allocate_compositor_context(Web::PageId page_id, Web::PagePresentationRegistration page_presentation_registration)
{
    if (page_presentation_registration == Web::PagePresentationRegistration::Yes) {
        if (auto* page = this->page(page_id))
            return compositor_context_id_for_page(*page);
        return Web::compositor_context_id_for_page(page_id);
    }

    auto context_id = Application::the().allocate_compositor_context_id();
    remember_compositor_context(context_id, {});
    Application::the().register_compositor_context(*this, context_id, {});
    return context_id;
}

void WebContentClient::did_destroy_compositor_context(Web::CompositorContextId context_id)
{
    forget_compositor_context(context_id);
}

bool WebContentClient::forget_compositor_context(Web::CompositorContextId context_id)
{
    if (!m_compositor_contexts.remove(context_id))
        return false;
    return true;
}

void WebContentClient::remember_compositor_context(Web::CompositorContextId context_id, Optional<Web::PageId> page_id)
{
    m_compositor_contexts.set(context_id, page_id);
}

WebContentPage& WebContentClient::open_initial_page_for_new_top_level_traversable()
{
    auto initial_page_id = m_unassigned_initial_page_id.release_value();

    // A tab's first process creates its traversable, in the page that displays the tab.
    VERIFY(m_initial_top_level_history_entry.has_value());
    auto& traversable = CanonicalTraversable::create_a_new_top_level_traversable(m_root_navigable_id, {}, m_initial_top_level_history_entry.release_value());
    auto& page = open_page_for_new_top_level_traversable(initial_page_id, traversable);
    page.async_set_browsing_context_group(traversable.active_browsing_context().group()->id());
    return page;
}

void WebContentClient::set_compositor_connection_id(Badge<Application>, i32 compositor_connection_id)
{
    m_compositor_connection_id = compositor_connection_id;
}

WebContentPage& WebContentClient::open_page_for_new_top_level_traversable(Web::PageId page_id, CanonicalTraversable& traversable)
{
    if (m_detached_page_close_timer)
        m_detached_page_close_timer->stop();
    Application::process_manager().cancel_forced_exit(pid());

    auto& page = open_page(page_id, traversable);
    traversable.active_document().set_host(page);
    return page;
}

// The process never learns of a page no view displays, so it can make no claim for it.
void WebContentClient::discard_page_of_undisplayed_top_level_traversable(Web::PageId page_id)
{
    if (auto page = m_pages.take(page_id); page.has_value())
        page.value()->close();
    release_unneeded_representing_pages();
}

void WebContentClient::close_page_of_closed_tab(Web::PageId page_id)
{
    if (auto* page = this->page(page_id))
        page->traversable().remove_page(*page);

    if (auto* page = find_page(page_id)) {
        // A page that still needs a beforeunload check is not a detached
        // background close. It is being closed without waiting for WebContent,
        // e.g. because the user requested a forced close.
        if (page->needs_beforeunload_check())
            page->set_detached_close_pending(false);
        page->close();
    }
    // Forgotten once the page is closed, so nothing registers it again.
    forget_compositor_context(Web::compositor_context_id_for_page(page_id));
    release_unneeded_representing_pages();
    close_server_if_unused();
}

void WebContentClient::fail_renderer_owned_downloads()
{
    for (auto& page : m_pages)
        page.value->fail_renderer_owned_downloads();
}

void WebContentClient::register_embedded_page(Web::PageId page_id, CanonicalTraversable& traversable)
{
    auto& page = open_page(page_id, traversable);
    if (m_unassigned_initial_page_id == page_id)
        m_unassigned_initial_page_id.clear();
    Application::process_manager().cancel_forced_exit(pid());

    page.view().send_preferences_to_page(page);
    if (Application::browser_options().webdriver_browser_endpoint.has_value())
        Application::the().push_webdriver_session_config(page);
    page.async_set_has_focus(traversable.has_system_focus());
    page.async_set_viewport_is_fullscreen(page.view().is_fullscreen());
    if (auto focused_navigable_id = traversable.focused_navigable_id(); focused_navigable_id.has_value())
        page.async_set_focused_navigable(*focused_navigable_id);
}

Optional<Web::PageId> WebContentClient::page_id_for_traversable(CanonicalTraversable const& traversable) const
{
    for (auto const& [page_id, page] : m_pages) {
        if (page->is_open() && &page->traversable() == &traversable)
            return page_id;
    }
    return {};
}

void WebContentClient::unregister_embedded_page(Web::PageId page_id)
{
    if (auto* page = find_page(page_id); page && !(page->is_open() && page->displays_tab()))
        page->close();
    release_unneeded_representing_pages();
    close_server_if_unused();
}

// Whether a page of this process holds part of a tab in the browsing context group of the traversable's tab.
bool WebContentClient::holds_part_of_a_tab_in_the_group_of(CanonicalTraversable const& traversable)
{
    auto group = traversable.active_browsing_context().group();
    if (!group)
        return false;
    for (auto const& [page_id, page] : m_pages) {
        if (!page->is_open())
            continue;
        auto const& held = page->traversable();
        if (!page->displays_tab() && !held.page_hosts_any(*page))
            continue;
        if (held.active_browsing_context().group() == group)
            return true;
    }
    return false;
}

void WebContentClient::release_unneeded_representing_pages()
{
    Vector<NonnullRefPtr<WebContentPage>> representing_pages;
    for_each_page([&](WebContentPage& page) {
        if (!page.displays_tab() && page.traversable().is_representing_page(page))
            representing_pages.append(page);
        return IterationDecision::Continue;
    });
    for (auto const& page : representing_pages) {
        if (page->is_open())
            page->traversable().release_page_if_unused(page);
    }
}

WebContentPage& WebContentClient::open_page(Web::PageId page_id, CanonicalTraversable& traversable)
{
    auto page = adopt_ref(*new WebContentPage(*this, page_id, traversable));
    m_pages.set(page_id, page);
    return page;
}

WebContentPage* WebContentClient::find_page(Web::PageId page_id) const
{
    auto page = m_pages.find(page_id);
    if (page == m_pages.end())
        return nullptr;
    return page->value.ptr();
}

WebContentPage* WebContentClient::page(Web::PageId page_id) const
{
    auto* page = find_page(page_id);
    if (!page || !page->is_open())
        return nullptr;
    return page;
}

bool WebContentClient::may_act_for_page(Web::PageId page_id) const
{
    // A page ID the connection was given is not a false claim once the page is gone: the connection can
    // have sent the message while it still had the page, and the handler drops it.
    return m_pages.contains(page_id) || m_unassigned_initial_page_id == page_id;
}

Optional<CanonicalNavigable&> WebContentClient::hosted_navigable(Web::HTML::CrossProcessId navigable_id)
{
    Optional<CanonicalNavigable&> result;
    for_each_page([&](WebContentPage& page) {
        result = page.hosted_navigable(navigable_id);
        return result.has_value() ? IterationDecision::Break : IterationDecision::Continue;
    });
    return result;
}

void WebContentClient::close_server_if_unused()
{
    bool any_detached_close_pending = false;
    for (auto const& page : m_pages) {
        if (page.value->is_open())
            return;
        any_detached_close_pending |= page.value->detached_close_pending();
    }

    if (!any_detached_close_pending) {
        if (m_detached_page_close_timer)
            m_detached_page_close_timer->stop();
        m_requested_close = true;
        async_close_server();
        Application::process_manager().force_exit_after_timeout(pid(), close_server_exit_timeout_ms);
        return;
    }

    Application::process_manager().force_exit_after_timeout(pid(), detached_page_forced_exit_timeout_ms);

    if (!m_detached_page_close_timer) {
        m_detached_page_close_timer = Core::Timer::create_single_shot(detached_page_close_timeout_ms, [this] {
            dbgln("Timed out waiting for detached WebContent page close acknowledgement");
            for (auto& page : m_pages)
                page.value->set_detached_close_pending(false);
            close_server_if_unused();
        });
    }

    if (!m_detached_page_close_timer->is_active())
        m_detached_page_close_timer->start();
}

void WebContentClient::set_web_ui(RefPtr<WebUI> web_ui)
{
    m_web_ui = move(web_ui);
}

void WebContentClient::web_ui_disconnected(Badge<WebUI>)
{
    m_web_ui.clear();
}

void WebContentClient::destroy_all_compositor_contexts()
{
    m_compositor_contexts.clear();
}

ErrorOr<void> WebContentClient::reconnect_to_compositor_process(Badge<Application>)
{
    if (!is_open())
        return {};

    m_compositor_connection_id.clear();
    TRY(Application::the().connect_web_content_to_compositor(*this));
    return {};
}

ErrorOr<void> WebContentClient::recreate_compositor_contexts(Badge<Application>)
{
    if (!is_open())
        return {};

    for (auto const& [context_id, page_id] : m_compositor_contexts)
        TRY(Application::the().try_register_compositor_context(*this, context_id, page_id));

    return {};
}

void WebContentClient::replay_compositor_view_state_after_reconnect(Badge<Application>)
{
    if (!is_open())
        return;

    for (auto& [page_id, page] : m_pages) {
        if (!page->is_open() || !page->displays_tab())
            continue;
        auto context_id = Web::compositor_context_id_for_page(page_id);
        if (!m_compositor_contexts.contains(context_id))
            continue;
        auto& view = page->view();
        Application::the().update_compositor_viewport(context_id, view.viewport_size().to_type<int>());
        Application::the().update_compositor_display_metadata(context_id, view.display_id(), view.maximum_frames_per_second());
        Application::the().update_compositor_context_visibility(context_id, view.traversable().system_visibility_state());
        view.update_paused_debugger_overlay();
    }
}

void WebContentClient::notify_compositor_process_reconnected(Badge<Application>)
{
    if (!is_open())
        return;

    async_compositor_process_reconnected();
}

void WebContentClient::did_lose_process()
{
    // Removing pages can release the last reference to this client.
    RefPtr self = this;
    // A close request the client made before the loss; one its lost pages' release makes now is not one.
    auto requested_close = m_requested_close;

    struct LostPage {
        NonnullRefPtr<WebContentPage> page;
        Optional<u64> view_id;
    };
    Vector<LostPage> lost_pages;
    for_each_page([&](WebContentPage& page) {
        lost_pages.append({ page, page.displays_tab() ? Optional<u64> { page.view().view_id() } : Optional<u64> {} });
        return IterationDecision::Continue;
    });

    // Closing a page settles the replies it owed once this task is over.
    for (auto const& lost : lost_pages)
        lost.page->close();

    destroy_all_compositor_contexts();

    for (auto const& lost : lost_pages) {
        // The view displaying the tab waits for the events it handed down to this page.
        if (!lost.view_id.has_value())
            lost.page->view().did_lose_input_event_endpoint({}, *lost.page);
        lost.page->traversable().did_lose_page(*lost.page, WebContentProcessLost::Yes);
    }
    for (auto const& lost : lost_pages)
        lost.page->traversable().remove_page(*lost.page);

    cancel_navigation_transactions();
    fail_renderer_owned_downloads();
    remove_blob_url_entries();

    if (requested_close)
        return;

    // The views are told deferred, and looked up by ID, in case they are destroyed before then.
    auto crash_reason = m_rejected_ipc ? ViewImplementation::WebContentCrashReason::RejectedIPC : ViewImplementation::WebContentCrashReason::ProcessCrash;
    for (auto const& lost : lost_pages) {
        if (!lost.view_id.has_value())
            continue;
        m_crashed_view_ids.append(*lost.view_id);
        Core::deferred_invoke([view_id = *lost.view_id, crash_reason] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            view->handle_web_content_process_crash();
            if (view->on_web_content_crashed)
                view->on_web_content_crashed(crash_reason);
        });
    }
}

// Deferred like the loss itself, so the view reads this after it has taken on its crash state. A crash makes one report,
// so one of the views it took down is offered it. Offering it to several would let each send it, uploading it more than
// once and removing it from under the others.
void WebContentClient::did_save_crash_report(ByteString const& report_name)
{
    Core::deferred_invoke([view_ids = m_crashed_view_ids, report_name] {
        for (auto view_id : view_ids) {
            if (auto view = ViewImplementation::find_view_by_id(view_id); view.has_value()) {
                view->did_save_crash_report(report_name);
                return;
            }
        }
    });
}

void WebContentClient::cancel_navigation_transactions()
{
    ViewImplementation::for_each_view([this](ViewImplementation& view) {
        view.traversable().for_each_in_inclusive_subtree([this](CanonicalNavigable& navigable) {
            navigable.cancel_navigation_transaction_for_client(*this);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
}

// Seeing HttpOnly cookies, storing cookies the way an HTTP response does, and changing a cookie by its identity are for
// WebDriver and the test harness. RequestServer handles the cookies of HTTP responses and requests itself, so a renderer
// serving an ordinary browsing session never needs any of it.
bool WebContentClient::renderers_may_access_cookies_like_http()
{
    return Application::browser_options().webdriver_browser_endpoint.has_value()
        || Application::web_content_options().is_test_mode == IsTestMode::Yes;
}

Messages::WebContentClient::DidRequestAllCookiesWebdriverResponse WebContentClient::did_request_all_cookies_webdriver(URL::URL url)
{
    if (!renderers_may_access_cookies_like_http()) {
        did_misbehave("did_request_all_cookies_webdriver"sv, "not driven by WebDriver"sv);
        return Vector<HTTP::Cookie::Cookie> {};
    }
    return m_session->cookie_jar->get_all_cookies_webdriver(url);
}

// Script reaches cookies through a document the process hosts, and only those of such a document's origin.
bool WebContentClient::hosts_an_environment_that_may_use_cookies_of(URL::URL const& url) const
{
    for (auto const& [page_id, page] : m_pages) {
        if (!page->is_open())
            continue;
        bool hosts_one = false;
        page->for_each_hosted_document([&](CanonicalDocument& document) {
            hosts_one = document.relevant_global_object().relevant_settings_object().may_use_cookies_of(url);
            return hosts_one ? IterationDecision::Break : IterationDecision::Continue;
        });
        if (hosts_one)
            return true;
    }
    return false;
}

Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse WebContentClient::did_request_all_cookies_cookiestore(Web::PageId page_id, URL::URL url)
{
    if (auto* page = this->page(page_id))
        return page->did_request_all_cookies_cookiestore(move(url));

    return Vector<HTTP::Cookie::Cookie> {};
}

Messages::WebContentClient::DidRequestNamedCookieResponse WebContentClient::did_request_named_cookie(URL::URL url, String name)
{
    if (!renderers_may_access_cookies_like_http()) {
        did_misbehave("did_request_named_cookie"sv, "not driven by WebDriver"sv);
        return Optional<HTTP::Cookie::Cookie> {};
    }

    return m_session->cookie_jar->get_named_cookie(url, name);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentClient::did_request_cookie(Web::PageId page_id, URL::URL url, HTTP::Cookie::Source source)
{
    if (source == HTTP::Cookie::Source::Http && !renderers_may_access_cookies_like_http()) {
        did_misbehave("did_request_cookie"sv, "HTTP cookie source"sv);
        return HTTP::Cookie::VersionedCookie {};
    }

    if (auto* page = this->page(page_id))
        return page->did_request_cookie(move(url), source);

    // A spare process can request cookies for its initial page before a view adopts it. No document of that page is
    // known here, so only a request with the HTTP source is answered.
    if (m_unassigned_initial_page_id != page_id || source != HTTP::Cookie::Source::Http)
        return HTTP::Cookie::VersionedCookie {};

    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = m_session->cookie_jar->get_cookie(url, source);
    return cookie;
}

Messages::WebContentClient::DidSetStorageItemResponse WebContentClient::did_set_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, Web::HTML::EnvironmentId environment_id, Utf16String bottle_key, Utf16String value)
{
    if (auto* page = this->page(page_id))
        return page->did_set_storage_item(storage_endpoint, move(environment_id), move(bottle_key), move(value));

    // A closed page has no storage left to set. Its reply cannot be empty, so it hears the refusal a full jar gives.
    return WebView::StorageOperationError::QuotaExceededError;
}

Messages::WebContentClient::DidRequestStorageItemResponse WebContentClient::did_request_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, Web::HTML::EnvironmentId environment_id, Utf16String bottle_key)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_item(storage_endpoint, move(environment_id), move(bottle_key));

    return Optional<Utf16String> {};
}

Messages::WebContentClient::DidRequestStorageKeysResponse WebContentClient::did_request_storage_keys(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, Web::HTML::EnvironmentId environment_id)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_keys(storage_endpoint, move(environment_id));

    return Vector<Utf16String> {};
}

Messages::WebContentClient::DidRequestStorageUsageResponse WebContentClient::did_request_storage_usage(Web::PageId page_id, Web::HTML::EnvironmentId environment_id)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_usage(move(environment_id));

    return 0u;
}

Messages::WebContentClient::DidStartDownloadWithoutRequestResponse WebContentClient::did_start_download_without_request(Web::PageId page_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size)
{
    if (auto* page = this->page(page_id))
        return page->did_start_download_without_request(move(url), move(suggested_filename), total_size);

    return Optional<u64> {};
}

Messages::WebContentClient::DidStartDownloadResponse WebContentClient::did_start_download(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data)
{
    if (auto* page = this->page(page_id))
        return page->did_start_download(navigable_id, move(navigation_id), move(url), move(suggested_filename), total_size, request_server_client_id, request_server_request_id, move(initial_data));

    return Optional<u64> {};
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(Web::PageId page_id, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name, Web::HTML::SandboxingFlagSet popup_sandboxing_flag_set)
{
    if (auto* page = this->page(page_id))
        return page->did_request_new_web_view(activate_tab, hints, opener_navigable_id, move(opener_base_url), move(target_name), popup_sandboxing_flag_set);

    return { Optional<Web::PageId> {}, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::HTML::SessionHistoryEntryDescriptor> {}, Optional<Web::HTML::EnvironmentId> {}, Optional<u64> {}, Web::HTML::VisibilityState::Hidden, String {} };
}

Messages::WebContentClient::StartWorkerAgentResponse WebContentClient::start_worker_agent(Web::PageId page_id, Web::HTML::WorkerAgentStartRequest request)
{
    if (auto* page = this->page(page_id))
        return page->start_worker_agent(move(request));

    return Web::HTML::WorkerAgentId {};
}

void WebContentClient::did_close_browsing_context(Web::PageId page_id)
{
    if (auto* page = this->page(page_id)) {
        page->did_close_browsing_context();
        return;
    }

    auto* page = find_page(page_id);
    if (!page)
        return;
    page->set_detached_close_pending(false);
    release_unneeded_representing_pages();
    close_server_if_unused();
}

void WebContentClient::did_update_cookie(HTTP::Cookie::Cookie cookie)
{
    if (!renderers_may_access_cookies_like_http()) {
        did_misbehave("did_update_cookie"sv, "not driven by WebDriver"sv);
        return;
    }

    m_session->cookie_jar->update_cookie(cookie);
}

Messages::WebContentClient::DidIsKnownHstsHostResponse WebContentClient::did_is_known_hsts_host(String domain)
{
    return m_session->hsts_store->is_known_hsts_host(domain);
}

Messages::WebContentClient::DidLoseRequestServerConnectionResponse WebContentClient::did_lose_request_server_connection()
{
    auto handle = connect_new_request_server_client(*m_session);
    if (handle.is_error()) {
        warnln("Unable to connect a replacement RequestServer client: {}", handle.error());
        return OptionalNone {};
    }

    return handle.release_value();
}

Messages::WebContentClient::RequestMediaServerConnectionResponse WebContentClient::request_media_server_connection()
{
    auto handle = connect_new_media_server_client(m_media_server_client);
    if (handle.is_error()) {
        warnln("Unable to connect a MediaServer client: {}", handle.error());
        return OptionalNone {};
    }
    return handle.release_value();
}

Optional<u64> WebContentClient::exclusive_performance_owner() const
{
    Optional<u64> owner;
    auto add_owner = [&](u64 id) {
        if (owner.has_value() && *owner != id)
            return false;
        owner = id;
        return true;
    };
    for (auto const& it : m_pages) {
        auto const& page = it.value;
        if (!page->is_open())
            continue;
        if (!add_owner(page->view().view_id()))
            return {};
    }
    return owner;
}

}
