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
#include <LibRequests/Request.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NavigationLoader.h>
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

static Optional<LexicalPath> choose_download_destination_or_report_error(URL::URL const& url, ByteString const& suggested_filename)
{
    auto destination = Application::the().default_path_for_downloaded_file(suggested_filename);
    if (destination.is_error()) {
        if (!destination.error().is_errno() || destination.error().code() != ECANCELED)
            Application::the().display_error_dialog(ByteString::formatted("Unable to download {}: {}", url, destination.error()));
        return {};
    }

    return destination.release_value();
}

static bool is_download_in_progress(FileDownloader const& file_downloader, u64 download_id)
{
    auto download = file_downloader.download(download_id);
    return download.has_value() && download->status == FileDownloader::DownloadStatus::InProgress;
}

WebContentClient::WebContentClient(NonnullOwnPtr<IPC::Transport> transport, IsPrivate is_private, u64 initial_page_id, Web::HTML::CrossProcessId root_navigable_id)
    : IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(transport))
    , m_is_private(is_private)
    , m_initial_page_id(initial_page_id)
    , m_root_navigable_id(root_navigable_id)
{
    VERIFY(m_initial_page_id > 0);
    clients().set(this);
}

WebContentClient::~WebContentClient()
{
    cancel_navigation_transactions();
    WorkerProcessManager::the().remove_web_content_owner(*this);
    clients().remove(this);

    if (m_is_private == IsPrivate::Yes)
        Application::the().maybe_close_private_browsing_session();
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
    cancel_navigation_transactions();
    fail_renderer_owned_downloads();
}

void WebContentClient::report_unexpected_debugger_response()
{
    // FIXME: Use IPC::ConnectionToServer::did_misbehave() once it provides the
    // same peer-reporting API as IPC::ConnectionFromClient.
    shutdown_with_error(Error::from_string_literal("WebContent sent an unexpected debugger response"));
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
    VERIFY(view.is_private() == m_is_private);
    view.m_client_state.page_index = m_initial_page_id;
    view.traversable().set_id(m_root_navigable_id);
    m_views.set(m_initial_page_id, view);

    if (m_initial_top_level_history_entry_awaiting_view.has_value())
        view.did_create_top_level_traversable({}, m_initial_top_level_history_entry_awaiting_view.release_value());
}

void WebContentClient::set_compositor_connection_id(Badge<Application>, i32 compositor_connection_id)
{
    m_compositor_connection_id = compositor_connection_id;
}

void WebContentClient::register_view(u64 page_id, ViewImplementation& view)
{
    VERIFY(page_id > 0);
    VERIFY(view.is_private() == m_is_private);
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
    SiteIsolationManager::the().remove_page(*this, page_id);

    // A page that still needs a beforeunload check is not a detached
    // background close. It is being closed without waiting for WebContent,
    // e.g. because the user requested a forced close.
    if (auto view = m_views.get(page_id); view.has_value() && (*view)->needs_beforeunload_check())
        m_detached_pages_pending_close.remove(page_id);

    m_views.remove(page_id);
    m_history_recorded_urls_for_current_load.remove(page_id);
    close_server_if_unused();
}

bool WebContentClient::is_renderer_owned_download(u64 page_id, u64 download_id) const
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

void WebContentClient::register_embedded_page(u64 page_id, CanonicalNavigable& child_frame)
{
    m_embedded_pages.set(page_id, child_frame.make_weak_ptr());
    Application::process_manager().cancel_forced_exit(pid());
}

void WebContentClient::unregister_embedded_page(u64 page_id)
{
    m_embedded_pages.remove(page_id);
    close_server_if_unused();
}

CanonicalNavigable* WebContentClient::embedded_page_host(u64 page_id)
{
    auto host = m_embedded_pages.find(page_id);
    if (host == m_embedded_pages.end())
        return nullptr;

    auto* child_frame = host->value.ptr();
    if (!child_frame || !child_frame->has_remote_host() || &child_frame->remote_host_client() != this)
        return nullptr;

    return child_frame;
}

CanonicalNavigable* WebContentClient::navigable_for_page(u64 page_id)
{
    if (auto* child_frame = embedded_page_host(page_id))
        return child_frame;

    if (auto view = view_for_page_id(page_id); view.has_value())
        return &view->traversable();

    return nullptr;
}

Optional<CanonicalNavigable&> WebContentClient::hosted_navigable_for_page(u64 page_id, Web::HTML::CrossProcessId navigable_id)
{
    auto* page_host = navigable_for_page(page_id);
    if (!page_host)
        return {};

    auto navigable = page_host->top_level_traversable().find(navigable_id);
    if (!navigable.has_value())
        return {};

    if (&*navigable == page_host || navigable->is_hosted_by(*this, page_id))
        return *navigable;

    return {};
}

Optional<CanonicalNavigable&> WebContentClient::child_frame(u64 page_id, Web::HTML::CrossProcessId frame_id)
{
    auto* host = navigable_for_page(page_id);
    if (!host)
        return {};

    return host->top_level_traversable().find(frame_id);
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

    for (auto& [page_id, view] : m_views) {
        auto context_id = Web::Compositor::compositor_context_id_for_page(page_id);
        if (!m_compositor_contexts.contains(context_id))
            continue;
        Application::the().update_compositor_viewport(context_id, view->viewport_size().to_type<int>());
        Application::the().update_compositor_display_metadata(context_id, view->display_id(), view->maximum_frames_per_second());
        Application::the().update_compositor_context_visibility(context_id, view->traversable().system_visibility_state());
        view->update_paused_debugger_overlay();
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

bool WebContentClient::send_async_scroll_to_compositor(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Web::Compositor::SnapContainerHandling snap_container_handling)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto handled = Application::the().send_async_scroll_to_compositor(compositor_context_id_for_page(page_id), position, delta_in_device_pixels, snap_container_handling);

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC async_scroll_by page {} returned {} in {} us",
        page_id, handled, timer.elapsed_time().to_microseconds());
    return handled;
}

