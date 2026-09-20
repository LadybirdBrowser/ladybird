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
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/FontService.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/ProcessHandle.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/SourceHighlighter.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentTestClient.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

Messages::WebContentClient::OpenSystemFontResponse WebContentClient::open_system_font(u64 generation, u64 face_id)
{
    auto font = Application::font_service().open_font(generation, face_id);
    return { move(font) };
}

Messages::WebContentClient::MatchLocalFontResponse WebContentClient::match_local_font(String name)
{
    auto font = Application::font_service().match_local_font(name);
    return { move(font) };
}

Messages::WebContentClient::MatchSystemFontResponse WebContentClient::match_system_font(String family, u16 weight, u16 width, u8 slope)
{
    auto font = Application::font_service().match_font(family, weight, width, slope);
    return { move(font) };
}

Messages::WebContentClient::MatchSystemFontForCodePointResponse WebContentClient::match_system_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    auto font = Application::font_service().match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    return { move(font) };
}

Messages::WebContentClient::ResolveGenericFontResponse WebContentClient::resolve_generic_font(String family, u16 weight, u8 slope)
{
    auto resolved = Application::font_service().resolve_generic_family(family, weight, slope);
    if (!resolved.has_value())
        return Optional<String> {};
    return Optional<String> { resolved->to_string() };
}

Messages::WebContentClient::DidAddBlobUrlEntryResponse WebContentClient::did_add_blob_url_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry)
{
    return m_session->blob_url_store->add_entry(move(url), move(entry), WeakPtr<WebContentClient> { *this });
}

void WebContentClient::did_remove_blob_url_entries(Vector<Utf16String> urls, URL::Origin origin)
{
    m_session->blob_url_store->remove_entries(urls, origin, WeakPtr<WebContentClient> { *this });
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
    clients().remove(this);
}

Optional<WebContentClient&> WebContentClient::client_for_compositor_context_id(Web::Compositor::CompositorContextId context_id)
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

void WebContentClient::die()
{
    m_process_lost = true;

    // The transport's peer-EOF and the process-exit notification race, and an embedded-only client can be destroyed
    // by this path before the process monitor looks it up. The removal is map-driven, so whichever path runs second
    // finds nothing left to do.
    SiteIsolationManager::the().remove_all_pages_for_client(*this);

    cancel_navigation_transactions();
    fail_renderer_owned_downloads();
    remove_blob_url_entries();
}

void WebContentClient::did_misbehave(StringView message_name, StringView reason)
{
    m_rejected_ipc = true;
    dbgln("WebContentClient: terminating helper process {}: {} rejected: {}", pid(), message_name, reason);
    if (should_terminate_pid(pid()))
        (void)Core::Process::terminate_process(pid(), Core::Process::TerminationMode::Forceful);
    shutdown();
}

