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
#include <LibCore/Timer.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/SourceHighlighter.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

HashTable<WebContentClient*>& WebContentClient::clients()
{
    static NeverDestroyed<HashTable<WebContentClient*>> clients;
    return *clients;
}

static constexpr auto detached_page_close_timeout_ms = 1000;
static constexpr auto close_server_exit_timeout_ms = 5000;
static constexpr auto detached_page_forced_exit_timeout_ms = detached_page_close_timeout_ms + close_server_exit_timeout_ms;

static Optional<String> history_title(Utf16String const& title, URL::URL const& url)
{
    if (title.is_empty())
        return {};

    auto title_utf8 = title.to_utf8();
    if (title_utf8 == url.serialize() || title_utf8 == url.serialize(URL::ExcludeFragment::Yes))
        return {};

    return title_utf8;
}

WebContentClient::WebContentClient(NonnullOwnPtr<IPC::Transport> transport, u64 initial_page_id)
    : IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(transport))
    , m_initial_page_id(initial_page_id)
{
    VERIFY(m_initial_page_id > 0);
    clients().set(this);
}

WebContentClient::~WebContentClient()
{
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
    // Intentionally empty. Restart is handled at another level.
}

Web::Compositor::CompositorContextId WebContentClient::compositor_context_id_for_page(u64 page_id)
{
    auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
    if (auto registered_page_id = m_compositor_contexts.get(context_id); registered_page_id.has_value()) {
        VERIFY(registered_page_id->has_value());
        VERIFY(**registered_page_id == page_id);
        return context_id;
    }

    remember_compositor_context(context_id, page_id);
    Application::the().register_compositor_context(*this, context_id, page_id);
    return context_id;
}

Optional<u64> WebContentClient::page_id_for_compositor_context_id(Web::Compositor::CompositorContextId context_id) const
{
    auto page_id = m_compositor_contexts.get(context_id);
    if (!page_id.has_value())
        return {};
    return *page_id;
}

Messages::WebContentClient::AllocateCompositorContextIdResponse WebContentClient::allocate_compositor_context_id(u64 page_id, Web::Compositor::PagePresentationRegistration page_presentation_registration)
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

void WebContentClient::remember_compositor_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id)
{
    m_compositor_contexts.set(context_id, page_id);
}

void WebContentClient::assign_view(Badge<Application>, ViewImplementation& view)
{
    VERIFY(m_views.is_empty());
    view.m_client_state.page_index = m_initial_page_id;
    m_views.set(m_initial_page_id, view);
}

void WebContentClient::set_compositor_connection_id(Badge<Application>, i32 compositor_connection_id)
{
    m_compositor_connection_id = compositor_connection_id;
}

void WebContentClient::register_view(u64 page_id, ViewImplementation& view)
{
    VERIFY(page_id > 0);
    if (m_detached_page_close_timer)
        m_detached_page_close_timer->stop();
    Application::process_manager().cancel_forced_exit(pid());
    view.m_client_state.page_index = page_id;
    m_views.set(page_id, view);
    m_history_recorded_urls_for_current_load.remove(page_id);
}

void WebContentClient::unregister_view(u64 page_id)
{
    forget_compositor_context(Web::Compositor::compositor_context_id_for_page(page_id));
    SiteIsolationManager::the().close_remote_child_frames_for_page(*this, page_id);
    SiteIsolationManager::the().remove_page(page_id);

    // A page that still needs a beforeunload check is not a detached
    // background close. It is being closed without waiting for WebContent,
    // e.g. because the user requested a forced close.
    if (auto view = m_views.get(page_id); view.has_value() && (*view)->needs_beforeunload_check())
        m_detached_pages_pending_close.remove(page_id);

    m_views.remove(page_id);
    m_history_recorded_urls_for_current_load.remove(page_id);
    close_server_if_unused();
}

void WebContentClient::prepare_for_detached_close(u64 page_id)
{
    m_detached_pages_pending_close.set(page_id);
}

void WebContentClient::request_close(u64 page_id)
{
    // The frontend may destroy the view immediately after this for pages that
    // cannot prompt during beforeunload. Keep owning the WebContent close until
    // the page reports that its top-level traversable has been closed.
    prepare_for_detached_close(page_id);
    async_request_close(page_id);
}

void WebContentClient::register_embedded_page(u64 page_id)
{
    m_embedded_pages.set(page_id);
    Application::process_manager().cancel_forced_exit(pid());
}

void WebContentClient::unregister_embedded_page(u64 page_id)
{
    m_embedded_pages.remove(page_id);
    close_server_if_unused();
}