bool WebContentClient::handle_mouse_event_in_compositor(u64 page_id, Web::MouseEvent const& event)
{
    if (auto target = SiteIsolationManager::the().remote_child_frame_input_target_at(*this, page_id, event.position); target.has_value()) {
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
    if (auto target = SiteIsolationManager::the().remote_child_frame_input_target_at(*this, page_id, event.position); target.has_value()) {
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

void WebContentClient::did_present_bitmap(u64 page_id, Gfx::IntRect rect, Gfx::IntRect damage_rect, i32 bitmap_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC did_paint for page {} bitmap {} rect={}x{} at {},{}",
        page_id, bitmap_id, rect.width(), rect.height(), rect.x(), rect.y());
    if (auto view = view_for_page_id(page_id); view.has_value()) {
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

void WebContentClient::did_request_navigation_start(u64 page_id, Web::HTML::CrossProcessId navigable_id, URL::URL current_url, Web::NavigationTarget target, URL::URL url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request)
{
    auto* target_navigable = navigable_for_page(page_id);
    if (target == Web::NavigationTarget::IFrame) {
        auto child_frame = this->child_frame(page_id, navigable_id);
        target_navigable = child_frame.has_value() ? &*child_frame : nullptr;
    }

    if (!target_navigable
        || target_navigable->id() != navigable_id
        || (start_request.has_value() && start_request->navigable_id != navigable_id)) {
        async_cancel_navigation_params_creation(page_id, navigable_id, navigation_id);
        return;
    }

    auto sequence_number = target_navigable->top_level_traversable().next_sequence_number();
    if (auto const& ongoing_navigation = target_navigable->ongoing_navigation(); ongoing_navigation.has_value()
        && ongoing_navigation->sequence_number != 0
        && ongoing_navigation->navigation_id == navigation_id) {
        // A UI-initiated top-level load records its transaction, under a navigation ID the UI
        // generated, before WebContent enters navigate(). Keep its original admission order now
        // that WebContent has started the navigation.
        sequence_number = ongoing_navigation->sequence_number;
    }

    // A javascript: navigation runs synchronously in the requesting process and never populates an entry.
    // Record its admission without population state, owned by the evaluating process, so its failure or
    // produced document is validated against that process.
    if (!start_request.has_value()) {
        target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
            .url = url,
            .current_url = move(current_url),
            .target = target,
            .navigation_id = navigation_id,
            .sequence_number = sequence_number,
        });
        target_navigable->set_navigation_population_worker(*this, page_id);
        if (target_navigable->is_top_level_traversable()) {
            if (auto view = view_for_page_id(page_id); view.has_value())
                begin_top_level_load(*view, page_id, move(navigation_id), url);
        }
        return;
    }

    target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = move(url),
        .current_url = move(current_url),
        .target = target,
        .navigation_id = navigation_id,
        .start_request = move(start_request),
        .sequence_number = sequence_number,
        .phase = CanonicalNavigable::OngoingNavigation::Phase::AwaitingUnloadCheck,
    });
    target_navigable->set_navigation_population_worker(*this, page_id);
    async_run_navigation_unload_check(page_id, navigable_id, move(navigation_id));
}

void WebContentClient::did_complete_navigation_unload_check(u64 page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || ongoing_navigation->phase != CanonicalNavigable::OngoingNavigation::Phase::AwaitingUnloadCheck
        || ongoing_navigation->population_worker_client.ptr() != this
        || ongoing_navigation->population_worker_page_id != page_id
        || !ongoing_navigation->start_request.has_value()) {
        return;
    }

    auto population_request = Web::HTML::create_navigation_population_request(
        ongoing_navigation->start_request.release_value(),
        Application::the().allocate_ui_process_cross_process_id());
    ongoing_navigation->loader = NavigationLoader::create(m_is_private, move(population_request));
    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;
    async_create_navigation_params(page_id, ongoing_navigation->loader->request());

    // Requesting navigation params starts the fetch, so a view's top-level population begins its recorded
    // load here.
    if (navigable->is_top_level_traversable()) {
        if (auto view = view_for_page_id(page_id); view.has_value())
            begin_top_level_load(*view, page_id, move(navigation_id), ongoing_navigation->loader->request().history_entry.url);
    }
}

void WebContentClient::did_request_navigation_population(u64 page_id, Web::HTML::CrossProcessId navigable_id, URL::URL current_url, Web::NavigationTarget target, Web::HTML::NavigationPopulationRequest request)
{
    auto const& target_url = request.history_entry.url;

    auto* target_navigable = navigable_for_page(page_id);
    Optional<CanonicalNavigable&> child_frame;
    if (target == Web::NavigationTarget::IFrame) {
        child_frame = this->child_frame(page_id, navigable_id);
        target_navigable = child_frame.has_value() ? &*child_frame : nullptr;
    }

    if (!target_navigable
        || target_navigable->id() != navigable_id
        || request.navigable_id != navigable_id) {
        async_cancel_navigation_params_creation(page_id, navigable_id, request.navigation_id);
        return;
    }

    if (auto const& ongoing_navigation = target_navigable->ongoing_navigation(); ongoing_navigation.has_value()
        && ongoing_navigation->navigation_id == request.navigation_id
        && ongoing_navigation->loader) {
        async_cancel_navigation_params_creation(page_id, navigable_id, request.navigation_id);
        return;
    }

    // A reconstructed child navigation was admitted in the populating phase without a request of its own;
    // only the process hosting the child's document may deliver its population request.
    auto continues_reconstructed_child_navigation = target_navigable->ongoing_navigation().has_value()
        && target_navigable->ongoing_navigation()->navigation_id == request.navigation_id
        && target_navigable->ongoing_navigation()->phase == CanonicalNavigable::OngoingNavigation::Phase::Populating
        && !target_navigable->ongoing_navigation()->loader
        && target_navigable->navigation_host_matches(*this, page_id);
    if (continues_reconstructed_child_navigation) {
        auto& ongoing_navigation = *target_navigable->ongoing_navigation();
        ongoing_navigation.url = target_url;
        ongoing_navigation.current_url = move(current_url);
        ongoing_navigation.target = target;
        ongoing_navigation.phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;
        ongoing_navigation.loader = NavigationLoader::create(m_is_private, move(request));
    } else {
        target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
            .url = target_url,
            .current_url = move(current_url),
            .target = target,
            .navigation_id = request.navigation_id,
            .sequence_number = target_navigable->top_level_traversable().next_sequence_number(),
            .phase = CanonicalNavigable::OngoingNavigation::Phase::Populating,
            .loader = NavigationLoader::create(m_is_private, move(request)),
        });
    }

    // The UI process owns the in-parallel population work. Dispatch the document-dependent
    // steps through step 4 to the process with the live source document. The response URL then
    // determines which process receives the task queued by step 5.
    if (!target_navigable->ongoing_navigation()->population_worker_client)
        target_navigable->set_navigation_population_worker(*this, page_id);
    async_create_navigation_params(page_id, target_navigable->ongoing_navigation()->loader->request());

    // Requesting navigation params starts the fetch, so a view's top-level population begins its recorded
    // load here.
    if (target_navigable->is_top_level_traversable()) {
        if (auto view = view_for_page_id(page_id); view.has_value())
            begin_top_level_load(*view, page_id, target_navigable->ongoing_navigation()->navigation_id, target_navigable->ongoing_navigation()->loader->request().history_entry.url);
    }
}