Web::Compositor::CompositorContextId WebContentClient::compositor_context_id_for_page(Web::PageId page_id)
{
    auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
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

Optional<Web::PageId> WebContentClient::page_id_for_compositor_context_id(Web::Compositor::CompositorContextId context_id) const
{
    auto page_id = m_compositor_contexts.get(context_id);
    if (!page_id.has_value())
        return {};
    return *page_id;
}

Messages::WebContentClient::AllocateCompositorContextIdResponse WebContentClient::allocate_compositor_context_id(Web::PageId page_id, Web::Compositor::PagePresentationRegistration page_presentation_registration)
{
    if (page_presentation_registration == Web::Compositor::PagePresentationRegistration::Yes)
        return compositor_context_id_for_page(page_id);

    auto context_id = Application::the().allocate_compositor_context_id();
    remember_compositor_context(context_id, {});
    Application::the().register_compositor_context(*this, context_id, {});
    return context_id;
}

void WebContentClient::did_destroy_compositor_context(Web::Compositor::CompositorContextId context_id)
{
    forget_compositor_context(context_id);
}

bool WebContentClient::forget_compositor_context(Web::Compositor::CompositorContextId context_id)
{
    if (!m_compositor_contexts.remove(context_id))
        return false;
    return true;
}

void WebContentClient::remember_compositor_context(Web::Compositor::CompositorContextId context_id, Optional<Web::PageId> page_id)
{
    m_compositor_contexts.set(context_id, page_id);
}

void WebContentClient::assign_view(Badge<Application>, ViewImplementation& view)
{
    VERIFY(!has_views());
    VERIFY(view.is_private() == m_is_private);
    auto initial_page_id = m_unassigned_initial_page_id.release_value();
    view.traversable().set_id(m_root_navigable_id);
    view.m_client_state.page = open_page(initial_page_id, view.traversable());

    if (m_initial_top_level_history_entry.has_value()) {
        view.traversable().create_a_new_top_level_traversable({}, m_initial_top_level_history_entry.release_value(), *this);
        view.update_navigation_action_state();
    }
}

void WebContentClient::set_compositor_connection_id(Badge<Application>, i32 compositor_connection_id)
{
    m_compositor_connection_id = compositor_connection_id;
}

void WebContentClient::register_view(Web::PageId page_id, ViewImplementation& view)
{
    // A process's initial page is assigned to the view that launched it, not registered.
    VERIFY(page_id > 0 || !has_views());
    VERIFY(view.is_private() == m_is_private);
    if (m_detached_page_close_timer)
        m_detached_page_close_timer->stop();
    Application::process_manager().cancel_forced_exit(pid());
    view.m_client_state.page = open_page(page_id, view.traversable());
}

void WebContentClient::keep_view_page_for_displaced_document(Web::PageId page_id, CanonicalTraversable& traversable)
{
    auto* page = find_page(page_id);
    VERIFY(page && page->displays_tab());
    page->m_traversable = traversable.make_weak_ptr<CanonicalTraversable>();
    page->clear_history_recorded_url_for_current_load();
}

void WebContentClient::unregister_view(Web::PageId page_id)
{
    forget_compositor_context(Web::Compositor::compositor_context_id_for_page(page_id));
    if (auto* page = this->page(page_id))
        SiteIsolationManager::the().remove_page(*page);

    if (auto* page = find_page(page_id)) {
        // A page that still needs a beforeunload check is not a detached
        // background close. It is being closed without waiting for WebContent,
        // e.g. because the user requested a forced close.
        if (page->needs_beforeunload_check())
            page->set_detached_close_pending(false);
        page->close();
    }
    release_unneeded_opener_pages();
    close_server_if_unused();
}

bool WebContentClient::is_renderer_owned_download(Web::PageId page_id, u64 download_id) const
{
    auto owning_page_id = m_renderer_owned_downloads.get(download_id);
    return owning_page_id.has_value() && *owning_page_id == page_id;
}

void WebContentClient::forget_renderer_owned_download(u64 download_id)
{
    m_renderer_owned_downloads.remove(download_id);
}

void WebContentClient::fail_renderer_owned_downloads()
{
    Vector<u64> download_ids;
    download_ids.ensure_capacity(m_renderer_owned_downloads.size());
    for (auto const& entry : m_renderer_owned_downloads)
        download_ids.append(entry.key);

    m_renderer_owned_downloads.clear();

    auto& file_downloader = Application::the().file_downloader();
    for (auto download_id : download_ids)
        file_downloader.fail_download(download_id, "Download process exited"_string);
}

void WebContentClient::prepare_for_detached_close(Web::PageId page_id)
{
    if (auto* page = find_page(page_id))
        page->set_detached_close_pending(true);
}

void WebContentClient::request_close(Web::PageId page_id)
{
    // The frontend may destroy the view immediately after this for pages that
    // cannot prompt during beforeunload. Keep owning the WebContent close until
    // the page reports that its top-level traversable has been closed.
    prepare_for_detached_close(page_id);
    async_request_close(page_id);
}

void WebContentClient::register_embedded_page(Web::PageId page_id, CanonicalTraversable& traversable)
{
    auto& page = open_page(page_id, traversable);
    if (m_unassigned_initial_page_id == page_id)
        m_unassigned_initial_page_id.clear();
    Application::process_manager().cancel_forced_exit(pid());

    page.view().send_preferences_to_page({}, page);
    if (Application::browser_options().webdriver_browser_endpoint.has_value())
        Application::the().push_webdriver_session_config(page);
    page.async_set_has_focus(traversable.has_system_focus());
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
    release_unneeded_opener_pages();
    close_server_if_unused();
}

// Whether a page of this process holds part of a tab the traversable's tab opened, directly or through the tabs those
// opened.
bool WebContentClient::holds_part_of_a_tab_opened_by(CanonicalTraversable const& opener_traversable)
{
    Vector<CanonicalTraversable const*> reached;
    Vector<CanonicalTraversable const*> to_visit;
    auto reach = [&](CanonicalTraversable const& traversable) {
        if (reached.contains_slow(&traversable))
            return;
        reached.append(&traversable);
        to_visit.append(&traversable);
    };

    for (auto const& [page_id, page] : m_pages) {
        if (!page->is_open())
            continue;
        auto const& traversable = page->traversable();
        if (!page->displays_tab() && traversable.is_opener_page(*page) && !traversable.page_hosts_any(*page))
            continue;
        reach(traversable);
    }

    while (!to_visit.is_empty()) {
        to_visit.take_last()->for_each_opener_traversable([&](CanonicalTraversable& traversable) {
            reach(traversable);
        });
    }
    return reached.contains_slow(&opener_traversable);
}

void WebContentClient::release_unneeded_opener_pages()
{
    if (m_process_lost)
        return;

    Vector<NonnullRefPtr<WebContentPage>> opener_pages;
    for_each_page([&](WebContentPage& page) {
        if (!page.displays_tab() && page.traversable().is_opener_page(page))
            opener_pages.append(page);
        return IterationDecision::Continue;
    });
    for (auto const& page : opener_pages) {
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

Optional<ViewImplementation&> WebContentClient::display_view(Web::PageId page_id) const
{
    auto* page = this->page(page_id);
    if (!page || !page->displays_tab())
        return {};
    return page->view();
}

bool WebContentClient::may_act_for_page(Web::PageId page_id) const
{
    // A page ID the connection was given is not a false claim once the page is gone: the connection can
    // have sent the message while it still had the page, and the handler drops it.
    return m_pages.contains(page_id) || m_unassigned_initial_page_id == page_id;
}

bool WebContentClient::has_views() const
{
    for (auto const& page : m_pages) {
        if (page.value->is_open() && page.value->displays_tab())
            return true;
    }
    return false;
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
        auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
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

void WebContentClient::notify_all_views_of_crash()
{
    // Removing remote pages can release the last reference to this client.
    RefPtr self = this;

    // Every page must read as closed before pending history work resolves against this endpoint.
    m_process_lost = true;

    destroy_all_compositor_contexts();

    // Resolve any history work waiting on this endpoint before removing the canonical page subtrees that identify
    // their owning traversables. A missing renderer is an exactly-once completion for descendant unload tasks.
    for (auto& [page_id, page] : m_pages) {
        if (!page->is_open())
            continue;
        // The view displaying the tab waits for the events it handed down to this page.
        if (!page->displays_tab())
            page->view().did_lose_input_event_endpoint({}, *page);
        page->traversable().did_lose_page(*page);
    }

    SiteIsolationManager::the().remove_all_pages_for_client(*this);

    // Collect view IDs first, then use deferred_invoke to handle crashes safely
    // (avoids signal handler deadlock and allows views to be looked up by ID
    // in case they're destroyed before the deferred_invoke runs).
    Vector<u64> view_ids;
    for (auto& page : m_pages) {
        if (page.value->is_open() && page.value->displays_tab())
            view_ids.append(page.value->view().view_id());
    }

    auto crash_reason = m_rejected_ipc ? ViewImplementation::WebContentCrashReason::RejectedIPC : ViewImplementation::WebContentCrashReason::ProcessCrash;
    for (auto view_id : view_ids) {
        Core::deferred_invoke([view_id, crash_reason] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            view->handle_web_content_process_crash();
            if (view->on_web_content_crashed)
                view->on_web_content_crashed(crash_reason);
        });
    }
}

bool WebContentClient::send_async_scroll_to_compositor(Web::PageId page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().send_async_scroll_to_compositor(compositor_context_id_for_page(page_id), position, delta_in_device_pixels, wheel_delta_precision, scroll_gesture_phase);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC async_scroll_by page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

bool WebContentClient::handle_key_event_in_compositor(Web::PageId page_id, Web::KeyEvent const& event)
{
    return Application::the().handle_key_event_in_compositor(compositor_context_id_for_page(page_id), event);
}

void WebContentClient::dispatch_key_event_to_web_content(Web::PageId page_id, Web::KeyEvent const& event)
{
    if (!Application::the().dispatch_key_event_to_web_content(compositor_context_id_for_page(page_id), event))
        async_key_event(page_id, event.clone_without_browser_data());
}

bool WebContentClient::handle_mouse_event_in_compositor(Web::PageId page_id, Web::MouseEvent const& event)
{
    auto* page = this->page(page_id);
    if (!page)
        return false;
    return handle_mouse_event_in_compositor(page_id, page->traversable(), compositor_context_id_for_page(page_id), event);
}

// Input over a remote child of the root is the hosting process's to handle, in the root's compositor context there.
bool WebContentClient::handle_mouse_event_in_compositor(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId> context_id, Web::MouseEvent const& event)
{
    auto* page = this->page(page_id);
    auto target = page ? SiteIsolationManager::the().remote_child_frame_input_target_at(*page, root, event.position) : Optional<SiteIsolationManager::RemoteChildFrameInputTarget> {};
    if (target.has_value()) {
        auto translated_event = event.clone_without_browser_data();
        translated_event.position.set_x(event.position.x() - target->viewport_rect.x());
        translated_event.position.set_y(event.position.y() - target->viewport_rect.y());
        return target->remote_page->client().handle_mouse_event_in_compositor(target->remote_page->id(), *target->navigable, target->compositor_context_id, translated_event);
    }

    if (!context_id.has_value())
        return false;

    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().handle_mouse_event_in_compositor(*context_id, event);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC mouse_event page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

bool WebContentClient::handle_pinch_event_in_compositor(Web::PageId page_id, Web::PinchEvent const& event)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().handle_pinch_event_in_compositor(compositor_context_id_for_page(page_id), event);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC pinch_event page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

void WebContentClient::dispatch_mouse_event_to_web_content(Web::PageId page_id, Web::MouseEvent const& event)
{
    auto* page = this->page(page_id);
    if (!page)
        return;
    dispatch_mouse_event_to_web_content(page_id, page->traversable(), compositor_context_id_for_page(page_id), event);
}

void WebContentClient::dispatch_mouse_event_to_web_content(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId> context_id, Web::MouseEvent const& event)
{
    auto* page = this->page(page_id);
    auto target = page ? SiteIsolationManager::the().remote_child_frame_input_target_at(*page, root, event.position) : Optional<SiteIsolationManager::RemoteChildFrameInputTarget> {};
    if (target.has_value()) {
        auto translated_event = event.clone_without_browser_data();
        translated_event.position.set_x(event.position.x() - target->viewport_rect.x());
        translated_event.position.set_y(event.position.y() - target->viewport_rect.y());
        target->remote_page->client().dispatch_mouse_event_to_web_content(target->remote_page->id(), *target->navigable, target->compositor_context_id, translated_event);
        return;
    }

    // The compositor forwards input to the page a context presents, which the context of a hosted root has none of.
    if (&root != &root.top_level_traversable()) {
        async_mouse_event_in_hosted_root(page_id, root.id(), event.clone_without_browser_data());
        return;
    }

    if (context_id.has_value() && Application::the().dispatch_mouse_event_to_web_content(*context_id, event))
        return;

    async_mouse_event(page_id, event.clone_without_browser_data());
}

void WebContentClient::notify_presented_bitmap_ready_to_paint(Web::PageId page_id, i32 bitmap_id)
{
    auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
    if (!m_compositor_contexts.contains(context_id))
        return;

    Application::the().notify_compositor_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void WebContentClient::did_present_bitmap(Web::PageId page_id, Gfx::IntRect rect, Gfx::IntRect damage_rect, i32 bitmap_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC did_paint for page {} bitmap {} rect={}x{} at {},{}",
        page_id, bitmap_id, rect.width(), rect.height(), rect.x(), rect.y());
    if (auto view = display_view(page_id); view.has_value()) {
        view->server_did_paint({}, bitmap_id, rect.size(), damage_rect);
    } else {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping did_paint for page {} bitmap {}: no view",
            page_id, bitmap_id);
        notify_presented_bitmap_ready_to_paint(page_id, bitmap_id);
    }
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

void WebContentClient::did_finish_test(Web::PageId page_id, String text)
{
    if (auto view = display_view(page_id); view.has_value()) {
        if (view->on_test_finish)
            view->on_test_finish(text);
    }
}

void WebContentClient::did_set_test_timeout(Web::PageId page_id, double milliseconds)
{
    if (auto view = display_view(page_id); view.has_value()) {
        if (view->on_set_test_timeout)
            view->on_set_test_timeout(milliseconds);
    }
}

void WebContentClient::did_receive_reference_test_metadata(Web::PageId page_id, JsonValue metadata)
{
    if (auto view = display_view(page_id); view.has_value()) {
        if (view->on_reference_test_metadata)
            view->on_reference_test_metadata(metadata);
    }
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

Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse WebContentClient::did_request_all_cookies_cookiestore(URL::URL url)
{
    return m_session->cookie_jar->get_all_cookies_cookiestore(url);
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

    // A spare process can request cookies for its initial page before a view adopts it.
    if (m_unassigned_initial_page_id != page_id)
        return HTTP::Cookie::VersionedCookie {};

    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = m_session->cookie_jar->get_cookie(url, source);
    return cookie;
}

Messages::WebContentClient::DidSetStorageItemResponse WebContentClient::did_set_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key, Utf16String value)
{
    if (auto* page = this->page(page_id))
        return page->did_set_storage_item(storage_endpoint, move(storage_key), move(bottle_key), move(value));

    // A closed page has no storage left to set. Its reply cannot be empty, so it hears the refusal a full jar gives.
    return WebView::StorageOperationError::QuotaExceededError;
}

Messages::WebContentClient::DidRequestStorageItemResponse WebContentClient::did_request_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_item(storage_endpoint, move(storage_key), move(bottle_key));

    return Optional<Utf16String> {};
}

Messages::WebContentClient::DidRequestStorageKeysResponse WebContentClient::did_request_storage_keys(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_keys(storage_endpoint, move(storage_key));

    return Vector<Utf16String> {};
}

Messages::WebContentClient::DidRequestStorageUsageResponse WebContentClient::did_request_storage_usage(Web::PageId page_id, String storage_key)
{
    if (auto* page = this->page(page_id))
        return page->did_request_storage_usage(move(storage_key));

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

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(Web::PageId page_id, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name)
{
    if (auto* page = this->page(page_id))
        return page->did_request_new_web_view(activate_tab, hints, opener_navigable_id, move(opener_base_url), move(target_name));

    return { Optional<Web::PageId> {}, Optional<Web::HTML::CrossProcessId> {}, Optional<Web::HTML::SessionHistoryEntryDescriptor> {}, Web::HTML::VisibilityState::Hidden, String {} };
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
    release_unneeded_opener_pages();
    close_server_if_unused();
}

void WebContentClient::did_set_cookie(URL::URL url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    if (source == HTTP::Cookie::Source::Http && !renderers_may_access_cookies_like_http()) {
        did_misbehave("did_set_cookie"sv, "HTTP cookie source"sv);
        return;
    }

    m_session->cookie_jar->set_cookie(url, cookie, source);
}

void WebContentClient::did_update_cookie(HTTP::Cookie::Cookie cookie)
{
    if (!renderers_may_access_cookies_like_http()) {
        did_misbehave("did_update_cookie"sv, "not driven by WebDriver"sv);
        return;
    }

    m_session->cookie_jar->update_cookie(cookie);
}

void WebContentClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    m_session->cookie_jar->expire_cookies_with_time_offset(offset);
}

void WebContentClient::did_store_hsts_policy_for_testing(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    m_session->hsts_store->store_policy(domain, policy);
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

void WebContentClient::did_simulate_worker_request_server_connection_loss(Web::PageId page_id)
{
    VERIFY(Application::web_content_options().is_test_mode == IsTestMode::Yes);
    if (auto result = WorkerProcessManager::the().simulate_request_server_connection_loss_for_testing(*this, page_id); result.is_error()) {
        warnln("Unable to reconnect WebWorker processes to RequestServer: {}", result.error());
        VERIFY_NOT_REACHED();
    }
}

String WebContentClient::did_request_ui_process_session_history_for_testing(Web::PageId page_id)
{
    if (auto view = display_view(page_id); view.has_value())
        return { view->ui_process_session_history_for_testing({}) };

    return { "{}"_string };
}

String WebContentClient::did_request_site_isolation_process_tree_for_testing(Web::PageId page_id)
{
    return { SiteIsolationManager::the().dump_process_tree(*this, page_id) };
}

void WebContentClient::did_request_crash_of_remote_frame_processes_for_testing(Web::PageId page_id)
{
    auto* page = this->page(page_id);
    if (!page)
        return;

    page->traversable().for_each_in_subtree([](CanonicalNavigable& child_frame) {
        if (child_frame.has_remote_host())
            child_frame.remote_host().async_debug_request("crash-current-page"sv, ""sv);
        return IterationDecision::Continue;
    });
}

void WebContentClient::did_reset_session_history_for_testing(Web::PageId page_id, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    if (auto view = display_view(page_id); view.has_value())
        view->did_reset_session_history_for_testing({}, move(active_entry));
}

bool WebContentClient::did_request_capture_session_history_snapshot_for_testing(Web::PageId page_id)
{
    if (auto view = display_view(page_id); view.has_value())
        return view->capture_session_history_snapshot_for_testing({});

    return false;
}

bool WebContentClient::did_request_restore_session_history_snapshot_for_testing(Web::PageId page_id)
{
    if (auto view = display_view(page_id); view.has_value())
        return view->restore_captured_session_history_snapshot_for_testing({});

    return false;
}

bool WebContentClient::did_request_register_session_store_tab_for_testing(Web::PageId page_id)
{
    if (auto view = display_view(page_id); view.has_value())
        return view->register_session_store_tab_for_testing({});

    return false;
}

String WebContentClient::did_request_session_store_tab_state_for_testing(Web::PageId page_id)
{
    if (auto view = display_view(page_id); view.has_value())
        return { view->session_store_tab_state_for_testing({}) };

    return { "{}"_string };
}

void WebContentClient::did_present_backing_stores(Web::PageId page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received {} backing stores for page {}", backing_stores.size(), page_id);
    if (auto view = display_view(page_id); view.has_value()) {
        view->did_allocate_backing_stores({}, move(bitmap_ids), move(backing_stores));
    } else {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping {} backing stores for page {}: no view", backing_stores.size(), page_id);
    }
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