void WebContentClient::close_server_if_unused()
{
    if (!m_views.is_empty())
        return;
    if (!m_embedded_pages.is_empty())
        return;

    if (m_detached_pages_pending_close.is_empty()) {
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
            m_detached_pages_pending_close.clear();
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

    for (auto const& [page_id, view] : m_views) {
        auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
        if (!m_compositor_contexts.contains(context_id))
            continue;
        Application::the().update_compositor_viewport(context_id, view->viewport_size().to_type<int>());
        Application::the().update_compositor_display_metadata(context_id, view->display_id(), view->maximum_frames_per_second());
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
    destroy_all_compositor_contexts();
    SiteIsolationManager::the().remove_all_pages_for_client(*this);

    // Collect view IDs first, then use deferred_invoke to handle crashes safely
    // (avoids signal handler deadlock and allows views to be looked up by ID
    // in case they're destroyed before the deferred_invoke runs).
    Vector<u64> view_ids;
    view_ids.ensure_capacity(m_views.size());
    for (auto& [page_id, view] : m_views)
        view_ids.unchecked_append(view->view_id());

    for (auto view_id : view_ids) {
        Core::deferred_invoke([view_id] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            view->handle_web_content_process_crash();
            if (view->on_web_content_crashed)
                view->on_web_content_crashed();
        });
    }
}

bool WebContentClient::send_async_scroll_to_compositor(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().send_async_scroll_to_compositor(compositor_context_id_for_page(page_id), position, delta_in_device_pixels);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC async_scroll_by page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

bool WebContentClient::handle_mouse_event_in_compositor(u64 page_id, Web::MouseEvent const& event)
{
    if (auto target = SiteIsolationManager::the().remote_child_frame_input_target_at(page_id, event.position); target.has_value()) {
        auto translated_event = event.clone_without_browser_data();
        translated_event.position.set_x(event.position.x() - target->viewport_rect.x());
        translated_event.position.set_y(event.position.y() - target->viewport_rect.y());
        return target->remote_client->handle_mouse_event_in_compositor(target->remote_page_id, translated_event);
    }

    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().handle_mouse_event_in_compositor(compositor_context_id_for_page(page_id), event);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC mouse_event page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

bool WebContentClient::handle_pinch_event_in_compositor(u64 page_id, Web::PinchEvent const& event)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().handle_pinch_event_in_compositor(compositor_context_id_for_page(page_id), event);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC pinch_event page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

void WebContentClient::dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const& event)
{
    if (auto target = SiteIsolationManager::the().remote_child_frame_input_target_at(page_id, event.position); target.has_value()) {
        auto translated_event = event.clone_without_browser_data();
        translated_event.position.set_x(event.position.x() - target->viewport_rect.x());
        translated_event.position.set_y(event.position.y() - target->viewport_rect.y());
        target->remote_client->dispatch_mouse_event_to_web_content(target->remote_page_id, translated_event);
        return;
    }

    auto context_id = compositor_context_id_for_page(page_id);
    if (Application::the().dispatch_mouse_event_to_web_content(context_id, event))
        return;

    async_mouse_event(page_id, event.clone_without_browser_data());
}

void WebContentClient::notify_presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id)
{
    auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
    if (!m_compositor_contexts.contains(context_id))
        return;

    Application::the().notify_compositor_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void WebContentClient::did_present_bitmap(u64 page_id, Gfx::IntRect rect, i32 bitmap_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC did_paint for page {} bitmap {} rect={}x{} at {},{}",
        page_id, bitmap_id, rect.width(), rect.height(), rect.x(), rect.y());
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->server_did_paint({}, bitmap_id, rect.size());
    } else {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping did_paint for page {} bitmap {}: no view",
            page_id, bitmap_id);
        notify_presented_bitmap_ready_to_paint(page_id, bitmap_id);
    }
}

void WebContentClient::did_request_new_process_for_navigation(u64 page_id, URL::URL url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->create_new_process_for_cross_site_navigation(url, move(document_resource), history_handling);
}

Messages::WebContentClient::DecideNavigationProcessResponse WebContentClient::decide_navigation_process(u64 page_id, Optional<String> frame_id, URL::URL current_url, URL::URL target_url, Web::NavigationTarget target)
{
    return SiteIsolationManager::the().decide_navigation_process(*this, page_id, move(frame_id), move(current_url), move(target_url), target);
}

void WebContentClient::did_request_new_process_for_child_frame_navigation(u64 page_id, String frame_id, URL::URL url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    auto& site_isolation_manager = SiteIsolationManager::the();
    auto child_frame = site_isolation_manager.child_frame(page_id, frame_id);
    if (!child_frame.has_value())
        return;
    if (!site_isolation_manager.has_matching_pending_child_frame_navigation(page_id, frame_id, url, ChildFrameOwner::Remote))
        return;

    auto remote_process_or_error = Application::the().launch_child_frame_web_content_process();
    if (remote_process_or_error.is_error()) {
        warnln("Unable to create WebContent process for child frame navigation: {}", remote_process_or_error.error());
        site_isolation_manager.clear_pending_child_frame_navigation(page_id, frame_id);
        return;
    }

    auto remote_process = remote_process_or_error.release_value();
    auto remote_page_id = remote_process.page_id;
    auto remote_client = move(remote_process.client);
    site_isolation_manager.record_pending_child_frame_navigation(page_id, frame_id, url, ChildFrameOwner::Remote, remote_page_id);
    remote_client->register_embedded_page(remote_page_id);
    remote_client->async_set_page_parent_context(remote_page_id, Web::Compositor::compositor_context_id_for_page(page_id));
    if (child_frame->viewport_rect.has_value()) {
        remote_client->async_set_viewport(
            remote_page_id,
            child_frame->viewport_rect->size(),
            child_frame->device_pixel_ratio,
            Web::ViewportIsFullscreen::No);
    }
    remote_client->async_set_system_visibility_state(remote_page_id, Web::HTML::VisibilityState::Visible);
    remote_client->async_load_url_with_document_resource(remote_page_id, url, move(document_resource), history_handling);

    site_isolation_manager.transition_child_frame_to_remote(*this, page_id, frame_id, move(remote_client), remote_page_id);
}

void WebContentClient::did_create_child_frame(u64 page_id, String parent_frame_id, String frame_id)
{
    SiteIsolationManager::the().did_create_child_frame(page_id, move(parent_frame_id), move(frame_id));
}