void WebContentClient::did_finish_navigation_params_creation(u64 page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id, Optional<Web::HTML::NavigationPopulationResult> result)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value()) {
        if (result.has_value())
            NavigationLoader::discard(m_is_private, *result);
        return;
    }

    if (!navigable->navigation_population_matches(*this, page_id, navigation_id)) {
        if (result.has_value())
            NavigationLoader::discard(m_is_private, *result);
        return;
    }

    auto end_recorded_load_for_canceled_navigation = [&] {
        if (!navigable->is_top_level_traversable())
            return;
        if (auto view = view_for_page_id(page_id); view.has_value())
            view->did_cancel_loading(navigation_id);
    };

    if (!result.has_value()) {
        end_recorded_load_for_canceled_navigation();
        navigable->clear_ongoing_navigation();
        return;
    }

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || !ongoing_navigation->loader
        || !ongoing_navigation->current_url.has_value()) {
        NavigationLoader::discard(m_is_private, *result);
        end_recorded_load_for_canceled_navigation();
        navigable->clear_ongoing_navigation();
        return;
    }

    // Steps 1-4 have produced final navigation params. Keep the pending entry in
    // sync with redirects before choosing the process that will run step 5.
    navigable->did_finish_navigation_params_creation();
    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::AwaitingResponseBody;
    ongoing_navigation->loader->did_finish_navigation_params_creation(result.release_value());
    ongoing_navigation->url = ongoing_navigation->loader->request().history_entry.url;

    RefPtr self = this;
    ongoing_navigation->loader->acquire_response_body([self, page_id, navigable_id, navigation_id = move(navigation_id)](bool succeeded) mutable {
        auto cancel_navigation = [&](CanonicalNavigable& navigable) {
            if (navigable.is_top_level_traversable()) {
                if (auto view = self->view_for_page_id(page_id); view.has_value())
                    view->did_cancel_loading(navigation_id);
            }
            navigable.clear_ongoing_navigation();
        };
        if (!succeeded) {
            if (auto navigable = self->hosted_navigable_for_page(page_id, navigable_id); navigable.has_value())
                cancel_navigation(*navigable);
            return;
        }
        if (self->continue_navigation_population_in_selected_process(page_id, navigable_id, navigation_id))
            return;
        if (auto navigable = self->hosted_navigable_for_page(page_id, navigable_id); navigable.has_value()) {
            auto const& ongoing_navigation = navigable->ongoing_navigation();
            if (ongoing_navigation.has_value() && ongoing_navigation->navigation_id == navigation_id)
                cancel_navigation(*navigable);
        }
    });
}

void WebContentClient::did_fail_navigation_population(u64 page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;

    // The failure must name the admitted transaction, and must come from a process that owns part of its
    // outcome: the population worker (unload check, javascript: evaluation) or the population host.
    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || !navigable->navigation_owner_matches(*this, page_id)) {
        return;
    }

    // Only a failed population handoff owns the loader's response body.
    if (ongoing_navigation->loader && navigable->navigation_host_matches(*this, page_id))
        ongoing_navigation->loader->reclaim_response_body_after_failed_handoff();

    m_history_recorded_urls_for_current_load.remove(page_id);
    if (navigable->is_top_level_traversable()) {
        if (auto view = ViewImplementation::find_view_for_traversable(navigable->top_level_traversable()); view.has_value()) {
            view->did_cancel_loading(navigation_id);
            return;
        }
    }
    navigable->clear_ongoing_navigation();
}