void WebContentClient::did_update_child_frame_viewport(u64 page_id, String frame_id, Web::DevicePixelRect viewport_rect, double device_pixel_ratio)
{
    SiteIsolationManager::the().did_update_child_frame_viewport(page_id, move(frame_id), viewport_rect, device_pixel_ratio);
}

void WebContentClient::did_commit_child_frame_navigation(u64 page_id, String frame_id, URL::URL url)
{
    SiteIsolationManager::the().did_commit_child_frame_navigation(*this, page_id, frame_id, url);
}

void WebContentClient::did_destroy_child_frame(u64 page_id, String frame_id)
{
    SiteIsolationManager::the().did_destroy_child_frame(*this, page_id, frame_id);
}

Optional<WebContentClient::ChildFrameHost const&> WebContentClient::child_frame(u64 page_id, StringView frame_id) const
{
    return SiteIsolationManager::the().child_frame(page_id, frame_id);
}

void WebContentClient::did_start_webdriver_navigation(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_start_webdriver_navigation({}, url);
}

void WebContentClient::maybe_record_history_visit_for_current_load(u64 page_id, URL::URL const& url, Optional<String> title, StringView reason)
{
    auto normalized_url = HistoryStore::normalize_url(url);
    if (!normalized_url.has_value())
        return;

    if (auto recorded_url = m_history_recorded_urls_for_current_load.get(page_id); recorded_url.has_value() && *recorded_url == *normalized_url) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Visit for page {} at '{}' was already recorded during this load before {}", page_id, *normalized_url, reason);
        return;
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Recording history visit for page {} at '{}' after {}", page_id, *normalized_url, reason);

    // Title and favicon updates already give us a useful history entry, so
    // do not wait for did_finish_loading() on pages that never reach it.
    Application::history_store().record_visit(url, move(title));
    m_history_recorded_urls_for_current_load.set(page_id, normalized_url.release_value());
}

void WebContentClient::did_start_loading(u64 page_id, URL::URL url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(OptionalNone {});

    m_history_recorded_urls_for_current_load.remove(page_id);

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->m_should_suppress_history_for_current_load = view->m_should_suppress_history_for_next_load;
        view->m_should_suppress_history_for_next_load = false;
        view->did_start_navigation(url, move(document_resource), is_redirect, history_handling);

        view->set_url({}, url);

        if (view->on_load_start)
            view->on_load_start(url, is_redirect);

        for (auto const& [id, listener] : view->m_navigation_listeners) {
            if (listener.on_load_start)
                listener.on_load_start(url, is_redirect);
        }
    }
}

void WebContentClient::did_cancel_loading(u64 page_id, URL::URL url)
{
    m_history_recorded_urls_for_current_load.remove(page_id);

    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_cancel_navigation(url);
}

void WebContentClient::did_finish_loading(u64 page_id, URL::URL url)
{
    if (url.scheme() == "about"sv && url.paths().size() == 1) {
        if (auto web_ui = WebUI::create(*this, page_id, url.paths().first()); web_ui.is_error())
            warnln("Could not create WebUI for {}: {}", url, web_ui.error());
        else
            m_web_ui = web_ui.release_value();
    }

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto client_url = url;
        // Browser-generated pages can finish with an internal document URL.
        // Keep exposing the URL accepted at load start for suppressed loads.
        if (view->m_should_suppress_history_for_current_load)
            client_url = view->url();
        else
            view->set_url({}, url);
        auto should_update_history = !view->m_should_suppress_history_for_current_load;
        auto title = history_title(view->title(), url);

        if (should_update_history) {
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Load finished for page {} at '{}' with title '{}'",
                page_id,
                url,
                title.has_value() ? title->bytes_as_string_view() : "<none>"sv);

            maybe_record_history_visit_for_current_load(page_id, url, title, "load finish"sv);
            if (title.has_value())
                Application::history_store().update_title(url, *title);
            if (view->favicon_base64_png().has_value())
                Application::history_store().update_favicon(url, *view->favicon_base64_png());
        }

        view->did_finish_navigation(client_url);

        if (view->on_load_finish)
            view->on_load_finish(client_url);

        for (auto const& [id, listener] : view->m_navigation_listeners) {
            if (listener.on_load_finish)
                listener.on_load_finish(client_url);
        }
    } else {
        SiteIsolationManager::the().remote_child_frame_did_commit_navigation(*this, page_id, url);
    }
}

void WebContentClient::did_finish_test(u64 page_id, String text)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_test_finish)
            view->on_test_finish(text);
    }
}

void WebContentClient::did_set_test_timeout(u64 page_id, double milliseconds)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_set_test_timeout)
            view->on_set_test_timeout(milliseconds);
    }
}

void WebContentClient::did_receive_reference_test_metadata(u64 page_id, JsonValue metadata)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_reference_test_metadata)
            view->on_reference_test_metadata(metadata);
    }
}

void WebContentClient::did_set_browser_zoom(u64 page_id, double factor)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->set_zoom(factor);
}

void WebContentClient::did_find_in_page(u64 page_id, size_t current_match_index, Optional<size_t> total_match_count)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_find_in_page)
            view->on_find_in_page(current_match_index, total_match_count);
    }
}

void WebContentClient::did_request_refresh(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->reload();
}

void WebContentClient::did_request_cursor_change(u64 page_id, Gfx::Cursor cursor)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_cursor_change)
            view->on_cursor_change(cursor);
    }
}

void WebContentClient::did_change_title(u64 page_id, Utf16String title)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(title);

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (!title.is_empty() && !view->m_should_suppress_history_for_current_load) {
            auto title_utf8 = title.to_utf8();

            maybe_record_history_visit_for_current_load(page_id, view->url(), title_utf8, "title change"sv);
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Title changed for page {} at '{}' to '{}'",
                page_id,
                view->url(),
                title_utf8);

            Application::history_store().update_title(view->url(), title_utf8);
        }

        if (title.is_empty())
            title = Utf16String::from_utf8(view->url().serialize());

        view->set_title({}, title);

        if (view->on_title_change)
            view->on_title_change(title);
    }
}

void WebContentClient::did_change_url(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        // Some navigations report the same URL more than once. Keep those
        // duplicate updates inside LibWebView so frontends do not reset
        // location bar state or steal focus for a no-op change.
        if (view->url() == url)
            return;

        view->set_url({}, url);

        if (view->on_url_change)
            view->on_url_change(url);
    } else {
        SiteIsolationManager::the().remote_child_frame_did_finish_loading(*this, page_id, url);
    }
}

void WebContentClient::did_request_tooltip_override(u64 page_id, Gfx::IntPoint position, ByteString title)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_tooltip_override)
            view->on_request_tooltip_override(view->to_widget_position(position), title);
    }
}

void WebContentClient::did_stop_tooltip_override(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_stop_tooltip_override)
            view->on_stop_tooltip_override();
    }
}

void WebContentClient::did_enter_tooltip_area(u64 page_id, ByteString title)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_enter_tooltip_area)
            view->on_enter_tooltip_area(title);
    }
}

void WebContentClient::did_leave_tooltip_area(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_leave_tooltip_area)
            view->on_leave_tooltip_area();
    }
}

void WebContentClient::did_hover_link(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_hover)
            view->on_link_hover(url);
    }
}

void WebContentClient::did_unhover_link(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_unhover)
            view->on_link_unhover();
    }
}

void WebContentClient::did_click_link(u64 page_id, URL::URL url, ByteString target, unsigned modifiers)
{
    if (modifiers == Web::UIEvents::Mod_PlatformCtrl)
        Application::the().open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
    else if (target == "_blank"sv)
        Application::the().open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes);
    else if (auto view = view_for_page_id(page_id); view.has_value())
        view->load(url);
}

void WebContentClient::did_middle_click_link(u64, URL::URL url, ByteString, unsigned)
{
    Application::the().open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
}

void WebContentClient::did_request_context_menu(u64 page_id, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_request_page_context_menu({}, content_position, for_input_events_target);
}

void WebContentClient::did_request_link_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_request_link_context_menu({}, content_position, move(url));
}

void WebContentClient::did_request_image_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned, Optional<Gfx::ShareableBitmap> bitmap)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_request_image_context_menu({}, content_position, move(url), move(bitmap));
}

void WebContentClient::did_request_media_context_menu(u64 page_id, Gfx::IntPoint content_position, ByteString, unsigned, Web::Page::MediaContextMenu menu)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_request_media_context_menu({}, content_position, move(menu));
}

void WebContentClient::did_get_source(u64, URL::URL url, URL::URL base_url, String source)
{
    if (auto view = Application::the().open_blank_new_tab(Web::HTML::ActivateTab::Yes); view.has_value()) {
        auto html = highlight_source(url, base_url, source, Syntax::Language::HTML);
        view->load_html(html);
    }
}

static JsonObject parse_json(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (!parsed_tree.value().is_object()) {
        dbgln("Expected {} to be an object: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_object());
}

static JsonArray parse_json_array(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (!parsed_tree.value().is_array()) {
        dbgln("Expected {} to be an array: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_array());
}

static Optional<JsonObject> parse_optional_json_object(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (parsed_tree.value().is_null())
        return {};

    if (!parsed_tree.value().is_object()) {
        dbgln("Expected {} to be an object or null: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_object());
}

void WebContentClient::did_inspect_dom_tree(u64 page_id, String dom_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_tree)
            view->on_received_dom_tree(parse_json(dom_tree, "DOM tree"sv));
    }
}

static ErrorOr<Vector<DevTools::DevToolsDelegate::StorageItem>> parse_storage_items(String const& storage_items)
{
    auto parsed_items = JsonValue::from_string(storage_items);
    if (parsed_items.is_error())
        return Error::from_string_literal("Unable to parse storage items");

    if (!parsed_items.value().is_array())
        return Error::from_string_literal("Expected storage items to be an array");

    Vector<DevTools::DevToolsDelegate::StorageItem> items;
    parsed_items.value().as_array().for_each([&](auto const& item) {
        if (!item.is_object())
            return;

        auto name = item.as_object().get_string("name"sv);
        auto value = item.as_object().get_string("value"sv);
        if (!name.has_value() || !value.has_value())
            return;

        items.append({ name.release_value(), value.release_value() });
    });
    return items;
}

void WebContentClient::did_inspect_storage(u64 page_id, u64 request_id, String storage_items)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto handler = view->on_received_storage_items.take(request_id);
        if (handler.has_value())
            (*handler)(parse_storage_items(storage_items));
    }
}

void WebContentClient::did_inspect_dom_node(u64 page_id, DOMNodeProperties properties)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_node_properties)
            view->on_received_dom_node_properties(move(properties));
    }
}

void WebContentClient::did_inspect_grid_layouts(u64 page_id, String grid_layouts)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_grid_layouts)
            view->on_received_grid_layouts(parse_json_array(grid_layouts, "grid layouts"sv));
    }
}

void WebContentClient::did_inspect_current_grid(u64 page_id, String grid_layout)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_current_grid)
            view->on_received_current_grid(parse_optional_json_object(grid_layout, "current grid"sv));
    }
}