bool WebContentClient::continue_navigation_population_in_selected_process(u64 page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return false;

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || ongoing_navigation->phase != CanonicalNavigable::OngoingNavigation::Phase::AwaitingResponseBody
        || !ongoing_navigation->loader) {
        return false;
    }

    auto const& result = ongoing_navigation->loader->result();
    auto const& current_url = *ongoing_navigation->current_url;
    auto const& target_url = *ongoing_navigation->url;

    // Step 6 of https://html.spec.whatwg.org/multipage/browsing-the-web.html#process-a-navigate-response
    // A 204 or 205 response does not load a document, so keep the active document's process as the host.
    auto creates_a_document = true;
    if (result.navigation_params.has<Web::HTML::NavigationParamsDescriptor>()) {
        auto status = result.navigation_params.get<Web::HTML::NavigationParamsDescriptor>().response.status;
        creates_a_document = status != 204 && status != 205;
    }

    auto requires_process_swap = false;
    if (creates_a_document) {
        if (navigable->is_top_level_traversable()) {
            requires_process_swap = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target_url, ongoing_navigation->target.value_or(Web::NavigationTarget::TopLevel));
        } else {
            requires_process_swap = SiteIsolationManager::the().child_frame_navigation_requires_process_swap(*navigable, current_url, target_url);
        }
    }

    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;

    if (!requires_process_swap) {
        navigable->set_navigation_host(*this, page_id);
        async_populate_navigation(page_id, ongoing_navigation->loader->request(), ongoing_navigation->loader->take_result());
        return true;
    }

    if (navigable->is_top_level_traversable()) {
        if (auto view = view_for_page_id(page_id); view.has_value()) {
            return view->create_new_process_for_cross_site_navigation(navigation_id);
        } else {
            navigable->clear_ongoing_navigation();
            return false;
        }
    }

    auto current_step = navigable->top_level_traversable().session_history().current_step();
    VERIFY(current_step.has_value());
    auto const* current_entry = navigable->top_level_traversable().session_history().get_the_target_history_entry(*navigable, *current_step);
    VERIFY(current_entry);

    auto remote_process_or_error = Application::the().launch_child_frame_web_content_process(m_is_private, navigable_id, current_entry->document_state.id);
    if (remote_process_or_error.is_error()) {
        warnln("Unable to create WebContent process for child frame navigation: {}", remote_process_or_error.error());
        navigable->clear_ongoing_navigation();
        return false;
    }

    auto remote_process = remote_process_or_error.release_value();
    auto remote_page_id = remote_process.page_id;
    auto remote_client = move(remote_process.client);
    navigable->set_navigation_host(*remote_client, remote_page_id);
    remote_client->register_embedded_page(remote_page_id, *navigable);
    remote_client->async_set_page_parent_context(remote_page_id, Web::Compositor::compositor_context_id_for_page(navigable->reporting_page_id()));
    if (navigable->viewport_rect().has_value()) {
        remote_client->async_set_viewport(
            remote_page_id,
            navigable->viewport_rect()->size(),
            navigable->device_pixel_ratio(),
            Web::ViewportIsFullscreen::No);
    }
    remote_client->async_update_visibility_state(remote_page_id, navigable->id(), navigable->top_level_traversable().system_visibility_state());
    remote_client->async_populate_navigation(remote_page_id, ongoing_navigation->loader->request(), ongoing_navigation->loader->take_result());

    SiteIsolationManager::the().transition_child_frame_to_remote(navigable->reporting_client(), navigable->reporting_page_id(), navigable_id, move(remote_client), remote_page_id);
    return true;
}

void WebContentClient::did_create_child_frame(u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState replicated_state)
{
    auto* host = navigable_for_page(page_id);
    if (!host)
        return;

    host->top_level_traversable().insert(*this, page_id, move(parent_frame_id), move(frame_id), move(replicated_state), *host);
}

void WebContentClient::did_update_child_frame_viewport(u64 page_id, Web::HTML::CrossProcessId frame_id, Web::DevicePixelRect viewport_rect, double device_pixel_ratio)
{
    if (auto child_frame = this->child_frame(page_id, frame_id); child_frame.has_value())
        child_frame->set_viewport(viewport_rect, device_pixel_ratio);
}

void WebContentClient::did_destroy_child_frame(u64 page_id, Web::HTML::CrossProcessId frame_id)
{
    if (auto child_frame = this->child_frame(page_id, frame_id); child_frame.has_value())
        SiteIsolationManager::the().remove_child_frame_subtree(*child_frame);
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
    auto transition = HistoryVisitTransition::Link;
    if (auto view = view_for_page_id(page_id); view.has_value())
        transition = view->m_history_visit_transition_for_current_load;
    Application::history_store(m_is_private).record_visit(url, move(title), UnixDateTime::now(), transition);
    m_history_recorded_urls_for_current_load.set(page_id, normalized_url.release_value());
}

void WebContentClient::begin_top_level_load(ViewImplementation& view, u64 page_id, Optional<Utf16String> navigation_id, URL::URL const& url)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(OptionalNone {});

    m_history_recorded_urls_for_current_load.remove(page_id);

    view.m_history_visit_transition_for_current_load = view.m_history_visit_transition_for_next_load;
    view.m_history_visit_transition_for_next_load = HistoryVisitTransition::Link;
    view.did_start_navigation(move(navigation_id), url);

    view.set_url({}, url);
    view.set_title({}, Utf16String::from_utf8(url.serialize()));
    view.set_favicon({}, {});
    view.set_editing_history_state({}, false, false);

    if (view.on_load_start)
        view.on_load_start();

    for (auto const& [id, listener] : view.m_navigation_listeners) {
        if (listener.on_load_start)
            listener.on_load_start(url);
    }
}

Messages::WebContentClient::DidStartDownloadWithoutRequestResponse WebContentClient::did_start_download_without_request(u64 page_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size)
{
    auto destination = choose_download_destination_or_report_error(url, suggested_filename);
    if (!destination.has_value())
        return { Optional<u64> {} };

    auto& file_downloader = Application::the().file_downloader();
    auto download_id = file_downloader.start_download(is_private(), url, destination.release_value(), total_size);
    if (!is_download_in_progress(file_downloader, download_id))
        return { Optional<u64> {} };

    m_renderer_owned_downloads.set(download_id, page_id);

    auto weak_this = static_cast<Core::EventReceiver&>(*this).make_weak_ptr();
    file_downloader.set_cancel_callback(download_id, [weak_this, page_id, download_id] {
        if (!weak_this)
            return;

        auto& client = static_cast<WebContentClient&>(*weak_this.ptr());
        client.forget_renderer_owned_download(download_id);
        client.async_cancel_download(page_id, download_id);
    });

    return { download_id };
}