void WebContentClient::did_inspect_current_flexbox(u64 page_id, String flexbox_layout)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_current_flexbox)
            view->on_received_current_flexbox(parse_optional_json_object(flexbox_layout, "current flexbox"sv));
    }
}

void WebContentClient::did_inspect_indexed_database(u64 page_id, u64 request_id, String result)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_indexed_database_inspection(request_id, parse_json(result, "IndexedDB inspection result"sv));
}

void WebContentClient::did_inspect_accessibility_tree(u64 page_id, String accessibility_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_accessibility_tree)
            view->on_received_accessibility_tree(parse_json(accessibility_tree, "accessibility tree"sv));
    }
}

void WebContentClient::did_get_hovered_node_id(u64 page_id, Web::UniqueNodeID node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_hovered_node_id)
            view->on_received_hovered_node_id(node_id);
    }
}

void WebContentClient::did_get_node_id_at_position(u64 page_id, u64 request_id, Web::UniqueNodeID node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_receive_node_picker_hit_test(request_id, node_id);
    }
}

void WebContentClient::did_finish_editing_dom_node(u64 page_id, Optional<Web::UniqueNodeID> node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_finished_editing_dom_node)
            view->on_finished_editing_dom_node(node_id);
    }
}

void WebContentClient::did_mutate_dom(u64 page_id, Mutation mutation)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_dom_mutation_received)
            view->on_dom_mutation_received(move(mutation));
    }
}

void WebContentClient::did_get_dom_node_html(u64 page_id, String html)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_node_html)
            view->on_received_dom_node_html(move(html));
    }
}

void WebContentClient::did_list_style_sheets(u64 page_id, Vector<Web::CSS::StyleSheetIdentifier> stylesheets)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_list)
            view->on_received_style_sheet_list(stylesheets);
    }
}

void WebContentClient::did_get_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier, URL::URL base_url, String source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_source)
            view->on_received_style_sheet_source(identifier, base_url, source);
    }
}

void WebContentClient::did_list_devtools_sources(u64 page_id, u64 request_id, Vector<Web::HTML::ScriptRegistry::Description> sources)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto handler = view->on_received_devtools_sources.take(request_id);
        if (handler.has_value())
            (*handler)(move(sources));
    }
}

void WebContentClient::did_get_devtools_source(u64 page_id, Web::HTML::ScriptRegistry::Identifier source_id, Optional<Web::HTML::ScriptRegistry::Content> source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto handler = view->on_received_devtools_source.take(source_id);
        if (handler.has_value())
            (*handler)(move(source));
    }
}

void WebContentClient::did_add_devtools_source(u64 page_id, Web::HTML::ScriptRegistry::Description source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_devtools_source_available)
            view->on_devtools_source_available(move(source));
    }
}

void WebContentClient::did_resolve_dom_node_url(u64 page_id, u64 request_id, String resolved_url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto handler = view->on_resolved_dom_node_url.take(request_id);
        if (handler.has_value())
            (*handler)(move(resolved_url));
    }
}

void WebContentClient::did_take_screenshot(u64 page_id, Gfx::ShareableBitmap screenshot)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_screenshot({}, screenshot);
}

void WebContentClient::did_get_internal_page_info(u64 page_id, WebView::PageInfoType type, Optional<Core::AnonymousBuffer> info)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_internal_page_info({}, type, info);
}

void WebContentClient::did_execute_js_console_input(u64 page_id, JsonValue result)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_js_console_result)
            view->on_received_js_console_result(move(result));
    }
}

void WebContentClient::did_output_js_console_message(u64 page_id, ConsoleOutput console_output)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_console_message)
            view->on_console_message(move(console_output));
    }
}

void WebContentClient::did_start_network_request(u64 page_id, u64 request_id, URL::URL url, ByteString method, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_request_started)
            view->on_network_request_started(request_id, url, method, request_headers, move(request_body), move(initiator_type));
    }
}

void WebContentClient::did_receive_network_response_headers(u64 page_id, u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> response_headers)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_response_headers_received)
            view->on_network_response_headers_received(request_id, status_code, reason_phrase, response_headers);
    }
}

void WebContentClient::did_receive_network_response_body(u64 page_id, u64 request_id, ByteBuffer data)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_response_body_received)
            view->on_network_response_body_received(request_id, move(data));
    }
}

void WebContentClient::did_finish_network_request(u64 page_id, u64 request_id, u64 body_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_request_finished)
            view->on_network_request_finished(request_id, body_size, timing_info, network_error);
    }
}

void WebContentClient::did_request_alert(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_alert)
            view->on_request_alert(message);
    }
}

void WebContentClient::did_request_confirm(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_confirm)
            view->on_request_confirm(message);
    }
}

void WebContentClient::did_request_prompt(u64 page_id, String message, String default_)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_prompt)
            view->on_request_prompt(message, default_);
    }
}

void WebContentClient::did_request_set_prompt_text(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_set_prompt_text)
            view->on_request_set_prompt_text(message);
    }
}

void WebContentClient::did_request_accept_dialog(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_accept_dialog)
            view->on_request_accept_dialog();
    }
}

void WebContentClient::did_request_dismiss_dialog(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_dismiss_dialog)
            view->on_request_dismiss_dialog();
    }
}

void WebContentClient::did_change_favicon(u64 page_id, Gfx::ShareableBitmap favicon)
{
    if (!favicon.is_valid()) {
        dbgln("DidChangeFavicon: Received invalid favicon");
        return;
    }

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (!view->m_should_suppress_history_for_current_load)
            maybe_record_history_visit_for_current_load(page_id, view->url(), history_title(view->title(), view->url()), "favicon change"sv);
        view->set_favicon({}, *favicon.bitmap());
    }
}