Messages::WebContentClient::DidStartDownloadResponse WebContentClient::did_start_download(u64 page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data)
{
    // A download taking over an in-flight population's response body ends that navigation without a document;
    // the reporting process ends its side when its population output says the download was handled. The body
    // identifiers preserve the original RequestServer transfer lease, so only the process and navigation that
    // received that response may claim it, and only while its population is in flight.
    bool matches_in_flight_navigation = false;
    if (auto navigable = hosted_navigable_for_page(page_id, navigable_id); navigable.has_value()) {
        auto const& ongoing_navigation = navigable->ongoing_navigation();
        if (ongoing_navigation.has_value()
            && ongoing_navigation->navigation_id == navigation_id
            && ongoing_navigation->phase == CanonicalNavigable::OngoingNavigation::Phase::Populating
            && ongoing_navigation->loader
            && ongoing_navigation->loader->response_body_matches(request_server_client_id, request_server_request_id)) {
            if (navigable->is_top_level_traversable()) {
                if (auto view = view_for_page_id(page_id); view.has_value())
                    view->did_cancel_loading(ongoing_navigation->navigation_id);
            }
            matches_in_flight_navigation = true;
            navigable->clear_ongoing_navigation();
        }
    }
    if (!matches_in_flight_navigation)
        return { Optional<u64> {} };

    auto destination = choose_download_destination_or_report_error(url, suggested_filename);
    if (!destination.has_value())
        return { Optional<u64> {} };

    auto& file_downloader = Application::the().file_downloader();
    auto download_id = file_downloader.adopt_download(is_private(), url, destination.release_value(), total_size, request_server_client_id, request_server_request_id, initial_data.bytes());
    if (!is_download_in_progress(file_downloader, download_id))
        return { Optional<u64> {} };

    return { download_id };
}

void WebContentClient::did_receive_download_data(u64 page_id, u64 download_id, ByteBuffer data)
{
    if (!is_renderer_owned_download(page_id, download_id))
        return;

    Application::the().file_downloader().append_download_data(download_id, data.bytes());
}

void WebContentClient::did_finish_download(u64 page_id, u64 download_id)
{
    if (!is_renderer_owned_download(page_id, download_id))
        return;

    forget_renderer_owned_download(download_id);
    Application::the().file_downloader().finish_download(download_id);
}

void WebContentClient::did_fail_download(u64 page_id, u64 download_id, String error)
{
    if (!is_renderer_owned_download(page_id, download_id))
        return;

    forget_renderer_owned_download(download_id);
    Application::the().file_downloader().fail_download(download_id, move(error));
}

void WebContentClient::did_finish_loading(u64 page_id, Optional<Utf16String> navigation_id, URL::URL url)
{
    if (url.scheme() == "about"sv && url.paths().size() == 1) {
        if (auto web_ui = WebUI::create(*this, page_id, url.paths().first()); web_ui.is_error())
            warnln("Could not create WebUI for {}: {}", url, web_ui.error());
        else
            m_web_ui = web_ui.release_value();
    }

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (!view->matches_ongoing_navigation(navigation_id))
            return;

        // A replacement process's bootstrap about:blank finishes before the process hosts the committed
        // entry; it must not surface in the view or overwrite the crashed page's URL.
        if (!view->m_client_state.hosts_committed_entry)
            return;

        auto client_url = url;
        // Documents created for inline error content finish with the internal about:error URL; keep the URL the view
        // already shows, which for a failed navigation is the URL that failed to load, including any redirects the
        // navigation was taken through. Firefox/Chromium likewise never surface their internal error-document URLs.
        if (url == URL::about_error())
            client_url = view->url();
        else
            view->set_url({}, url);
        auto title = history_title(view->title(), url);

        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Load finished for page {} at '{}' with title '{}'",
            page_id,
            url,
            title.has_value() ? title->bytes_as_string_view() : "<none>"sv);

        maybe_record_history_visit_for_current_load(page_id, url, title, "load finish"sv);
        if (title.has_value())
            Application::history_store(m_is_private).update_title(url, *title);
        if (view->favicon_hash().has_value())
            Application::history_store(m_is_private).update_favicon(url, *view->favicon_hash());

        view->did_finish_navigation();

        if (view->on_load_finish)
            view->on_load_finish(client_url);

        for (auto const& [id, listener] : view->m_navigation_listeners) {
            if (listener.on_load_finish)
                listener.on_load_finish(client_url);
        }
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
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_request_cursor_change({}, move(cursor));
}

void WebContentClient::did_update_editing_history_state(u64 page_id, bool can_undo, bool can_redo)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->set_editing_history_state({}, can_undo, can_redo);
}

void WebContentClient::did_change_title(u64 page_id, Utf16String title)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(title);

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (!title.is_empty()) {
            auto title_utf8 = title.to_utf8();

            maybe_record_history_visit_for_current_load(page_id, view->url(), title_utf8, "title change"sv);
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Title changed for page {} at '{}' to '{}'",
                page_id,
                view->url(),
                title_utf8);

            Application::history_store(m_is_private).update_title(view->url(), title_utf8);
        }

        if (title.is_empty())
            title = Utf16String::from_utf8(view->url().serialize());

        view->set_title({}, title);
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
    auto open_in_background = modifiers == Web::UIEvents::Mod_PlatformCtrl;
    auto open_in_foreground = modifiers == (Web::UIEvents::Mod_PlatformCtrl | Web::UIEvents::Mod_Shift);
    if (open_in_background || open_in_foreground || target == "_blank"sv) {
        if (auto view = owning_view_for_page_id(page_id); view.has_value())
            view->open_url_in_new_tab(url, open_in_background ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes);
    } else if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->load(url);
    }
}

void WebContentClient::did_middle_click_link(u64 page_id, URL::URL url, ByteString, unsigned)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
}

void WebContentClient::did_request_external_url(u64 page_id, URL::URL url, URL::Origin initiator_origin, bool has_transient_activation)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->handle_external_url({}, move(url), move(initiator_origin), has_transient_activation);
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

void WebContentClient::did_get_source(u64, URL::URL url, URL::URL base_url, Utf16String source)
{
    if (auto view = Application::the().open_blank_new_tab(Web::HTML::ActivateTab::Yes); view.has_value()) {
        auto html = highlight_source(url, base_url, source.to_utf8(), Syntax::Language::HTML);
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

void WebContentClient::did_get_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier, URL::URL base_url, Utf16String source)
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

void WebContentClient::did_pause_debugger(u64 page_id, DebuggerPause pause)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_pause_debugger({});
        if (view->on_debugger_paused)
            view->on_debugger_paused(move(pause));
    }
}