void WebContentClient::did_request_document_cookie_version_index(u64 page_id, i64 document_id, String domain)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (auto document_index = view->ensure_document_cookie_version_index({}, domain); !document_index.is_error())
            async_set_document_cookie_version_index(page_id, document_id, document_index.value());
    }
}

Messages::WebContentClient::DidRequestAllCookiesWebdriverResponse WebContentClient::did_request_all_cookies_webdriver(URL::URL url)
{
    return Application::cookie_jar().get_all_cookies_webdriver(url);
}

Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse WebContentClient::did_request_all_cookies_cookiestore(URL::URL url)
{
    return Application::cookie_jar().get_all_cookies_cookiestore(url);
}

Messages::WebContentClient::DidRequestNamedCookieResponse WebContentClient::did_request_named_cookie(URL::URL url, String name)
{
    return Application::cookie_jar().get_named_cookie(url, name);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentClient::did_request_cookie(u64 page_id, URL::URL url, HTTP::Cookie::Source source)
{
    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = Application::cookie_jar().get_cookie(url, source);

    if (source == HTTP::Cookie::Source::NonHttp) {
        if (auto view = view_for_page_id(page_id); view.has_value())
            cookie.cookie_version = view->document_cookie_version(url);
    }

    return cookie;
}

void WebContentClient::did_set_cookie(URL::URL url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    Application::cookie_jar().set_cookie(url, cookie, source);
}

void WebContentClient::did_update_cookie(HTTP::Cookie::Cookie cookie)
{
    Application::cookie_jar().update_cookie(cookie);
}

void WebContentClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    Application::cookie_jar().expire_cookies_with_time_offset(offset);
}

void WebContentClient::did_request_delete_all_cookies(u64 page_id, u64 request_id, URL::URL url)
{
    Application::cookie_jar().delete_all_cookies(url);
    async_did_delete_all_cookies(page_id, request_id);
}

void WebContentClient::did_store_hsts_policy(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    Application::hsts_store().store_policy(domain, policy);
}

Messages::WebContentClient::DidIsKnownHstsHostResponse WebContentClient::did_is_known_hsts_host(String domain)
{
    return Application::hsts_store().is_known_hsts_host(domain);
}

Messages::WebContentClient::DidRequestStorageItemResponse WebContentClient::did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, String bottle_key)
{
    return Application::storage_jar().get_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidSetStorageItemResponse WebContentClient::did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, String bottle_key, String value)
{
    return Application::storage_jar().set_item(storage_endpoint, storage_key, bottle_key, value);
}

void WebContentClient::did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, String bottle_key)
{
    Application::storage_jar().remove_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidRequestStorageKeysResponse WebContentClient::did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    return Application::storage_jar().get_all_keys(storage_endpoint, storage_key);
}

void WebContentClient::did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    Application::storage_jar().clear_storage_key(storage_endpoint, storage_key);
}

void WebContentClient::did_change_storage_item(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String url, Optional<String> key, Optional<String> old_value, Optional<String> new_value)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto host = DevTools::storage_host_for_url(url);
        if (!host.has_value())
            return;

        DevTools::DevToolsDelegate::StorageChange::Type type;
        if (!key.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Cleared;
        else if (!old_value.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Added;
        else if (!new_value.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Deleted;
        else
            type = DevTools::DevToolsDelegate::StorageChange::Type::Changed;

        view->notify_storage_changed({
            .storage_endpoint = storage_endpoint,
            .host = host.release_value(),
            .type = type,
            .key = move(key),
        });
    }
}

void WebContentClient::did_update_indexed_database(u64 page_id, String update)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->notify_indexed_database_changed(parse_json(update, "IndexedDB update"sv));
}

void WebContentClient::did_post_broadcast_channel_message(u64, Web::HTML::BroadcastChannelMessage message)
{
    WebContentClient::for_each_client([&](auto& client) {
        if (client.pid() == message.source_process_id)
            return IterationDecision::Continue;
        client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });
    WorkerProcessManager::the().broadcast_channel_message_from_web_content(message);
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints)
{
    auto new_page_id = Application::the().allocate_page_id();
    String handle;
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_new_web_view)
            handle = view->on_new_web_view(activate_tab, hints, new_page_id);
    }

    return { new_page_id, move(handle) };
}

void WebContentClient::did_request_activate_tab(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_activate_tab)
            view->on_activate_tab();
    }
}

void WebContentClient::did_close_browsing_context(u64 page_id)
{
    unregister_embedded_page(page_id);
    m_detached_pages_pending_close.remove(page_id);
    SiteIsolationManager::the().close_remote_child_frames_for_page(*this, page_id);
    SiteIsolationManager::the().remove_page(page_id);

    if (auto view = m_views.get(page_id); view.has_value()) {
        if ((*view)->on_close)
            (*view)->on_close();
    }

    close_server_if_unused();
}

void WebContentClient::did_change_needs_beforeunload_check(u64 page_id, bool needs_beforeunload_check)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_needs_beforeunload_check({}, needs_beforeunload_check);
}

void WebContentClient::did_check_if_traverse_history_step_is_canceled(
    u64 page_id, u64 request_id, i32 step, bool canceled)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_check_if_traverse_history_step_is_canceled({}, request_id, step, canceled);
}

Messages::WebContentClient::DidRequestTraverseTheHistoryByDeltaResponse WebContentClient::did_request_traverse_the_history_by_delta(u64 page_id, i32 delta, Web::HistoryTraversalPrecheck history_traversal_precheck)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto view_id = view->view_id();
        // This request is already a synchronous IPC from WebContent, so defer
        // the UI traversal before it possibly calls back into WebContent for
        // cancelation checks.
        Core::deferred_invoke([view_id, delta, history_traversal_precheck] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            auto check_for_cancelation = ViewImplementation::CheckForCancelation::IfWebContentCannotTraverseTarget;
            if (history_traversal_precheck == Web::HistoryTraversalPrecheck::Needed)
                check_for_cancelation = ViewImplementation::CheckForCancelation::Yes;
            // NB: SourceDocumentSandboxingAlreadyDone only covers the source-document sandboxing
            //     check. If the UI process has to apply the traversal itself, WebContent still needs
            //     to run the cancelable part of the traverse history step prechecks.
            else if (history_traversal_precheck == Web::HistoryTraversalPrecheck::SourceDocumentSandboxingAlreadyDone)
                check_for_cancelation = ViewImplementation::CheckForCancelation::Yes;
            (void)view->traverse_the_history_by_delta(delta, check_for_cancelation);
        });
        return true;
    }

    return false;
}

void WebContentClient::did_request_webdriver_history_traversal(u64 page_id, u64 request_id, i32 delta)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto view_id = view->view_id();
        // This request originates from WebDriver in WebContent. Defer the UI
        // traversal so it can safely call back into WebContent for the
        // cancelation checks from the traverse history step algorithm.
        Core::deferred_invoke([this, page_id, request_id, view_id, delta] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value()) {
                async_complete_webdriver_history_traversal(page_id, request_id, false, false, false);
                return;
            }

            auto complete = [this, page_id, request_id](ViewImplementation::HistoryTraversalOutcome outcome) {
                auto traversal_started = outcome.status == ViewImplementation::HistoryTraversalStatus::Started;
                async_complete_webdriver_history_traversal(
                    page_id,
                    request_id,
                    true,
                    traversal_started && outcome.will_replace_web_content_process,
                    traversal_started && outcome.will_change_top_level_entry);
            };

            auto outcome = view->traverse_the_history_by_delta(delta, ViewImplementation::CheckForCancelation::Yes,
                [this, page_id, request_id](ViewImplementation::HistoryTraversalOutcome outcome) {
                    auto traversal_started = outcome.status == ViewImplementation::HistoryTraversalStatus::Started;
                    async_complete_webdriver_history_traversal(
                        page_id,
                        request_id,
                        true,
                        traversal_started && outcome.will_replace_web_content_process,
                        traversal_started && outcome.will_change_top_level_entry);
                });
            if (!outcome.waiting_for_cancelation_check)
                complete(outcome);
        });
        return;
    }

    async_complete_webdriver_history_traversal(page_id, request_id, false, false, false);
}

Messages::WebContentClient::DidRequestWebdriverLoadUrlFromUiResponse WebContentClient::did_request_webdriver_load_url_from_ui(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto view_id = view->view_id();
        if (url.scheme() != "javascript"sv)
            view->did_start_webdriver_navigation({}, url);
        Core::deferred_invoke([view_id, url = move(url)] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            view->load(url);
        });
        return { JsonValue {} };
    }

    return { Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv) };
}

Messages::WebContentClient::DidRequestWebdriverTraverseHistoryFromUiResponse WebContentClient::did_request_webdriver_traverse_history_from_ui(u64 page_id, i32 delta)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto view_id = view->view_id();
        // This request is already a synchronous IPC from WebContent, so defer the
        // UI traversal before running cancelation checks against WebContent.
        Core::deferred_invoke([view_id, delta] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            (void)view->traverse_the_history_by_delta(delta, ViewImplementation::CheckForCancelation::Yes);
        });
        return { JsonValue {} };
    }

    return { Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv) };
}

Messages::WebContentClient::DidRequestWebdriverMarkWebContentSessionHistoryStaleResponse WebContentClient::did_request_webdriver_mark_web_content_session_history_stale(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->mark_web_content_session_history_stale_for_testing({});
        return { JsonValue {} };
    }

    return { Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv) };
}

Messages::WebContentClient::DidRequestWebdriverSessionHistoryResponse WebContentClient::did_request_webdriver_session_history(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->webdriver_session_history() };

    return { Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv) };
}

void WebContentClient::did_request_webdriver_navigation_completion(u64 page_id, u64 request_id, Optional<u64> page_load_timeout)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->wait_for_webdriver_navigation_completion({}, page_load_timeout, [this, page_id, request_id](Web::WebDriver::Response response) {
            async_complete_webdriver_navigation_completion(page_id, request_id, move(response));
        });
        return;
    }

    async_complete_webdriver_navigation_completion(page_id, request_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
}

void WebContentClient::did_update_resource_count(u64 page_id, i32 count_waiting)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_resource_status_change)
            view->on_resource_status_change(count_waiting);
    }
}

void WebContentClient::did_request_restore_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_restore_window)
            view->on_restore_window();
    }
}

void WebContentClient::did_request_reposition_window(u64 page_id, Gfx::IntPoint position)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_reposition_window)
            view->on_reposition_window(position);
    }
}

void WebContentClient::did_request_resize_window(u64 page_id, Gfx::IntSize size)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_resize_window)
            view->on_resize_window(size);
    }
}

void WebContentClient::did_request_maximize_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_maximize_window)
            view->on_maximize_window();
    }
}

void WebContentClient::did_request_minimize_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_minimize_window)
            view->on_minimize_window();
    }
}

void WebContentClient::did_request_fullscreen_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_fullscreen_window)
            view->on_fullscreen_window();
    }
}