void WebContentClient::did_resume_debugger(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_resume_debugger({});
}

void WebContentClient::did_complete_debugger_breakpoint_operation(u64 page_id, u64 request_id, Optional<String> error)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_complete_debugger_breakpoint_operation(request_id, move(error));
}

void WebContentClient::did_get_debugger_environments(u64 page_id, u64 request_id, Optional<String> error, Vector<DebuggerEnvironment> environments)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto callback = view->m_pending_debugger_environments_requests.take(request_id);
        if (!callback.has_value()) {
            if (view->m_cancelled_debugger_environments_requests.remove(request_id))
                return;
            report_unexpected_debugger_response();
            return;
        }
        if (error.has_value())
            (*callback)(Error::from_string_view(error->bytes_as_string_view()));
        else
            (*callback)(move(environments));
    }
}

void WebContentClient::did_evaluate_javascript_in_debugger_frame(u64 page_id, u64 request_id, Optional<String> error, DebuggerEvaluationResult result)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto callback = view->m_pending_debugger_evaluation_requests.take(request_id);
        if (!callback.has_value()) {
            if (view->m_cancelled_debugger_evaluation_requests.remove(request_id))
                return;
            report_unexpected_debugger_response();
            return;
        }
        if (error.has_value())
            (*callback)(error.release_value());
        else
            (*callback)(move(result));
    }
}

void WebContentClient::did_get_debugger_object_properties(u64 page_id, u64 request_id, Optional<String> error, DebuggerObjectProperties properties)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto callback = view->m_pending_debugger_object_properties_requests.take(request_id);
        if (!callback.has_value()) {
            if (view->m_cancelled_debugger_object_properties_requests.remove(request_id))
                return;
            report_unexpected_debugger_response();
            return;
        }
        if (error.has_value())
            (*callback)(error.release_value());
        else
            (*callback)(move(properties));
    }
}

void WebContentClient::did_get_debugger_source_positions(u64 page_id, u64 request_id, Vector<DebuggerSourcePosition> positions)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto callback = view->m_pending_debugger_source_positions_requests.take(request_id);
        if (!callback.has_value()) {
            if (view->m_cancelled_debugger_source_positions_requests.remove(request_id))
                return;
            report_unexpected_debugger_response();
            return;
        }
        (*callback)(move(positions));
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

void WebContentClient::did_get_selected_text(u64 page_id, u64 request_id, ByteString selection)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_selected_text({}, request_id, move(selection));
}

void WebContentClient::did_get_selected_text_for_lookup(u64 page_id, u64 request_id, Optional<DictionaryLookup> lookup)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_selected_text_for_lookup({}, request_id, move(lookup));
}

void WebContentClient::did_select_word_for_dictionary_lookup(u64 page_id, u64 request_id, bool selected)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_select_word_for_dictionary_lookup({}, request_id, selected);
}

void WebContentClient::did_cut_selected_text(u64 page_id, u64 request_id, ByteString selection)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_cut_selected_text({}, request_id, move(selection));
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

void WebContentClient::did_start_network_request(u64 page_id, u64 request_id, URL::URL url, ByteString method, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority priority)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_request_started)
            view->on_network_request_started(request_id, url, method, request_headers, move(request_body), move(initiator_type), move(referrer_policy), is_navigation_request, priority);
    }
}

void WebContentClient::did_receive_network_response_headers(u64 page_id, u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> response_headers, Requests::CameFromCache came_from_cache)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_network_response_headers_received)
            view->on_network_response_headers_received(request_id, status_code, reason_phrase, response_headers, came_from_cache);
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

void WebContentClient::did_request_alert(u64 page_id, Utf16String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_alert)
            view->on_request_alert(message);
    }
}

void WebContentClient::did_request_confirm(u64 page_id, Utf16String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_confirm)
            view->on_request_confirm(message);
    }
}

void WebContentClient::did_request_prompt(u64 page_id, Utf16String message, Utf16String default_)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_prompt)
            view->on_request_prompt(message, default_);
    }
}

void WebContentClient::did_request_set_prompt_text(u64 page_id, Utf16String message)
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
    return Application::cookie_jar(m_is_private).get_all_cookies_webdriver(url);
}

Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse WebContentClient::did_request_all_cookies_cookiestore(URL::URL url)
{
    return Application::cookie_jar(m_is_private).get_all_cookies_cookiestore(url);
}

Messages::WebContentClient::DidRequestNamedCookieResponse WebContentClient::did_request_named_cookie(URL::URL url, String name)
{
    return Application::cookie_jar(m_is_private).get_named_cookie(url, name);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentClient::did_request_cookie(u64 page_id, URL::URL url, HTTP::Cookie::Source source)
{
    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = Application::cookie_jar(m_is_private).get_cookie(url, source);

    if (source == HTTP::Cookie::Source::NonHttp) {
        if (auto view = view_for_page_id(page_id); view.has_value())
            cookie.cookie_version = view->document_cookie_version(url);
    }

    return cookie;
}

void WebContentClient::did_set_cookie(URL::URL url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    Application::cookie_jar(m_is_private).set_cookie(url, cookie, source);
}

void WebContentClient::did_update_cookie(HTTP::Cookie::Cookie cookie)
{
    Application::cookie_jar(m_is_private).update_cookie(cookie);
}

void WebContentClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    Application::cookie_jar(m_is_private).expire_cookies_with_time_offset(offset);
}

void WebContentClient::did_request_delete_all_cookies(u64 page_id, u64 request_id, URL::URL url)
{
    Application::cookie_jar(m_is_private).delete_all_cookies(url);
    async_did_delete_all_cookies(page_id, request_id);
}

void WebContentClient::did_store_hsts_policy(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    Application::hsts_store(m_is_private).store_policy(domain, policy);
}