void WebContentClient::did_request_exit_fullscreen(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_exit_fullscreen_window)
            view->on_exit_fullscreen_window();
    }
}

void WebContentClient::did_request_file(u64 page_id, ByteString path, i32 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file)
            view->on_request_file(path, request_id);
    }
}

void WebContentClient::did_request_color_picker(u64 page_id, Color current_color)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_color_picker)
            view->on_request_color_picker(current_color);
    }
}

void WebContentClient::did_request_file_picker(u64 page_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file_picker)
            view->on_request_file_picker(accepted_file_types, allow_multiple_files);
    }
}

void WebContentClient::did_request_select_dropdown(u64 page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_select_dropdown)
            view->on_request_select_dropdown(view->to_widget_position(content_position), minimum_width / view->device_pixel_ratio(), items);
    }
}

void WebContentClient::did_finish_handling_input_event(u64 page_id, Web::EventResult event_result)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_finish_handling_input_event({}, event_result);
        return;
    }

    SiteIsolationManager::the().remote_child_frame_did_finish_handling_input_event(*this, page_id, event_result);
}

void WebContentClient::did_update_input_caret_rect(u64 page_id, Optional<Web::DevicePixelRect> rect)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->set_input_caret_rect({}, rect);
}

void WebContentClient::did_change_theme_color(u64 page_id, Gfx::Color color)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_theme_color_change)
            view->on_theme_color_change(color);
    }
}

void WebContentClient::did_change_background_color(u64 page_id, Gfx::Color color)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_background_color({}, color);
}

void WebContentClient::did_insert_clipboard_entry(u64, Web::Clipboard::SystemClipboardRepresentation entry, String)
{
    Application::the().insert_clipboard_entry(move(entry));
}

void WebContentClient::did_request_clipboard_entries(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        Vector<Web::Clipboard::SystemClipboardItem> items;
        if (auto entries = Application::the().clipboard_entries(); !entries.is_empty())
            items.empend(move(entries));

        view->retrieved_clipboard_entries(request_id, items);
    }
}

void WebContentClient::did_request_primary_paste(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto text = Application::the().clipboard_text(Application::ClipboardType::Selection);
        view->client().async_paste(page_id, text);
    }
}

void WebContentClient::did_update_primary_selection(u64 page_id, String text)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        Application::the().set_clipboard_text(move(text), Application::ClipboardType::Selection);
}

void WebContentClient::did_change_audio_play_state(u64 page_id, Web::HTML::AudioPlayState play_state)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_audio_play_state({}, play_state);
}

void WebContentClient::did_update_navigation_buttons_state(u64 page_id, bool back_enabled, bool forward_enabled)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->should_manage_session_history_in_ui_process())
            return;
        view->did_update_navigation_buttons_state({}, back_enabled, forward_enabled);
    }
}

void WebContentClient::did_update_session_history(u64 page_id, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_update_session_history({}, move(entries), move(used_steps), current_used_step_index);
}

Messages::WebContentClient::DidRequestUiProcessSessionHistoryForTestingResponse WebContentClient::did_request_ui_process_session_history_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->ui_process_session_history_for_testing({}) };

    return { "{}"_string };
}

Messages::WebContentClient::DidRequestSiteIsolationProcessTreeForTestingResponse WebContentClient::did_request_site_isolation_process_tree_for_testing(u64 page_id)
{
    return { SiteIsolationManager::the().dump_process_tree(*this, page_id) };
}

Messages::WebContentClient::DidUpdateSessionHistoryAndRequestUiProcessSessionHistoryForTestingResponse WebContentClient::did_update_session_history_and_request_ui_process_session_history_for_testing(u64 page_id, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_update_session_history_for_testing({}, move(entries), move(used_steps), current_used_step_index);
        return { view->ui_process_session_history_for_testing({}) };
    }

    return { "{}"_string };
}

void WebContentClient::did_set_top_level_session_history(u64 page_id, bool accepted, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_set_top_level_session_history({}, accepted, move(entries), move(used_steps), current_used_step_index);
}

void WebContentClient::did_traverse_the_history_to_step(u64 page_id, i32 step, bool step_was_available, Web::HTML::HistoryStepResult result)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_traverse_the_history_to_step({}, step, step_was_available, result);
}

void WebContentClient::did_reset_session_history_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_reset_session_history_for_testing({});
}

void WebContentClient::did_present_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received backing stores for page {} front={} back={}",
        page_id, front_bitmap_id, back_bitmap_id);
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_allocate_backing_stores({}, front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
    } else {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping backing stores for page {} front={} back={}: no view",
            page_id, front_bitmap_id, back_bitmap_id);
    }
}

Messages::WebContentClient::StartWorkerAgentResponse WebContentClient::start_worker_agent(u64 page_id, Web::HTML::WorkerAgentStartRequest request)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto agent_id = WorkerProcessManager::the().start_worker_agent(*this, page_id, move(request));
        return { agent_id };
    }

    return { 0 };
}

void WebContentClient::close_worker_agent(u64, Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    WorkerProcessManager::the().close_worker_agent(*this, agent_id, owner_token);
}

Optional<ViewImplementation&> WebContentClient::view_for_page_id(u64 page_id, SourceLocation location)
{
    // Don't bother logging anything for the spare WebContent process. It will only receive a load notification for about:blank.
    if (m_views.is_empty())
        return {};

    if (auto view = m_views.get(page_id); view.has_value())
        return *view.value();

    dbgln("WebContentClient::{}: Did not find a page with ID {}", location.function_name(), page_id);
    return {};
}

}