Messages::WebContentClient::DidIsKnownHstsHostResponse WebContentClient::did_is_known_hsts_host(String domain)
{
    return Application::hsts_store(m_is_private).is_known_hsts_host(domain);
}

Messages::WebContentClient::DidLoseRequestServerConnectionResponse WebContentClient::did_lose_request_server_connection()
{
    auto handle = connect_new_request_server_client(m_is_private);
    if (handle.is_error()) {
        warnln("Unable to connect a replacement RequestServer client: {}", handle.error());
        return OptionalNone {};
    }

    return handle.release_value();
}

void WebContentClient::did_simulate_worker_request_server_connection_loss(u64 page_id)
{
    VERIFY(Application::web_content_options().is_test_mode == IsTestMode::Yes);
    if (auto result = WorkerProcessManager::the().simulate_request_server_connection_loss_for_testing(*this, page_id); result.is_error()) {
        warnln("Unable to reconnect WebWorker processes to RequestServer: {}", result.error());
        VERIFY_NOT_REACHED();
    }
}

StorageJar* WebContentClient::storage_jar_for_page(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint)
{
    if (storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage) {
        if (auto* navigable = navigable_for_page(page_id))
            return &navigable->top_level_traversable().session_storage();
        return nullptr;
    }

    return &Application::storage_jar(m_is_private);
}

Messages::WebContentClient::DidRequestStorageItemResponse WebContentClient::did_request_storage_item(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key)
{
    auto* storage_jar = storage_jar_for_page(page_id, storage_endpoint);
    if (!storage_jar)
        return Optional<Utf16String> {};
    return storage_jar->get_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidSetStorageItemResponse WebContentClient::did_set_storage_item(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key, Utf16String value)
{
    auto* storage_jar = storage_jar_for_page(page_id, storage_endpoint);
    if (!storage_jar)
        return WebView::StorageOperationError::QuotaExceededError;
    return storage_jar->set_item(storage_endpoint, storage_key, bottle_key, value);
}

void WebContentClient::did_remove_storage_item(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key)
{
    if (auto* storage_jar = storage_jar_for_page(page_id, storage_endpoint))
        storage_jar->remove_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidRequestStorageKeysResponse WebContentClient::did_request_storage_keys(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    auto* storage_jar = storage_jar_for_page(page_id, storage_endpoint);
    if (!storage_jar)
        return Vector<Utf16String> {};
    return storage_jar->get_all_keys(storage_endpoint, storage_key);
}

void WebContentClient::did_clear_storage(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    if (auto* storage_jar = storage_jar_for_page(page_id, storage_endpoint))
        storage_jar->clear_storage_key(storage_endpoint, storage_key);
}

Messages::WebContentClient::DidRequestStorageUsageResponse WebContentClient::did_request_storage_usage(u64, String storage_key)
{
    return Application::storage_jar(m_is_private).usage(storage_key);
}

void WebContentClient::did_change_storage_item(u64 page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String url, Optional<Utf16String> key, Optional<Utf16String> old_value, Optional<Utf16String> new_value)
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
            .key = key.has_value() ? Optional<String> { key->to_utf8() } : Optional<String> {},
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
        if (client.is_private() != m_is_private)
            return IterationDecision::Continue;
        client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });
    WorkerProcessManager::the().broadcast_channel_message_from_web_content(message, m_is_private);
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, bool clone_session_storage)
{
    auto new_page_id = Application::the().allocate_page_id();
    String handle;
    auto opener_view = owning_view_for_page_id(page_id);
    if (opener_view.has_value()) {
        if (opener_view->on_new_web_view)
            handle = opener_view->on_new_web_view(activate_tab, hints, new_page_id);
    }

    auto view = view_for_page_id(new_page_id);
    if (!view.has_value())
        return { {}, {}, Web::HTML::VisibilityState::Hidden, move(handle) };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
    // 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]
    if (clone_session_storage && opener_view.has_value())
        view->traversable().clone_session_storage_from(opener_view->traversable());

    auto root_navigable_id = Application::the().allocate_ui_process_cross_process_id();
    view->traversable().set_id(root_navigable_id);

    return { new_page_id, root_navigable_id, view->traversable().system_visibility_state(), move(handle) };
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
    SiteIsolationManager::the().remove_page(*this, page_id);
    unregister_embedded_page(page_id);
    m_detached_pages_pending_close.remove(page_id);

    if (auto registered_view = m_views.get(page_id); registered_view.has_value()) {
        auto view = *registered_view;
        view->did_close_browsing_context({});
        if (view->on_close)
            view->on_close();
    }

    close_server_if_unused();
}

void WebContentClient::did_change_needs_beforeunload_check(u64 page_id, bool needs_beforeunload_check)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_needs_beforeunload_check({}, needs_beforeunload_check);
}

void WebContentClient::webdriver_user_prompt_handling_complete(u64 page_id, u64 request_id, Web::WebDriver::Response response)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_complete_webdriver_user_prompt_handling({}, request_id, move(response));
}

void WebContentClient::webdriver_command_complete(u64 page_id, u64 command_id, Web::WebDriver::Response response)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_complete_webdriver_content_command({}, command_id, move(response));
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

void WebContentClient::did_request_reposition_window(u64 page_id, Gfx::IntPoint position, u64 completion_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_reposition_window)
            view->on_reposition_window(position);
    }
    async_did_complete_window_rect_request(page_id, completion_id);
}

void WebContentClient::did_request_resize_window(u64 page_id, Gfx::IntSize size, u64 completion_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_resize_window)
            view->on_resize_window(size);
    }
    async_did_complete_window_rect_request(page_id, completion_id);
}

void WebContentClient::did_request_maximize_window(u64 page_id, u64 completion_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_maximize_window)
            view->on_maximize_window();
    }
    async_did_complete_window_rect_request(page_id, completion_id);
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

void WebContentClient::did_request_geolocation_position(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_geolocation_position)
            view->on_request_geolocation_position(request_id);
    }
}

void WebContentClient::did_cancel_geolocation_position_request(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_cancel_geolocation_position_request)
            view->on_cancel_geolocation_position_request(request_id);
    }
}

void WebContentClient::did_start_geolocation_position_watch(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_start_geolocation_position_watch)
            view->on_start_geolocation_position_watch(request_id);
    }
}

void WebContentClient::did_stop_geolocation_position_watch(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_stop_geolocation_position_watch)
            view->on_stop_geolocation_position_watch(request_id);
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

    if (auto* child_frame = embedded_page_host(page_id))
        child_frame->reporting_client().did_finish_handling_input_event(child_frame->reporting_page_id(), event_result);
}

void WebContentClient::did_update_input_method_state(u64 page_id, Optional<Web::DevicePixelRect> caret_rect, bool is_enabled, i32 cursor_position, i32 anchor_position, Utf16String text_before_cursor, Utf16String text_after_cursor)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->set_input_method_state({}, { is_enabled, cursor_position, anchor_position, move(text_before_cursor), move(text_after_cursor), caret_rect });
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

void WebContentClient::did_insert_clipboard_item(u64 page_id, Web::Clipboard::SystemClipboardItem item, String)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->insert_clipboard_item(move(item));
}

void WebContentClient::did_request_clipboard_entries(u64 page_id, u64 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        Vector<Web::Clipboard::SystemClipboardItem> items;
        if (auto entries = view->clipboard_entries(); !entries.is_empty())
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

void WebContentClient::did_change_screen_wake_lock_state(u64 page_id, Web::ScreenWakeLockState wake_lock_state)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_screen_wake_lock_state({}, wake_lock_state);
}

void WebContentClient::did_create_top_level_traversable(u64 page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    if (page_id == m_initial_page_id && navigable_id == m_root_navigable_id) {
        if (m_did_create_initial_top_level_traversable)
            return;
        m_did_create_initial_top_level_traversable = true;

        if (m_views.is_empty()) {
            m_initial_top_level_history_entry_awaiting_view = move(initial_history_entry);
            return;
        }
    }

    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value() || !navigable->is_top_level_traversable())
        return;

    auto view = ViewImplementation::find_view_for_traversable(navigable->top_level_traversable());
    if (!view.has_value())
        return;
    view->did_create_top_level_traversable({}, move(initial_history_entry));
}

void WebContentClient::did_update_session_history_entry_navigation_api_state(u64 page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().update_session_history_entry_navigation_api_state(*navigable, entry_identity, move(navigation_api_state));
}

void WebContentClient::did_update_session_history_entry_scroll_restoration_mode(u64 page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().update_session_history_entry_scroll_restoration_mode(*navigable, entry_identity, scroll_restoration_mode);
}

void WebContentClient::did_update_session_history_entry_document_state_navigable_target_name(u64 page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Utf16String navigable_target_name)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().update_session_history_entry_document_state_navigable_target_name(*navigable, entry_identity, move(navigable_target_name));
}

void WebContentClient::did_set_session_history_entry_document_state_reload_pending(u64 page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_api_key, bool reload_pending)
{
    auto navigable = hosted_navigable_for_page(page_id, navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().set_session_history_entry_document_state_reload_pending(*navigable, navigation_api_key, reload_pending);
}

void WebContentClient::did_request_set_system_visibility_state(u64 page_id, Web::HTML::VisibilityState visibility_state)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->set_system_visibility_state(visibility_state);
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

void WebContentClient::did_reset_session_history_for_testing(u64 page_id, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_reset_session_history_for_testing({}, move(active_entry));
}

void WebContentClient::request_history_operation(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->request_history_operation({}, *this, page_id, operation_id, move(parameters));
}

void WebContentClient::history_operation_ready(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult result)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->did_receive_history_operation_ready({}, *this, page_id, operation_id, move(result));
}

void WebContentClient::history_step_unload_cancelation_result(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->did_receive_history_step_unload_cancelation_result({}, *this, page_id, operation_id, result);
}

void WebContentClient::changing_navigable_history_job_ready(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->did_receive_changing_navigable_history_job_ready({}, *this, page_id, operation_id, navigable_id, disposition);
}

void WebContentClient::changing_navigable_continuation_applied(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->did_receive_changing_navigable_continuation_applied({}, *this, page_id, operation_id, navigable_id, move(activated_navigable_state), move(previous_entry_persisted_state));
}

void WebContentClient::nonchanging_navigable_history_state_updated(u64 page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto view = owning_view_for_page_id(page_id); view.has_value())
        view->did_receive_nonchanging_navigable_history_state_updated({}, *this, page_id, operation_id, navigable_id);
}

Messages::WebContentClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse WebContentClient::did_request_capture_session_history_snapshot_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->capture_session_history_snapshot_for_testing({}) };

    return { false };
}

Messages::WebContentClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse WebContentClient::did_request_restore_session_history_snapshot_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->restore_captured_session_history_snapshot_for_testing({}) };

    return { false };
}

Messages::WebContentClient::DidRequestRegisterSessionStoreTabForTestingResponse WebContentClient::did_request_register_session_store_tab_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->register_session_store_tab_for_testing({}) };

    return { false };
}

Messages::WebContentClient::DidRequestSessionStoreTabStateForTestingResponse WebContentClient::did_request_session_store_tab_state_for_testing(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        return { view->session_store_tab_state_for_testing({}) };

    return { "{}"_string };
}

void WebContentClient::did_present_backing_stores(u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received {} backing stores for page {}", backing_stores.size(), page_id);
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->did_allocate_backing_stores({}, move(bitmap_ids), move(backing_stores));
    } else {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping {} backing stores for page {}: no view", backing_stores.size(), page_id);
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

Optional<ViewImplementation&> WebContentClient::owning_view_for_page_id(u64 page_id)
{
    auto* navigable = navigable_for_page(page_id);
    if (!navigable)
        return {};

    return ViewImplementation::find_view_for_traversable(navigable->top_level_traversable());
}

}
