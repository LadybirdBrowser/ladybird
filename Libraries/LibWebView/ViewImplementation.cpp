/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibURL/Parser.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/ErrorHTML.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Menu.h>
#include <LibWebView/PausedDebuggerOverlay.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentTestClient.h>

namespace WebView {

static HashMap<u64, ViewImplementation*>& all_views()
{
    static NeverDestroyed<HashMap<u64, ViewImplementation*>> views;
    return *views;
}

static void fail_webdriver_content_commands_after_process_replacement(HashMap<u64, NonnullRefPtr<WebContentPage>> const& commands)
{
    for (auto const& command : commands)
        Application::the().complete_webdriver_content_command(command.key, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent was replaced while executing the command"sv));
}

static void fail_webdriver_content_commands_after_window_close(HashMap<u64, NonnullRefPtr<WebContentPage>> const& commands)
{
    for (auto const& command : commands)
        Application::the().complete_webdriver_content_command(command.key, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while executing the command"sv));
}

static u64 s_view_count = 1; // This has to start at 1 for Firefox DevTools.

static Utf16String generate_navigation_id()
{
    auto uuid = Web::Crypto::generate_random_uuid();
    return Utf16String::from_ascii_without_validation(uuid.bytes());
}

void ViewImplementation::for_each_view(Function<IterationDecision(ViewImplementation&)> callback)
{
    for (auto& view : all_views()) {
        if (callback(*view.value) == IterationDecision::Break)
            break;
    }
}

Optional<ViewImplementation&> ViewImplementation::find_view_by_id(u64 id)
{
    if (auto view = all_views().get(id); view.has_value())
        return *view.value();
    return {};
}

ViewImplementation::ViewImplementation(IsPrivate is_private)
    : m_is_private(is_private)
    , m_session(Application::session_for_new_view(is_private))
    , m_document_cookie_version_buffer(Core::create_shared_version_buffer())
    , m_view_id(s_view_count++)
{
    all_views().set(m_view_id, this);
    m_top_level_traversable.set_view({}, *this);

    initialize_context_menus();

    m_repeated_crash_timer = Core::Timer::create_single_shot(1000, [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    });

    m_top_level_traversable.on_session_history_changed = [this] {
        notify_session_history_changed();
    };
}

ViewImplementation::~ViewImplementation()
{
    TabPerformanceMonitor::forget_view(view_id());
    m_top_level_traversable.clear_ongoing_navigation();
    m_top_level_traversable.discard_pending_host();
    cancel_all_native_geolocation_requests();

    if (!m_client_state.client_handle.is_empty())
        Application::the().notify_webdriver_window_closed(m_client_state.client_handle);

    all_views().remove(m_view_id);

    m_top_level_traversable.discard_displaced_document_host();
    m_top_level_traversable.discard_opener_pages();
    if (m_client_state.page)
        client().unregister_view(page_id());

    // A headless parent can own and destroy its child view without the child receiving a browsing-context-close
    // notification. Do not strand a WebDriver command which raced with that teardown.
    fail_webdriver_content_commands_after_window_close(m_pending_webdriver_commands);
    fail_webdriver_content_commands_after_window_close(m_pending_webdriver_crash_commands);
}

WebContentPage& ViewImplementation::page() const
{
    VERIFY(m_client_state.page);
    return *m_client_state.page;
}

WebContentClient& ViewImplementation::client()
{
    return page().client();
}

WebContentClient const& ViewImplementation::client() const
{
    return page().client();
}

Web::PageId ViewImplementation::page_id() const
{
    return page().id();
}

void ViewImplementation::set_url(URL::URL url)
{
    if (m_url == url)
        return;

    m_url = move(url);
    update_bookmark_action();

    if (on_url_change)
        on_url_change(m_url);
}

void ViewImplementation::set_title(Badge<WebContentPage>, Utf16String title)
{
    if (m_title == title)
        return;

    m_title = move(title);

    if (on_title_change)
        on_title_change(m_title);
}

void ViewImplementation::set_favicon(Badge<WebContentPage>, Optional<Gfx::Bitmap const&> favicon)
{
    m_favicon_hash.clear();

    if (favicon.has_value()) {
        if (auto favicon_png = Gfx::PNGWriter::encode(*favicon); !favicon_png.is_error())
            m_favicon_hash = m_session->favicon_store->add_favicon(favicon_png.release_value());

        if (m_favicon_hash.has_value()) {
            if (m_is_private == IsPrivate::No)
                Application::bookmark_store().update_favicon(m_url, *m_favicon_hash);
            m_session->history_store->update_favicon(m_url, *m_favicon_hash);
        }
    }

    if (on_favicon_change)
        on_favicon_change(favicon);
}

bool ViewImplementation::create_new_process_for_cross_site_navigation(Utf16String const& navigation_id)
{
    auto& ongoing_navigation = m_top_level_traversable.ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || ongoing_navigation->phase != CanonicalNavigable::OngoingNavigation::Phase::Populating
        || !ongoing_navigation->loader) {
        return false;
    }

    auto request = ongoing_navigation->loader->request();
    auto url = request.history_entry.url;
    ongoing_navigation->url = url;

    auto pending_webdriver_commands = move(m_pending_webdriver_commands);
    auto pending_webdriver_crash_commands = move(m_pending_webdriver_crash_commands);
    auto fail_pending_webdriver_commands = ScopeGuard([&] {
        fail_webdriver_content_commands_after_process_replacement(pending_webdriver_commands);
        fail_webdriver_content_commands_after_process_replacement(pending_webdriver_crash_commands);
    });

    dump_session_history("before-process-swap"sv);

    if (m_client_state.has_usable_bitmap) {
        // Keep showing the old page until the new WebContent process paints its first frame.
        m_backup_shared_image_buffer = move(m_client_state.front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }

    // The outgoing process keeps displaying the traversable's document until the UI process has unloaded it there,
    // before the document the new process populates activates.
    RefPtr<WebContentPage> displaced_page = m_client_state.page;
    if (displaced_page) {
        fail_pending_debugger_requests();
        displaced_page->client().keep_view_page_for_displaced_document(displaced_page->id(), m_top_level_traversable);
    }

    reset_page_media_state();

    // Replies from the replaced process will never arrive. Complete the in-flight operations so the
    // traversal queue can serve the new process.
    m_top_level_traversable.abandon_history_operations();
    if (displaced_page)
        m_top_level_traversable.set_displaced_document_host(*displaced_page);

    Optional<Web::HTML::CrossProcessId> initial_document_state_id;
    if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
        initial_document_state_id = current_entry->document_state.id;
    initialize_client(CreateNewClient::Yes, initial_document_state_id);
    VERIFY(m_client_state.page);

    if (on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();

    handle_resize();

    auto navigation_still_awaits_population = [&] {
        auto const& current_navigation = m_top_level_traversable.ongoing_navigation();
        return current_navigation.has_value()
            && current_navigation->navigation_id == navigation_id
            && current_navigation->phase == CanonicalNavigable::OngoingNavigation::Phase::Populating
            && current_navigation->loader;
    };

    // Replacing WebContent can synchronously abandon history operations, and the process-change callback can reenter
    // navigation. Do not expose the target as loading if that already canceled or superseded this navigation.
    if (!navigation_still_awaits_population())
        return false;

    set_loading_state(true);
    m_last_stopped_load_url.clear();
    set_url(url);

    // Loading-state and URL callbacks can likewise reenter navigation. Only transfer the response body if this is
    // still the navigation for which the process was created.
    if (!navigation_still_awaits_population())
        return false;

    begin_webdriver_navigation(WebDriverNavigationCompletionSource::Load);
    m_top_level_traversable.set_navigation_host(page());
    auto& current_navigation = *m_top_level_traversable.ongoing_navigation();
    auto result = current_navigation.loader->take_result();
    dump_session_history("process-swap-load"sv);
    client().async_populate_navigation(page_id(), move(request), move(result));
    dump_session_history("after-process-swap-load"sv);
    return true;
}

void ViewImplementation::replace_web_content_process_for_history_traversal(Web::HTML::CrossProcessId target_document_state_id)
{
    auto pending_webdriver_commands = move(m_pending_webdriver_commands);
    auto pending_webdriver_crash_commands = move(m_pending_webdriver_crash_commands);

    dump_session_history("before-history-traversal-process-swap"sv);

    if (m_client_state.has_usable_bitmap) {
        m_backup_shared_image_buffer = move(m_client_state.front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }

    // The outgoing process keeps displaying the traversable's document until the UI process has unloaded it there,
    // before the document the new process populates activates.
    if (m_client_state.page) {
        client().keep_view_page_for_displaced_document(page_id(), m_top_level_traversable);
        m_top_level_traversable.set_displaced_document_host(page());
    }

    reset_page_media_state();
    // NB: Preserve the in-flight traversal operations so crash recovery can redispatch them to the
    //     replacement process.
    initialize_client(CreateNewClient::Yes, target_document_state_id);
    VERIFY(m_client_state.page);

    if (on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();

    handle_resize();
    dump_session_history("after-history-traversal-process-swap"sv);

    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_commands);
    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_crash_commands);
}

void ViewImplementation::server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size, Gfx::IntRect damage_rect)
{
    bool did_swap_bitmap = false;
    auto previous_front_bitmap_id = m_client_state.front_bitmap.id;
    auto bitmap_index = m_client_state.other_bitmaps.find_first_index_if([bitmap_id](auto const& bitmap) { return bitmap_id == bitmap.id; });
    if (bitmap_index.has_value()) {
        m_client_state.has_usable_bitmap = true;
        m_client_state.other_bitmaps[*bitmap_index].last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_client_state.other_bitmaps[*bitmap_index], m_client_state.front_bitmap);
        m_backup_shared_image_buffer = nullptr;
        did_swap_bitmap = true;
    }

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received presented bitmap {} for page {} size={}x{} did_swap={} front={}",
        bitmap_id, page_id(), size.width(), size.height(), did_swap_bitmap, m_client_state.front_bitmap.id);

    auto bitmap_to_release = did_swap_bitmap ? previous_front_bitmap_id : bitmap_id;
    if (!defer_backing_store_release(bitmap_to_release))
        release_backing_store(bitmap_to_release);

    if (did_swap_bitmap)
        did_accept_presented_backing_store(bitmap_id, damage_rect);
    if (did_swap_bitmap && m_crash_state.has_value() && m_crash_state->recovery_started && !m_top_level_traversable.has_pending_host())
        set_crash_state({});
    if (did_swap_bitmap)
        TabPerformanceMonitor::did_present(view_id());

    if (did_swap_bitmap && on_ready_to_paint)
        on_ready_to_paint();
}

void ViewImplementation::release_backing_store(i32 bitmap_id)
{
    client().notify_presented_bitmap_ready_to_paint(page_id(), bitmap_id);
}

void ViewImplementation::set_window_position(Gfx::IntPoint position)
{
    if (!m_client_state.page)
        return;

    client().async_set_window_position(page_id(), position.to_type<Web::DevicePixels>());
}

void ViewImplementation::set_window_size(Gfx::IntSize size)
{
    if (!m_client_state.page)
        return;

    client().async_set_window_size(page_id(), size.to_type<Web::DevicePixels>());
}

void ViewImplementation::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    if (!m_client_state.page)
        return;

    if (m_top_level_traversable.system_visibility_state() == visibility_state)
        return;

    m_top_level_traversable.set_system_visibility_state(visibility_state);
    Application::the().update_compositor_context_visibility(client().compositor_context_id_for_page(page_id()), visibility_state);
}

void ViewImplementation::set_has_system_focus(bool has_system_focus)
{
    if (!m_client_state.page)
        return;
    m_top_level_traversable.set_has_system_focus(has_system_focus, {});
}

void ViewImplementation::load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash(url);
    set_loading_state(true);
    auto navigation_id = generate_navigation_id();
    m_top_level_traversable.set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = url,
        .navigation_id = navigation_id,
        .sequence_number = m_top_level_traversable.next_sequence_number(),
    });
    m_last_stopped_load_url.clear();
    if (url.scheme() != "javascript"sv)
        set_url(url);
    dump_session_history("load"sv);
    client().async_load_url(page_id(), url, history_handling, move(navigation_id));
}

void ViewImplementation::load_from_user_input(URL::URL const& url)
{
    if (!is_url_handled_internally(url)) {
        handle_external_url_from_user_input(url);
        return;
    }

    load(url);
}

void ViewImplementation::load_from_user_input(StringView input)
{
    load_from_user_input(input, sanitize_url(input, Application::settings().search_engine()));
}

void ViewImplementation::load_from_user_input(StringView input, Optional<URL::URL> fallback_url)
{
    auto classified_input = classify_user_input(input);

    if (classified_input.classification != UserInputClassification::ExternalURL) {
        if (fallback_url.has_value())
            load(*fallback_url);
        else
            load_navigation_error_page(input);
        return;
    }

    VERIFY(classified_input.url.has_value());
    auto external_url = classified_input.url.release_value();

    if (fallback_url.has_value() && *fallback_url == external_url) {
        handle_external_url_from_user_input(external_url);
        return;
    }

    auto input_string = MUST(String::from_utf8(input));
    auto view_id = m_view_id;
    handle_external_url_from_user_input(external_url, [view_id, input = move(input_string), fallback_url = move(fallback_url)] {
        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value())
            return;

        if (fallback_url.has_value())
            view->load(*fallback_url);
        else
            view->load_navigation_error_page(input);
    });
}

void ViewImplementation::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
{
    if (!is_url_handled_internally(url)) {
        handle_external_url_from_user_input(url);
        return;
    }

    Application::the().open_url_in_new_tab(url, activate_tab);
}

void ViewImplementation::open_url_in_new_window(URL::URL const& url, IsPrivate is_private)
{
    if (!is_url_handled_internally(url)) {
        handle_external_url_from_user_input(url);
        return;
    }

    Application::the().open_url_in_new_window(url, is_private);
}

void ViewImplementation::load_html(StringView html)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    set_loading_state(true);
    auto navigation_id = generate_navigation_id();
    m_top_level_traversable.set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = URL::about_srcdoc(),
        .navigation_id = navigation_id,
        .sequence_number = m_top_level_traversable.next_sequence_number(),
    });
    m_last_stopped_load_url.clear();
    client().async_load_html(page_id(), html, navigation_id.utf16_view());
}

void ViewImplementation::load_navigation_error_page(StringView text)
{
    auto message = MUST(String::formatted("Failed to load \"{}\"", text));

    StringBuilder builder;
    builder.appendff(ERROR_HTML_HEADER, ""sv, ERROR_SVG, message);
    builder.append("<p>If you were trying to enter a search query, please enable search in <a href=\"about:settings#search\">settings</a>.</p>"sv);
    builder.append(ERROR_HTML_FOOTER);
    load_html(builder.string_view());
}

void ViewImplementation::reload()
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Reload;

    if (m_last_stopped_load_url.has_value()) {
        // AD-HOC: If a UI-requested navigation was stopped before its document committed, WebContent still considers
        //         the previous document active. Reissue the stopped URL instead of reloading that previous document.
        auto url = m_last_stopped_load_url.release_value();
        load(url, Web::Bindings::NavigationHistoryBehavior::Replace);
        return;
    }

    if (m_crash_state.has_value() && m_crash_state->navigation_to_retry.has_value()) {
        load(m_crash_state->navigation_to_retry.release_value());
        return;
    }

    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    set_loading_state(true);
    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    Optional<URL::URL> ongoing_url;
    if (m_top_level_traversable.ongoing_navigation().has_value())
        ongoing_url = move(m_top_level_traversable.ongoing_navigation()->url);
    else if (current_entry)
        ongoing_url = current_entry->url;
    m_top_level_traversable.set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = move(ongoing_url),
        .sequence_number = m_top_level_traversable.next_sequence_number(),
    });
    if (m_crash_state.has_value()) {
        prepare_for_navigation_after_crash();
        recover_current_session_history_entry_with_history_operation();
        return;
    }

    m_top_level_traversable.prepare_for_reload();
    update_navigation_action_state();
    dump_session_history("reload-mark-current-entry-reload-pending"sv);
    client().async_reload(page_id());
}

void ViewImplementation::stop_loading()
{
    if (!m_is_loading)
        return;
    // Only a stopped navigation that never activated its document needs reissuing on reload; a stopped
    // active-document load reloads through the session history.
    if (m_top_level_traversable.ongoing_navigation().has_value())
        m_last_stopped_load_url = m_top_level_traversable.ongoing_navigation()->url;
    else
        m_last_stopped_load_url = {};
    if (cancel_uncommitted_top_level_navigation("stop-loading"sv, true))
        return;
    set_loading_state(false);
    m_top_level_traversable.clear_ongoing_navigation();
    m_top_level_traversable.clear_active_document_load();
    client().async_stop_loading(page_id());
}

void ViewImplementation::traverse_the_history_by_delta(
    int delta,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    m_top_level_traversable.traverse_the_history_by_delta(delta, check_for_cancelation, move(on_ready));
}

bool ViewImplementation::cancel_uncommitted_top_level_navigation_for_browser_traversal()
{
    auto process_hosts_committed_entry = !m_top_level_traversable.has_pending_host();
    auto canceled = cancel_uncommitted_top_level_navigation("traverse-canceled-pending-navigation"sv, true, ReconstructCanceledNavigation::No);
    VERIFY(canceled);
    return !process_hosts_committed_entry;
}

void ViewImplementation::traverse_the_history_to_step(
    i32 step,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    m_top_level_traversable.traverse_the_history_to_step(step, check_for_cancelation, move(on_ready));
}

void ViewImplementation::will_apply_history_traversal_step(Web::HTML::CrossProcessId operation_id)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    if (m_webdriver_navigation_observation.has_value()
        && m_webdriver_navigation_observation->completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
        m_webdriver_navigation_observation->history_operation_id = operation_id;
    } else {
        begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal, operation_id);
    }
    update_navigation_action_state();
    dump_session_history("traverse-apply-history-step"sv);
}

void ViewImplementation::did_resume_history_traversal(Web::HTML::CrossProcessId operation_id)
{
    if (m_webdriver_navigation_observation.has_value()
        && m_webdriver_navigation_observation->completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
        m_webdriver_navigation_observation->history_operation_id = operation_id;
        return;
    }

    if (!m_webdriver_navigation_observation.has_value()
        || m_webdriver_navigation_observation->completion_source != WebDriverNavigationCompletionSource::HistoryTraversal) {
        begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal, operation_id);
        return;
    }

    m_webdriver_navigation_observation->history_operation_id = operation_id;
}

void ViewImplementation::did_apply_top_level_history_traversal_step(Web::HTML::CrossProcessId operation_id)
{
    complete_webdriver_history_traversal(operation_id);
}

void ViewImplementation::did_finish_history_traversal(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result)
{
    if (result == Web::HTML::HistoryStepResult::Applied) {
        if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
            set_url(current_entry->url);
    }

    complete_webdriver_history_traversal(operation_id);
    update_navigation_action_state();
    if (on_browser_history_traversal_complete)
        on_browser_history_traversal_complete();
    dump_session_history(result == Web::HTML::HistoryStepResult::Applied
            ? "webcontent-history-step-applied"sv
            : "webcontent-history-step-canceled"sv);
}

Vector<ViewImplementation::SessionHistoryTraversalMenuItem> ViewImplementation::session_history_traversal_menu_items(int direction) const
{
    VERIFY(direction == -1 || direction == 1);

    auto current_used_step_index = m_top_level_traversable.session_history().current_used_step_index();
    if (!current_used_step_index.has_value())
        return {};

    Vector<SessionHistoryTraversalMenuItem> items;
    auto append_item = [&](i32 target_step, TraversableSessionHistory::Entry const& target_entry) {
        auto history_entry = m_session->history_store->entry_for_url(target_entry.url);
        auto url = target_entry.url.serialize();
        auto title = history_entry.has_value() && history_entry->title.has_value() && !history_entry->title->is_empty()
            ? move(*history_entry->title)
            : url;
        items.append({
            target_step,
            move(title),
            move(url),
            history_entry.has_value() ? move(history_entry->favicon_png) : OptionalNone {},
        });
    };

    if (direction < 0) {
        for (size_t target_step_index = *current_used_step_index; target_step_index > 0; --target_step_index) {
            auto target_step = m_top_level_traversable.session_history().step_at(target_step_index - 1);
            if (!target_step.has_value())
                continue;
            auto const* target_entry = m_top_level_traversable.session_history().top_level_entry_for_step(*target_step);
            if (!target_entry)
                continue;
            append_item(*target_step, *target_entry);
        }
    } else {
        for (size_t target_step_index = *current_used_step_index + 1; target_step_index < m_top_level_traversable.session_history().used_step_count(); ++target_step_index) {
            auto target_step = m_top_level_traversable.session_history().step_at(target_step_index);
            if (!target_step.has_value())
                continue;
            auto const* target_entry = m_top_level_traversable.session_history().top_level_entry_for_step(*target_step);
            if (!target_entry)
                continue;
            append_item(*target_step, *target_entry);
        }
    }

    return items;
}

void ViewImplementation::zoom_in()
{
    if (m_zoom_level >= ZOOM_MAX_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level + ZOOM_STEP) * 100) / 100.0;
    update_zoom();

    if (m_is_private == IsPrivate::No)
        Application::settings().set_zoom_for_host(current_host_for_settings(), m_zoom_level);
}

void ViewImplementation::zoom_out()
{
    if (m_zoom_level <= ZOOM_MIN_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level - ZOOM_STEP) * 100) / 100.0;
    update_zoom();

    if (m_is_private == IsPrivate::No)
        Application::settings().set_zoom_for_host(current_host_for_settings(), m_zoom_level);
}

void ViewImplementation::set_zoom(double zoom_level)
{
    m_zoom_level = max(ZOOM_MIN_LEVEL, min(zoom_level, ZOOM_MAX_LEVEL));
    update_zoom();
}

void ViewImplementation::reset_zoom()
{
    m_zoom_level = 1.0;
    update_zoom();
    client().async_reset_zoom(page_id());

    if (m_is_private == IsPrivate::No)
        Application::settings().set_zoom_for_host(current_host_for_settings(), m_zoom_level);
}

// NB: The event is handled once it leaves the queue of pending input events, or when it never enters it.
struct WebDriverInputData final : public Web::BrowserInputData {
    explicit WebDriverInputData(Function<void()> on_handled)
        : on_handled(move(on_handled))
    {
    }

    virtual ~WebDriverInputData() override { on_handled(); }

    Function<void()> on_handled;
};

void ViewImplementation::enqueue_webdriver_mouse_event(Badge<WebContentPage>, Web::MouseEvent event, Function<void()> on_handled)
{
    event.browser_data = make<WebDriverInputData>(move(on_handled));
    enqueue_input_event(move(event));
}

void ViewImplementation::enqueue_input_event(Web::InputEvent event)
{
    if (!m_client_state.page)
        return;

    auto* key_event = event.get_pointer<Web::KeyEvent>();
    auto* mouse_event = event.get_pointer<Web::MouseEvent>();
    auto* pinch_event = event.get_pointer<Web::PinchEvent>();
    if (m_debugger_paused) {
        if (mouse_event) {
            if (mouse_event->type == Web::MouseEvent::Type::MouseMove) {
                auto position = mouse_event->position.to_type<int>();
                set_debugger_overlay_hovered_action(paused_debugger_overlay_action_at(position, viewport_size().to_type<int>(), device_pixel_ratio()));
                if (m_debugger_overlay_pointer_state.is_active() && (mouse_event->buttons & Web::UIEvents::MouseButton::Primary) == Web::UIEvents::MouseButton::None)
                    m_debugger_overlay_pointer_state.cancel();
            } else if (mouse_event->type == Web::MouseEvent::Type::MouseLeave) {
                set_debugger_overlay_hovered_action({});
                m_debugger_overlay_pointer_state.cancel();
            }

            if (mouse_event->type == Web::MouseEvent::Type::MouseDown
                && mouse_event->button == Web::UIEvents::MouseButton::Primary) {
                auto position = mouse_event->position.to_type<int>();
                m_debugger_overlay_pointer_state.press(paused_debugger_overlay_action_at(position, viewport_size().to_type<int>(), device_pixel_ratio()));
            }

            if (mouse_event->type == Web::MouseEvent::Type::MouseUp
                && mouse_event->button == Web::UIEvents::MouseButton::Primary) {
                auto position = mouse_event->position.to_type<int>();
                auto released_action = paused_debugger_overlay_action_at(position, viewport_size().to_type<int>(), device_pixel_ratio());
                if (auto action = m_debugger_overlay_pointer_state.release(released_action); action.has_value()) {
                    resume_debugger(*action == PausedDebuggerOverlayAction::StepOver
                            ? DebuggerResumeMode::StepOver
                            : DebuggerResumeMode::Continue);
                }
            }
        }
        return;
    }

    // User input enables a single request for an external URL.
    if ((key_event && key_event->type == Web::KeyEvent::Type::KeyDown && !key_event->repeat
            && first_is_one_of(key_event->key, Web::UIEvents::KeyCode::Key_Return, Web::UIEvents::KeyCode::Key_Space))
        || (mouse_event && mouse_event->type == Web::MouseEvent::Type::MouseDown
            && first_is_one_of(mouse_event->button, Web::UIEvents::MouseButton::Primary, Web::UIEvents::MouseButton::Middle))) {
        m_external_url_request_policy.allow_next_request();
    }

    if (mouse_event && mouse_event->type == Web::MouseEvent::Type::MouseWheel) {
        mouse_event->wheel_delta_x /= zoom_level();
        mouse_event->wheel_delta_y /= zoom_level();
    }

    if (key_event && Web::is_keyboard_scroll_key(key_event->key, Web::UIEvents::Mod_None)) {
        bool preceding_input_may_change_target = false;
        for (auto const& pending : m_pending_input_events) {
            auto const* key = pending.event.get_pointer<Web::KeyEvent>();
            if (!key || key->type != Web::KeyEvent::Type::KeyDown || !key->async_scroll_performed_default_action)
                preceding_input_may_change_target = true;
        }
        // Always deliver key release to the compositor, even if it could no longer accept a new scroll. A focused
        // navigable another page hosts scrolls there.
        if (&focused_navigable_host() == &page()
            && (key_event->type == Web::KeyEvent::Type::KeyUp
                || (Application::web_content_options().enable_async_scrolling == EnableAsyncScrolling::Yes
                    && m_client_state.has_usable_bitmap && !preceding_input_may_change_target))) {
            auto handled = client().handle_key_event_in_compositor(page_id(), *key_event);
            key_event->async_scroll_performed_default_action = handled && key_event->type == Web::KeyEvent::Type::KeyDown;
        }
    }

    if (Application::web_content_options().enable_async_scrolling == EnableAsyncScrolling::Yes
        && m_client_state.has_usable_bitmap
        && mouse_event) {
        if (mouse_event->type == Web::MouseEvent::Type::MouseWheel) {
            auto wheel_delta_x = mouse_event->wheel_delta_x;
            auto wheel_delta_y = mouse_event->wheel_delta_y;
            if (mouse_event->modifiers & Web::UIEvents::KeyModifier::Mod_Shift)
                swap(wheel_delta_x, wheel_delta_y);

            auto device_pixels_per_css_pixel = static_cast<float>(device_pixel_ratio() * zoom_level());
            auto position = Gfx::FloatPoint {
                static_cast<float>(mouse_event->position.x().value()),
                static_cast<float>(mouse_event->position.y().value()),
            };
            auto delta_in_device_pixels = Gfx::FloatPoint { wheel_delta_x, wheel_delta_y }.scaled(device_pixels_per_css_pixel);
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI attempting compositor wheel bypass for page {} at {},{} device delta {},{}",
                page_id(), position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            if (client().send_async_scroll_to_compositor(page_id(), position, delta_in_device_pixels, mouse_event->wheel_delta_precision, mouse_event->scroll_gesture_phase))
                mouse_event->async_scroll_performed_default_action = true;
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor wheel bypass result for page {}: {}",
                page_id(), mouse_event->async_scroll_performed_default_action ? "accepted"sv : "rejected"sv);
        } else if (client().handle_mouse_event_in_compositor(page_id(), *mouse_event)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor handled mouse event for page {} at {},{}",
                page_id(), mouse_event->position.x().value(), mouse_event->position.y().value());
            return;
        }
    }
    if (Application::web_content_options().enable_async_scrolling == EnableAsyncScrolling::Yes
        && m_client_state.has_usable_bitmap
        && pinch_event) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI attempting compositor pinch bypass for page {} at {},{} scale delta {}",
            page_id(), pinch_event->position.x().value(), pinch_event->position.y().value(), pinch_event->scale_delta);
        auto handled = client().handle_pinch_event_in_compositor(page_id(), *pinch_event);
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor pinch bypass result for page {}: {}",
            page_id(), handled ? "accepted"sv : "rejected"sv);
    }

    // Send the next event over to the WebContent to be handled by JS. We'll later get a message to say whether JS
    // prevented the default event behavior, at which point we either discard or handle that event, and then try to
    // process the next one.
    Web::set_input_event_id(event, m_next_input_event_id++);
    m_pending_input_events.append({ move(event), page() });

    auto& pending = m_pending_input_events.last();
    pending.event.visit(
        [&](Web::KeyEvent const& event) {
            auto& host = focused_navigable_host();
            if (&host == &page()) {
                client().dispatch_key_event_to_web_content(page_id(), event);
            } else {
                pending.endpoint = host;
                host.async_key_event(event.clone_without_browser_data());
            }
        },
        [this](Web::MouseEvent const& event) {
            client().dispatch_mouse_event_to_web_content(page_id(), event);
        },
        [this](Web::DragEvent& event) {
            auto cloned_event = event.clone_without_browser_data();
            cloned_event.files = move(event.files);

            client().async_drag_event(page_id(), cloned_event);
        },
        [this](Web::PinchEvent const& event) {
            client().async_pinch_event(page_id(), event);
        });
}

void ViewImplementation::handle_external_url(Badge<WebContentPage>, URL::URL url, URL::Origin initiator_origin, bool has_transient_activation)
{
    handle_external_url(move(url), move(initiator_origin), has_transient_activation);
}

void ViewImplementation::handle_external_url_from_user_input(URL::URL const& url, Function<void()> on_handler_unavailable)
{
    // Browser UI actions can submit several URLs at once, for example when opening a bookmark folder. Keep those
    // requests ordered while a handler lookup or confirmation is in progress.
    m_pending_external_url_requests.enqueue({ url, m_url.origin(), move(on_handler_unavailable) });
    process_next_external_url_request();
}

void ViewImplementation::process_next_external_url_request()
{
    while (!m_external_url_confirmation_pending && !m_pending_external_url_requests.is_empty()) {
        auto request = m_pending_external_url_requests.dequeue();
        m_external_url_request_policy.allow_request_from_browser_ui(request.url);
        handle_external_url(move(request.url), move(request.initiator_origin), false, move(request.on_handler_unavailable));
    }
}

void ViewImplementation::complete_external_url_request()
{
    VERIFY(m_external_url_confirmation_pending);
    m_external_url_confirmation_pending = false;

    if (m_pending_external_url_requests.is_empty())
        return;

    auto view_id = m_view_id;
    Core::deferred_invoke([view_id] {
        if (auto view = ViewImplementation::find_view_by_id(view_id); view.has_value())
            view->process_next_external_url_request();
    });
}

void ViewImplementation::handle_external_url(URL::URL url, URL::Origin initiator_origin, bool has_transient_activation, Function<void()> on_handler_unavailable)
{
    if (m_external_url_confirmation_pending)
        return;

    auto action = m_external_url_request_policy.decide(url, has_transient_activation);
    if (action == ExternalURLAction::Block)
        return;

    auto const headless_mode = Application::browser_options().headless_mode.has_value();
    if (!headless_mode && action == ExternalURLAction::Prompt && !on_request_external_url_confirmation)
        return;

    // Reserve the allowance while the handler is resolved. Browser UI allowances are specific to one navigation and
    // are never restored.
    auto request_source = m_external_url_request_policy.take_request_allowance();
    if (!request_source.has_value())
        return;

    if (headless_mode) {
        if (on_handler_unavailable) {
            on_handler_unavailable();
            return;
        }

        Application::the().display_error_dialog(MUST(String::formatted("External URL handling is unavailable in headless mode: {}", url)));
        return;
    }

    m_external_url_confirmation_pending = true;
    auto view_id = m_view_id;
    auto url_for_callback = url;
    Application::the().resolve_external_url_handler(url, [view_id, url = move(url_for_callback), initiator_origin = move(initiator_origin), action, request_source = *request_source, on_handler_unavailable = move(on_handler_unavailable)](RefPtr<ExternalURLHandler> handler) mutable {
        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value())
            return;

        if (!handler) {
            // This is step 4 of this algorithm:
            // https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-create-a-non-fetch-scheme-document
            // As noted in attempt_to_create_a_non_fetch_scheme_document() in LocalNavigable.cpp, it can't happen there
            // because resolving the external url handler is asynchronous.
            //
            // 4. Handle url by displaying some sort of inline content, e.g., an error message because the specified
            //    scheme is not one of the supported protocols, or an inline prompt to allow the user to select a
            //    registered handler for the given scheme. Return the result of displaying the inline content given
            //    navigable, navigationParams's id, navigationParams's navigation timing type, and navigationParams's
            //    user involvement.
            // AD-HOC: We can't implement this directly, so show a warning dialog instead.
            if (on_handler_unavailable) {
                view->complete_external_url_request();
                on_handler_unavailable();
                return;
            }

            auto error_message = MUST(String::formatted("No application is registered to open {}", url));
            Application::the().display_error_dialog(error_message);
            if (auto current_view = ViewImplementation::find_view_by_id(view_id); current_view.has_value())
                current_view->complete_external_url_request();
            return;
        }

        if (handler->is_ladybird()) {
            view->complete_external_url_request();
            if (on_handler_unavailable) {
                on_handler_unavailable();
                return;
            }

            if (request_source == ExternalURLRequestSource::Page)
                view->m_external_url_request_policy.allow_next_request();
            return;
        }

        if (action == ExternalURLAction::Launch) {
            handler->launch(url, [view_id](bool success) {
                if (!success && ViewImplementation::find_view_by_id(view_id).has_value())
                    Application::the().display_error_dialog("Unable to open external URL"sv);
            });
            if (auto current_view = ViewImplementation::find_view_by_id(view_id); current_view.has_value())
                current_view->complete_external_url_request();
            return;
        }

        if (!view->on_request_external_url_confirmation) {
            if (request_source == ExternalURLRequestSource::Page)
                view->m_external_url_request_policy.allow_next_request();
            view->complete_external_url_request();
            return;
        }

        auto on_complete = [view_id, url, handler](bool accepted) {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;

            if (accepted) {
                handler->launch(url, [view_id](bool success) {
                    if (!success && ViewImplementation::find_view_by_id(view_id).has_value())
                        Application::the().display_error_dialog("Unable to open external URL"sv);
                });
            }
            if (auto current_view = ViewImplementation::find_view_by_id(view_id); current_view.has_value())
                current_view->complete_external_url_request();
        };
        view->on_request_external_url_confirmation(url, initiator_origin, *handler, move(on_complete));
    });
}

Web::UIEvents::KeyModifier ViewImplementation::history_traversal_key_modifier()
{
#if defined(AK_OS_MACOS)
    return Web::UIEvents::KeyModifier::Mod_Super;
#else
    return Web::UIEvents::KeyModifier::Mod_Alt;
#endif
}

static bool is_history_traversal_key_event(Web::KeyEvent const& event)
{
    if (event.type != Web::KeyEvent::Type::KeyDown)
        return false;
    if (event.key != Web::UIEvents::KeyCode::Key_Left && event.key != Web::UIEvents::KeyCode::Key_Right)
        return false;
    auto modifier = ViewImplementation::history_traversal_key_modifier();
    return event.modifiers == modifier || event.modifiers == (modifier | Web::UIEvents::Mod_Keypad);
}

void ViewImplementation::did_finish_handling_input_event(Badge<WebContentPage>, u64 event_id, Web::EventResult event_result)
{
    // Adjacent events can be handled by different processes, which finish them in no particular order.
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    if (!index.has_value())
        return;
    auto event = m_pending_input_events.take(*index).event;

    if (event_result == Web::EventResult::Handled || event_result == Web::EventResult::Cancelled)
        return;

    if (auto const* key_event = event.get_pointer<Web::KeyEvent>(); key_event && is_history_traversal_key_event(*key_event)) {
        traverse_the_history_by_delta(key_event->key == Web::UIEvents::KeyCode::Key_Left ? -1 : 1);
        return;
    }

    // Here we handle events that were not consumed by the WebContent. Propagate the event back
    // to the concrete view implementation.
    event.visit(
        [this](Web::KeyEvent const& event) {
            if (on_finish_handling_key_event)
                on_finish_handling_key_event(event);
        },
        [this](Web::DragEvent const& event) {
            if (on_finish_handling_drag_event)
                on_finish_handling_drag_event(event);
        },
        [](auto const&) {});
}

void ViewImplementation::did_lose_input_event_endpoint(Badge<WebContentClient>, WebContentPage& page)
{
    // Nothing will finish the events the lost page held, and a pending event holds back compositor input.
    m_pending_input_events.remove_all_matching([&](auto const& pending) { return pending.endpoint == page; });
}

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    set_page_background_color(preferred_canvas_background_color());

    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_color_scheme(color_scheme);
    });
}

void ViewImplementation::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_contrast(contrast);
    });
}

void ViewImplementation::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_motion(motion);
    });
}

static void send_browsing_behavior(WebContentPage const& page)
{
    page.async_set_browsing_behavior(Application::settings().browsing_behavior());
}

static void send_autoplay_settings(WebContentPage const& page)
{
    auto const& autoplay_settings = Application::settings().autoplay_settings();
    auto const& web_content_options = Application::web_content_options();

    auto policy = autoplay_settings.policy;
    if (web_content_options.enable_autoplay == EnableAutoplay::Yes)
        policy = Web::HTML::AutoplayPolicy::AllowAudioAndVideo;

    Vector<Utf16String> allowlist;
    allowlist.ensure_capacity(autoplay_settings.site_filters.size());
    for (auto const& site_filter : autoplay_settings.site_filters)
        allowlist.unchecked_append(Utf16String::from_utf8(site_filter));

    page.async_set_autoplay_settings(policy, move(allowlist));
}

static void send_global_privacy_control(WebContentPage const& page)
{
    page.async_set_enable_global_privacy_control(Application::settings().global_privacy_control() == GlobalPrivacyControl::Yes);
}

void ViewImplementation::send_preferences_to_page(Badge<WebContentClient>, WebContentPage& page)
{
    page.async_set_preferred_color_scheme(m_preferred_color_scheme);
    page.async_set_preferred_contrast(m_preferred_contrast);
    page.async_set_preferred_motion(m_preferred_motion);
    page.async_set_preferred_languages(Application::settings().languages());
    if (m_user_style_sheet.has_value())
        page.async_set_user_style(*m_user_style_sheet);
    send_browsing_behavior(page);
    send_autoplay_settings(page);
    send_global_privacy_control(page);
    send_geolocation_emulated_position(page);
}

void ViewImplementation::notify_cookies_changed(HashTable<String> const& changed_domains, ReadonlySpan<HTTP::Cookie::Cookie> page_cookies, ReadonlySpan<HTTP::Cookie::Cookie> host_cookies)
{
    for (auto const& domain : changed_domains) {
        if (auto document_index = m_document_cookie_version_indices.get(domain); document_index.has_value())
            Core::increment_shared_version(m_document_cookie_version_buffer, *document_index);
    }

    if (!page_cookies.is_empty())
        client().async_cookies_changed(page_id(), page_cookies);
    if (m_on_host_cookie_change)
        m_on_host_cookie_change(Vector<HTTP::Cookie::Cookie> { host_cookies });
}

void ViewImplementation::listen_for_host_cookie_changes(DevTools::DevToolsDelegate::OnHostCookieChange on_cookie_change)
{
    m_on_host_cookie_change = move(on_cookie_change);
}

void ViewImplementation::stop_listening_for_host_cookie_changes()
{
    m_on_host_cookie_change = nullptr;
}

void ViewImplementation::notify_storage_changed(DevTools::DevToolsDelegate::StorageChange change)
{
    for (auto& listener : m_storage_change_listeners)
        listener.value(change);
}

u64 ViewImplementation::add_storage_change_listener(DevTools::DevToolsDelegate::OnStorageChange on_storage_change)
{
    auto listener_id = m_next_storage_change_listener_id++;
    m_storage_change_listeners.set(listener_id, move(on_storage_change));
    return listener_id;
}

void ViewImplementation::remove_storage_change_listener(u64 listener_id)
{
    m_storage_change_listeners.remove(listener_id);
}

void ViewImplementation::notify_indexed_database_changed(JsonObject update)
{
    for (auto& listener : m_indexed_database_change_listeners)
        listener.value(update);
}

u64 ViewImplementation::add_indexed_database_change_listener(DevTools::DevToolsDelegate::OnIndexedDatabaseChange on_indexed_database_change)
{
    auto listener_id = m_next_indexed_database_change_listener_id++;
    m_indexed_database_change_listeners.set(listener_id, move(on_indexed_database_change));
    return listener_id;
}

void ViewImplementation::remove_indexed_database_change_listener(u64 listener_id)
{
    m_indexed_database_change_listeners.remove(listener_id);
}

ErrorOr<Core::SharedVersionIndex> ViewImplementation::ensure_document_cookie_version_index(Badge<WebContentPage>, String const& domain)
{
    return m_document_cookie_version_indices.try_ensure(domain, [&]() -> ErrorOr<Core::SharedVersionIndex> {
        Core::SharedVersionIndex document_index = m_document_cookie_version_indices.size();

        if (!Core::initialize_shared_version(m_document_cookie_version_buffer, document_index)) {
            dbgln("Reached maximum document cookie version count for {}, cannot create new version for {}", m_url, domain);
            return Error::from_string_literal("Reached maximum document cookie version count");
        }

        return document_index;
    });
}

Optional<Core::SharedVersion> ViewImplementation::document_cookie_version(URL::URL const& url) const
{
    auto domain = HTTP::Cookie::canonicalize_domain(url);
    if (!domain.has_value())
        return {};

    auto document_index = m_document_cookie_version_indices.get(*domain);
    if (!document_index.has_value())
        return {};

    return Core::get_shared_version(m_document_cookie_version_buffer, *document_index);
}

NonnullRefPtr<Core::Promise<ByteString>> ViewImplementation::selected_text()
{
    auto promise = Core::Promise<ByteString>::construct();
    auto request_id = m_next_selection_request_id++;
    m_pending_selected_text_requests.set(request_id, promise);
    auto& host = focused_navigable_host();
    host.async_get_selected_text(request_id);
    return promise;
}

void ViewImplementation::did_receive_selected_text(Badge<WebContentPage>, u64 request_id, ByteString selection)
{
    auto promise = m_pending_selected_text_requests.take(request_id);
    if (!promise.has_value())
        return;
    promise.value()->resolve(move(selection));
}

NonnullRefPtr<Core::Promise<ByteString>> ViewImplementation::cut_selected_text()
{
    auto promise = Core::Promise<ByteString>::construct();
    auto request_id = m_next_selection_request_id++;
    m_pending_cut_selected_text_requests.set(request_id, promise);
    auto& host = focused_navigable_host();
    host.async_cut_selected_text(request_id);
    return promise;
}

void ViewImplementation::did_cut_selected_text(Badge<WebContentPage>, u64 request_id, ByteString selection)
{
    auto promise = m_pending_cut_selected_text_requests.take(request_id);
    if (!promise.has_value())
        return;
    promise.value()->resolve(move(selection));
}

NonnullRefPtr<Core::Promise<Optional<String>>> ViewImplementation::selected_text_with_whitespace_collapsed()
{
    return selected_text()->map<Optional<String>>([](auto& selection) -> Optional<String> {
        auto collapsed_selection = MUST(Web::Infra::strip_and_collapse_whitespace(selection));
        if (collapsed_selection.is_empty())
            return {};
        return collapsed_selection;
    });
}

NonnullRefPtr<Core::Promise<Optional<DictionaryLookup>>> ViewImplementation::selected_text_for_dictionary_lookup()
{
    auto promise = Core::Promise<Optional<DictionaryLookup>>::construct();
    auto request_id = m_next_selection_request_id++;
    m_pending_selected_text_for_lookup_requests.set(request_id, promise);
    auto& host = focused_navigable_host();
    host.async_get_selected_text_for_lookup(request_id);

    return promise->map<Optional<DictionaryLookup>>([](auto& lookup) -> Optional<DictionaryLookup> {
        if (!lookup.has_value())
            return {};

        auto collapsed_selection = MUST(Web::Infra::strip_and_collapse_whitespace(lookup->text));
        if (collapsed_selection.is_empty())
            return {};

        auto result = lookup;
        result->text = move(collapsed_selection);
        return result;
    });
}

void ViewImplementation::did_receive_selected_text_for_lookup(Badge<WebContentPage>, u64 request_id, Optional<DictionaryLookup> lookup)
{
    auto promise = m_pending_selected_text_for_lookup_requests.take(request_id);
    if (!promise.has_value())
        return;
    promise.value()->resolve(move(lookup));
}

NonnullRefPtr<Core::Promise<bool>> ViewImplementation::select_word_for_dictionary_lookup(Gfx::IntPoint widget_position)
{
    auto promise = Core::Promise<bool>::construct();
    auto request_id = m_next_selection_request_id++;
    m_pending_select_word_for_dictionary_lookup_requests.set(request_id, promise);

    // The word is selected in the page the lookup then asks for the selection, in the viewport of its local root.
    auto& host = focused_navigable_host();
    auto position = to_content_position(widget_position).to_type<Web::DevicePixels>() - m_top_level_traversable.focused_navigable_host_offset();
    host.async_select_word_for_dictionary_lookup(request_id, position);
    return promise;
}

void ViewImplementation::did_select_word_for_dictionary_lookup(Badge<WebContentPage>, u64 request_id, bool selected)
{
    auto promise = m_pending_select_word_for_dictionary_lookup_requests.take(request_id);
    if (!promise.has_value())
        return;
    promise.value()->resolve(selected);
}

bool ViewImplementation::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    if (!on_request_dictionary_lookup)
        return false;

    auto weak_this = make_weak_ptr();
    selected_text_for_dictionary_lookup()->when_resolved([weak_this, widget_position](auto& lookup) {
        if (!weak_this)
            return;

        if (lookup.has_value()) {
            auto lookup_position = lookup->baseline_origin.has_value() ? weak_this->to_widget_position(*lookup->baseline_origin) : widget_position;
            weak_this->on_request_dictionary_lookup(*lookup, lookup_position);
            return;
        }

        weak_this->select_word_for_dictionary_lookup(widget_position)->when_resolved([weak_this, widget_position](bool& selected) {
            if (!weak_this || !selected)
                return;

            weak_this->selected_text_for_dictionary_lookup()->when_resolved([weak_this, widget_position](auto& lookup) {
                if (!weak_this || !lookup.has_value())
                    return;

                auto lookup_position = lookup->baseline_origin.has_value() ? weak_this->to_widget_position(*lookup->baseline_origin) : widget_position;
                weak_this->on_request_dictionary_lookup(*lookup, lookup_position);
            });
        });
    });
    return true;
}

void ViewImplementation::select_all()
{
    auto& host = focused_navigable_host();
    host.async_select_all();
}

void ViewImplementation::undo()
{
    auto& host = focused_navigable_host();
    host.async_undo();
}

void ViewImplementation::set_editing_history_state(bool can_undo, bool can_redo)
{
    m_can_undo = can_undo;
    m_can_redo = can_redo;
    Application::the().update_editing_history_actions();
}

void ViewImplementation::redo()
{
    auto& host = focused_navigable_host();
    host.async_redo();
}

void ViewImplementation::find_in_page(Utf16String const& query, CaseSensitivity case_sensitivity)
{
    client().async_find_in_page(page_id(), query, case_sensitivity);
}

void ViewImplementation::find_in_page_next_match()
{
    client().async_find_in_page_next_match(page_id());
}

void ViewImplementation::find_in_page_previous_match()
{
    client().async_find_in_page_previous_match(page_id());
}

void ViewImplementation::get_source()
{
    client().async_get_source(page_id());
}

void ViewImplementation::inspect_dom_tree()
{
    client().async_inspect_dom_tree(page_id());
}

void ViewImplementation::inspect_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, u64 request_id)
{
    client().async_inspect_storage(page_id(), storage_endpoint, request_id);
}

Optional<StorageSetResult> ViewImplementation::set_session_storage_item(Utf16String const& key, Utf16String const& value)
{
    return client().set_session_storage_item(page_id(), key, value);
}

Optional<Utf16String> ViewImplementation::remove_session_storage_item(Utf16String const& key)
{
    return client().remove_session_storage_item(page_id(), key);
}

bool ViewImplementation::clear_session_storage()
{
    return client().clear_session_storage(page_id());
}

void ViewImplementation::inspect_accessibility_tree()
{
    client().async_inspect_accessibility_tree(page_id());
}

void ViewImplementation::get_hovered_node_id()
{
    client().async_get_hovered_node_id(page_id());
}

void ViewImplementation::start_node_picker(DevTools::DevToolsDelegate::OnNodePickerEvent on_node_picker_event)
{
    m_node_picker_active = true;
    m_node_picker_hovered_node_id.clear();
    m_pending_node_picker_requests.clear();
    m_on_node_picker_event = move(on_node_picker_event);
}

void ViewImplementation::stop_node_picker()
{
    if (!m_node_picker_active)
        return;

    clear_node_picker();
    m_node_picker_active = false;
    m_pending_node_picker_requests.clear();
    m_on_node_picker_event = nullptr;
}

void ViewImplementation::clear_node_picker()
{
    m_node_picker_hovered_node_id.clear();
    clear_highlighted_dom_node();
}

void ViewImplementation::node_picker_hover(Web::DevicePixelPoint position)
{
    request_node_picker_hit_test(NodePickerRequestType::Hovered, position);
}

void ViewImplementation::node_picker_pick(Web::DevicePixelPoint position)
{
    request_node_picker_hit_test(NodePickerRequestType::Picked, position);
}

void ViewImplementation::node_picker_preview(Web::DevicePixelPoint position)
{
    request_node_picker_hit_test(NodePickerRequestType::Previewed, position);
}

void ViewImplementation::node_picker_cancel()
{
    if (!m_node_picker_active)
        return;

    if (m_on_node_picker_event) {
        m_on_node_picker_event({
            .type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Canceled,
            .node_id = {},
        });
    }
}

void ViewImplementation::request_node_picker_hit_test(NodePickerRequestType type, Web::DevicePixelPoint position)
{
    if (!m_node_picker_active)
        return;

    auto request_id = m_next_node_picker_request_id++;
    m_pending_node_picker_requests.set(request_id, type);
    client().async_get_node_id_at_position(page_id(), request_id, position);
}

void ViewImplementation::did_receive_node_picker_hit_test(u64 request_id, Web::UniqueNodeID node_id)
{
    auto request_type = m_pending_node_picker_requests.take(request_id);
    if (!request_type.has_value() || !m_node_picker_active)
        return;

    if (*request_type == NodePickerRequestType::Hovered) {
        if (node_id == m_node_picker_hovered_node_id.value_or(0))
            return;

        m_node_picker_hovered_node_id = node_id;
        if (node_id == 0) {
            clear_node_picker();
            return;
        }
    } else if (node_id == 0) {
        return;
    }

    if (!m_on_node_picker_event)
        return;

    DevTools::DevToolsDelegate::NodePickerEvent::Type event_type;
    switch (*request_type) {
    case NodePickerRequestType::Hovered:
        event_type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Hovered;
        break;
    case NodePickerRequestType::Picked:
        event_type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Picked;
        break;
    case NodePickerRequestType::Previewed:
        event_type = DevTools::DevToolsDelegate::NodePickerEvent::Type::Previewed;
        break;
    }

    m_on_node_picker_event({
        .type = event_type,
        .node_id = node_id,
    });
}

void ViewImplementation::inspect_dom_node(Web::UniqueNodeID node_id, DOMNodeProperties::Type property_type, Optional<Web::CSS::PseudoElement> pseudo_element, JsonValue options)
{
    client().async_inspect_dom_node(page_id(), property_type, node_id, pseudo_element, move(options));
}

void ViewImplementation::inspect_grid_layouts(Web::UniqueNodeID root_node_id)
{
    client().async_inspect_grid_layouts(page_id(), root_node_id);
}

void ViewImplementation::inspect_current_grid(Web::UniqueNodeID node_id)
{
    client().async_inspect_current_grid(page_id(), node_id);
}

void ViewImplementation::inspect_current_flexbox(Web::UniqueNodeID node_id, bool only_look_at_parents)
{
    client().async_inspect_current_flexbox(page_id(), node_id, only_look_at_parents);
}

void ViewImplementation::inspect_indexed_database_storage(DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    client().async_inspect_indexed_database_storage(page_id(), request_id);
}

void ViewImplementation::inspect_indexed_database_objects(String const& host, Optional<JsonArray> names, JsonObject options, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    if (names.has_value())
        client().async_inspect_indexed_database_objects(page_id(), request_id, host, JsonValue { names.release_value() }, JsonValue { move(options) });
    else
        client().async_inspect_indexed_database_objects(page_id(), request_id, host, JsonValue {}, JsonValue { move(options) });
}

void ViewImplementation::delete_indexed_database(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    client().async_delete_indexed_database(page_id(), request_id, host, name);
}

void ViewImplementation::clear_indexed_database_object_store(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    client().async_clear_indexed_database_object_store(page_id(), request_id, host, name);
}

void ViewImplementation::delete_indexed_database_record(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    client().async_delete_indexed_database_record(page_id(), request_id, host, name);
}

void ViewImplementation::did_receive_indexed_database_inspection(u64 request_id, JsonObject result)
{
    auto callback = m_pending_indexed_database_inspection_requests.take(request_id);
    if (!callback.has_value())
        return;

    if (result.has_string("error"sv)) {
        (*callback)(Error::from_string_literal("IndexedDB operation failed"));
        return;
    }

    (*callback)(move(result));
}

void ViewImplementation::retrieve_devtools_sources(DevTools::DevToolsDelegate::OnSourcesReceived on_complete)
{
    auto request_id = m_next_devtools_sources_request_id++;
    on_received_devtools_sources.set(request_id, move(on_complete));
    client().async_list_devtools_sources(page_id(), request_id);
}

void ViewImplementation::request_devtools_source(Web::HTML::ScriptRegistry::Identifier const& source_id)
{
    client().async_request_devtools_source(page_id(), source_id);
}

void ViewImplementation::attach_debugger(DevTools::DevToolsDelegate::OnDebuggerPaused on_paused, DevTools::DevToolsDelegate::OnDebuggerResumed on_resumed)
{
    on_debugger_paused = move(on_paused);
    on_debugger_resumed = move(on_resumed);
    m_debugger_is_attached = true;
    client().async_attach_debugger(page_id());
}

void ViewImplementation::configure_debugger(DebuggerConfiguration configuration)
{
    client().async_configure_debugger(page_id(), configuration);
}

void ViewImplementation::detach_debugger()
{
    fail_pending_debugger_requests();
    m_debugger_is_attached = false;
    on_debugger_paused = nullptr;
    on_debugger_resumed = nullptr;
    client().async_detach_debugger(page_id());
}

void ViewImplementation::interrupt_debugger()
{
    client().async_interrupt_debugger(page_id());
}

void ViewImplementation::resume_debugger(DebuggerResumeMode mode)
{
    client().async_resume_debugger(page_id(), mode);
}

template<typename Callback, typename ErrorFactory>
static void fail_pending_debugger_request_map(HashMap<u64, Callback>& requests, HashTable<u64>* cancelled_requests, ErrorFactory make_error)
{
    auto requests_to_fail = move(requests);
    requests = {};

    for (auto& request : requests_to_fail) {
        if (cancelled_requests)
            cancelled_requests->set(request.key);
        request.value(make_error());
    }
}

void ViewImplementation::fail_pending_debugger_requests()
{
    auto make_error = [] { return Error::from_string_literal("WebContent process was replaced"); };
    fail_pending_debugger_request_map(m_pending_debugger_breakpoint_requests, nullptr, make_error);
    fail_pending_debugger_request_map(m_pending_debugger_environments_requests, &m_cancelled_debugger_environments_requests, make_error);
    fail_pending_debugger_request_map(m_pending_debugger_evaluation_requests, &m_cancelled_debugger_evaluation_requests, [] { return "WebContent process was replaced"_string; });
    fail_pending_debugger_request_map(m_pending_debugger_object_properties_requests, &m_cancelled_debugger_object_properties_requests, [] { return "WebContent process was replaced"_string; });
    fail_pending_debugger_request_map(m_pending_debugger_source_positions_requests, &m_cancelled_debugger_source_positions_requests, make_error);
}

void ViewImplementation::did_pause_debugger(Badge<WebContentPage>)
{
    set_debugger_paused(true);
}

void ViewImplementation::did_resume_debugger(Badge<WebContentPage>)
{
    set_debugger_paused(false);
    if (on_debugger_resumed)
        on_debugger_resumed();
}

void ViewImplementation::did_request_cursor_change(Badge<WebContentPage>, Gfx::Cursor cursor)
{
    m_page_cursor = move(cursor);
    if (!m_debugger_overlay_hovered_action.has_value() && on_cursor_change)
        on_cursor_change(m_page_cursor);
}

void ViewImplementation::set_debugger_paused(bool paused)
{
    if (m_debugger_paused == paused)
        return;

    m_debugger_paused = paused;
    if (!paused)
        m_debugger_overlay_pointer_state.cancel();
    if (!paused && m_debugger_overlay_hovered_action.has_value()) {
        m_debugger_overlay_hovered_action.clear();
        if (on_cursor_change)
            on_cursor_change(m_page_cursor);
    }
    update_paused_debugger_overlay();
}

void ViewImplementation::set_debugger_overlay_hovered_action(Optional<PausedDebuggerOverlayAction> action)
{
    if (m_debugger_overlay_hovered_action == action)
        return;

    m_debugger_overlay_hovered_action = action;
    update_paused_debugger_overlay();

    if (on_cursor_change)
        on_cursor_change(action.has_value() ? Gfx::Cursor { Gfx::StandardCursor::Hand } : m_page_cursor);
}

void ViewImplementation::update_paused_debugger_overlay()
{
    if (!m_client_state.page)
        return;

    auto context_id = client().compositor_context_id_for_page(page_id());
    Optional<u8> hovered_action;
    if (m_debugger_overlay_hovered_action.has_value())
        hovered_action = to_underlying(*m_debugger_overlay_hovered_action);
    Application::the().update_compositor_paused_debugger_overlay(context_id, m_debugger_paused, device_pixel_ratio(), Application::the().ui_font_family(), hovered_action);
}

void ViewImplementation::update_debugger_blackboxing(Utf16String url, Vector<DebuggerBlackboxRange> ranges, DebuggerBlackboxingOperation operation)
{
    client().async_update_debugger_blackboxing(page_id(), move(url), move(ranges), operation);
}

void ViewImplementation::set_debugger_breakpoint(DebuggerBreakpointLocation location, DebuggerBreakpointOptions options, DevTools::DevToolsDelegate::OnDebuggerBreakpointOperationComplete on_complete)
{
    auto request_id = m_next_debugger_breakpoint_request_id++;
    m_pending_debugger_breakpoint_requests.set(request_id, move(on_complete));
    client().async_set_debugger_breakpoint(page_id(), request_id, move(location), move(options));
}

void ViewImplementation::remove_debugger_breakpoint(DebuggerBreakpointLocation location, DevTools::DevToolsDelegate::OnDebuggerBreakpointOperationComplete on_complete)
{
    auto request_id = m_next_debugger_breakpoint_request_id++;
    m_pending_debugger_breakpoint_requests.set(request_id, move(on_complete));
    client().async_remove_debugger_breakpoint(page_id(), request_id, move(location));
}

void ViewImplementation::did_complete_debugger_breakpoint_operation(u64 request_id, Optional<String> error)
{
    auto callback = m_pending_debugger_breakpoint_requests.take(request_id);
    if (!callback.has_value())
        return;

    if (error.has_value()) {
        (*callback)(Error::from_string_literal("Debugger breakpoint operation failed"));
        return;
    }

    (*callback)({});
}

void ViewImplementation::retrieve_debugger_environments(u64 frame_id, DevTools::DevToolsDelegate::OnDebuggerEnvironmentsReceived on_complete)
{
    auto request_id = m_next_debugger_environments_request_id++;
    m_pending_debugger_environments_requests.set(request_id, move(on_complete));
    client().async_get_debugger_environments(page_id(), request_id, frame_id);
}

void ViewImplementation::evaluate_javascript_in_debugger_frame(u64 frame_id, String const& source_text, DevTools::DevToolsDelegate::OnDebuggerEvaluationComplete on_complete)
{
    auto request_id = m_next_debugger_evaluation_request_id++;
    m_pending_debugger_evaluation_requests.set(request_id, move(on_complete));
    client().async_evaluate_javascript_in_debugger_frame(page_id(), request_id, frame_id, Utf16String::from_utf8(source_text));
}

void ViewImplementation::retrieve_debugger_object_properties(u64 object_id, DevTools::DevToolsDelegate::OnDebuggerObjectPropertiesReceived on_complete)
{
    auto request_id = m_next_debugger_object_properties_request_id++;
    m_pending_debugger_object_properties_requests.set(request_id, move(on_complete));
    client().async_get_debugger_object_properties(page_id(), request_id, object_id);
}

void ViewImplementation::retrieve_debugger_source_positions(Web::HTML::ScriptRegistry::Identifier source_id, DevTools::DevToolsDelegate::OnDebuggerSourcePositionsReceived on_complete)
{
    auto request_id = m_next_debugger_source_positions_request_id++;
    m_pending_debugger_source_positions_requests.set(request_id, move(on_complete));
    client().async_get_debugger_source_positions(page_id(), request_id, source_id);
}

void ViewImplementation::resolve_dom_node_url(Optional<Web::UniqueNodeID> node_id, String const& url, DevTools::DevToolsDelegate::OnResolvedURLReceived on_complete)
{
    auto request_id = m_next_resolve_dom_node_url_request_id++;
    on_resolved_dom_node_url.set(request_id, move(on_complete));
    client().async_resolve_dom_node_url(page_id(), request_id, node_id, url);
}

void ViewImplementation::clear_inspected_dom_node()
{
    client().async_clear_inspected_dom_node(page_id());
}

void ViewImplementation::highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    client().async_highlight_dom_node(page_id(), node_id, pseudo_element);
}

void ViewImplementation::clear_highlighted_dom_node()
{
    highlight_dom_node(0, {});
}

void ViewImplementation::highlight_flexbox(Web::UniqueNodeID node_id, JsonValue options)
{
    client().async_highlight_flexbox(page_id(), node_id, move(options));
}

void ViewImplementation::clear_flexbox_highlight(Web::UniqueNodeID node_id)
{
    client().async_clear_flexbox_highlight(page_id(), node_id);
}

void ViewImplementation::highlight_grid(Web::UniqueNodeID node_id, JsonValue options)
{
    client().async_highlight_grid(page_id(), node_id, move(options));
}

void ViewImplementation::clear_grid_highlight(Web::UniqueNodeID node_id)
{
    client().async_clear_grid_highlight(page_id(), node_id);
}

void ViewImplementation::set_listen_for_dom_mutations(bool listen_for_dom_mutations)
{
    client().async_set_listen_for_dom_mutations(page_id(), listen_for_dom_mutations);
}

void ViewImplementation::did_connect_devtools_client()
{
    m_devtools_connected = true;
    client().async_did_connect_devtools_client(page_id());
}

void ViewImplementation::did_disconnect_devtools_client()
{
    m_devtools_connected = false;
    client().async_did_disconnect_devtools_client(page_id());
}

void ViewImplementation::get_dom_node_inner_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_inner_html(page_id(), node_id);
}

void ViewImplementation::get_dom_node_outer_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_outer_html(page_id(), node_id);
}

void ViewImplementation::set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html)
{
    client().async_set_dom_node_outer_html(page_id(), node_id, html);
}

void ViewImplementation::set_dom_node_text(Web::UniqueNodeID node_id, String const& text)
{
    client().async_set_dom_node_text(page_id(), node_id, text);
}

void ViewImplementation::set_dom_node_tag(Web::UniqueNodeID node_id, Utf16FlyString const& name)
{
    client().async_set_dom_node_tag(page_id(), node_id, name);
}

void ViewImplementation::add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes)
{
    client().async_add_dom_node_attributes(page_id(), node_id, attributes);
}

void ViewImplementation::replace_dom_node_attribute(Web::UniqueNodeID node_id, Utf16FlyString const& name, ReadonlySpan<Attribute> replacement_attributes)
{
    client().async_replace_dom_node_attribute(page_id(), node_id, name, replacement_attributes);
}

void ViewImplementation::create_child_element(Web::UniqueNodeID node_id)
{
    client().async_create_child_element(page_id(), node_id);
}

void ViewImplementation::create_child_text_node(Web::UniqueNodeID node_id)
{
    client().async_create_child_text_node(page_id(), node_id);
}

void ViewImplementation::insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id)
{
    client().async_insert_dom_node_before(page_id(), node_id, parent_node_id, sibling_node_id);
}

void ViewImplementation::clone_dom_node(Web::UniqueNodeID node_id)
{
    client().async_clone_dom_node(page_id(), node_id);
}

void ViewImplementation::remove_dom_node(Web::UniqueNodeID node_id)
{
    client().async_remove_dom_node(page_id(), node_id);
}

void ViewImplementation::list_style_sheets()
{
    client().async_list_style_sheets(page_id());
}

void ViewImplementation::request_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier)
{
    client().async_request_style_sheet_source(page_id(), identifier);
}

void ViewImplementation::debug_request(ByteString const& request, ByteString const& argument)
{
    if (request == "dump-session-history"sv)
        dump_session_history("debug-request"sv, SessionHistoryDumpMode::Always);
    if (request == "dump-site-isolation-process-tree"sv) {
        dbgln("{}", SiteIsolationManager::the().dump_process_tree(client(), page_id()));
        return;
    }

    client().async_debug_request(page_id(), request, argument);
}

void ViewImplementation::run_javascript(String const& js_source)
{
    client().async_run_javascript(page_id(), js_source);
}

void ViewImplementation::js_console_input(String const& js_source)
{
    client().async_js_console_input(page_id(), js_source);
}

void ViewImplementation::exit_fullscreen()
{
    client().async_exit_fullscreen(page_id());
}

void ViewImplementation::set_is_fullscreen(Web::ViewportIsFullscreen is_fullscreen)
{
    if (m_is_fullscreen == is_fullscreen)
        return;
    m_is_fullscreen = is_fullscreen;

    handle_resize();
}

// NB: Every page of the tab holds the dialog a document of one of them opened.
void ViewImplementation::alert_closed()
{
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_alert_closed();
    });
}

void ViewImplementation::confirm_closed(bool accepted)
{
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_confirm_closed(accepted);
    });
}

void ViewImplementation::prompt_closed(Optional<Utf16String> const& response)
{
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_prompt_closed(response);
    });
}

// The UI shows one dialog of a kind at a time, so a page asking while another page's is open is told it closed.
static bool another_page_awaits(RefPtr<WebContentPage> const& owner, WebContentPage& requesting_page)
{
    return owner && owner->is_live() && owner.ptr() != &requesting_page;
}

void ViewImplementation::did_request_color_picker(Badge<WebContentPage>, WebContentPage& requesting_page, Color current_color)
{
    if (another_page_awaits(m_color_picker_page, requesting_page)) {
        requesting_page.async_color_picker_update({}, Web::HTML::ColorPickerUpdateState::Closed);
        return;
    }
    m_color_picker_page = requesting_page;
    if (on_request_color_picker)
        on_request_color_picker(current_color);
}

void ViewImplementation::color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    NonnullRefPtr<WebContentPage> target = m_color_picker_page ? *m_color_picker_page : page();
    if (state == Web::HTML::ColorPickerUpdateState::Closed)
        m_color_picker_page.clear();
    if (target->is_live())
        target->async_color_picker_update(picked_color, state);
}

void ViewImplementation::did_request_file_picker(Badge<WebContentPage>, WebContentPage& requesting_page, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (another_page_awaits(m_file_picker_page, requesting_page)) {
        requesting_page.async_file_picker_closed({});
        return;
    }
    m_file_picker_page = requesting_page;
    if (on_request_file_picker)
        on_request_file_picker(accepted_file_types, allow_multiple_files);
}

void ViewImplementation::file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files)
{
    NonnullRefPtr<WebContentPage> target = m_file_picker_page ? *m_file_picker_page : page();
    m_file_picker_page.clear();
    if (target->is_live())
        target->async_file_picker_closed(move(selected_files));
}

void ViewImplementation::did_request_select_dropdown(Badge<WebContentPage>, WebContentPage& requesting_page, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (another_page_awaits(m_select_dropdown_page, requesting_page)) {
        requesting_page.async_select_dropdown_closed({});
        return;
    }
    m_select_dropdown_page = requesting_page;
    if (on_request_select_dropdown)
        on_request_select_dropdown(to_widget_position(content_position), minimum_width / device_pixel_ratio(), move(items));
}

void ViewImplementation::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    NonnullRefPtr<WebContentPage> target = m_select_dropdown_page ? *m_select_dropdown_page : page();
    m_select_dropdown_page.clear();
    if (target->is_live())
        target->async_select_dropdown_closed(selected_item_id);
}

// The page hosting the tab's focused navigable, which the view's own page is when no other process hosts it.
WebContentPage& ViewImplementation::focused_navigable_host() const
{
    auto host = m_top_level_traversable.focused_navigable_host();
    return host ? *host : page();
}

void ViewImplementation::paste_from_clipboard()
{
    auto& host = focused_navigable_host();
    host.async_paste_from_clipboard();
}

void ViewImplementation::set_marked_text_from_input_method(Utf16String const& text)
{
    auto& host = focused_navigable_host();
    host.async_set_marked_text_from_input_method(text);
}

void ViewImplementation::commit_text_from_input_method(Utf16String const& text, i32 replacement_start, i32 replacement_length)
{
    auto& host = focused_navigable_host();
    host.async_commit_text_from_input_method(text, replacement_start, replacement_length);
}

void ViewImplementation::unmark_text_from_input_method()
{
    auto& host = focused_navigable_host();
    host.async_unmark_text_from_input_method();
}

Optional<Web::DevicePixelRect> ViewImplementation::get_input_caret_rect()
{
    // Returns the most-recent caret position pushed by WebContent (see set_input_method_state). Deliberately makes no
    // synchronous IPC request: This is read from inside AppKit text-input callbacks, where blocking can re-enter the
    // run loop and deadlock the input method.
    return m_input_method_state.caret_rect;
}

void ViewImplementation::set_input_method_state(Badge<WebContentPage>, InputMethodState state)
{
    m_input_method_state = move(state);

    if (on_input_method_state_change)
        on_input_method_state_change();
}

Web::Clipboard::SystemClipboardItem ViewImplementation::clipboard_item() const
{
    return Application::the().clipboard_item();
}

void ViewImplementation::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    Application::the().insert_clipboard_item(move(item));
}

void ViewImplementation::toggle_page_mute_state()
{
    m_mute_state = Web::HTML::invert_mute_state(m_mute_state);
    client().async_set_page_mute_state(page_id(), m_mute_state);
}

void ViewImplementation::did_change_audio_play_state(Badge<WebContentPage>, Web::HTML::AudioPlayState play_state)
{
    bool state_changed = false;

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (--m_number_of_elements_playing_audio == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;

    case Web::HTML::AudioPlayState::Playing:
        if (m_number_of_elements_playing_audio++ == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;
    }

    if (state_changed && on_audio_play_state_changed)
        on_audio_play_state_changed(m_audio_play_state);
}

void ViewImplementation::reset_page_media_state()
{
    auto const should_notify_audio_play_state_changed = m_audio_play_state != Web::HTML::AudioPlayState::Paused
        || m_number_of_elements_playing_audio != 0;

    m_audio_play_state = Web::HTML::AudioPlayState::Paused;
    m_number_of_elements_playing_audio = 0;

    if (should_notify_audio_play_state_changed && on_audio_play_state_changed)
        on_audio_play_state_changed(m_audio_play_state);

    if (m_screen_wake_lock_state != Web::ScreenWakeLockState::Released) {
        m_screen_wake_lock_state = Web::ScreenWakeLockState::Released;
        if (on_screen_wake_lock_state_changed)
            on_screen_wake_lock_state_changed(m_screen_wake_lock_state);
    }
}

void ViewImplementation::did_change_screen_wake_lock_state(Badge<WebContentPage>, Web::ScreenWakeLockState wake_lock_state)
{
    if (m_screen_wake_lock_state == wake_lock_state)
        return;

    m_screen_wake_lock_state = wake_lock_state;
    if (on_screen_wake_lock_state_changed)
        on_screen_wake_lock_state_changed(m_screen_wake_lock_state);
}

bool ViewImplementation::needs_beforeunload_check() const
{
    // Each page of the tab reports for the documents it hosts.
    bool needs_beforeunload_check = false;
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        if (page.needs_beforeunload_check())
            needs_beforeunload_check = true;
    });
    return needs_beforeunload_check;
}

void ViewImplementation::did_change_background_color(Badge<WebContentPage>, Gfx::Color color)
{
    set_page_background_color(color);
}

void ViewImplementation::set_page_background_color_to_system_canvas(bool dark)
{
    auto color_scheme = dark ? Web::CSS::PreferredColorScheme::Dark : Web::CSS::PreferredColorScheme::Light;
    m_system_canvas_background_color = Web::CSS::SystemColor::canvas(color_scheme);
    set_page_background_color(preferred_canvas_background_color());
}

void ViewImplementation::set_page_background_color(Gfx::Color color)
{
    m_page_background_color = color;
    if (on_page_background_color_change)
        on_page_background_color_change(m_page_background_color);
}

Gfx::Color ViewImplementation::preferred_canvas_background_color() const
{
    if (m_preferred_color_scheme == Web::CSS::PreferredColorScheme::Dark)
        return Web::CSS::SystemColor::canvas(Web::CSS::PreferredColorScheme::Dark);
    if (m_preferred_color_scheme == Web::CSS::PreferredColorScheme::Light)
        return Web::CSS::SystemColor::canvas(Web::CSS::PreferredColorScheme::Light);
    return m_system_canvas_background_color;
}

void ViewImplementation::did_allocate_backing_stores(Badge<WebContentClient>, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    VERIFY(bitmap_ids.size() == backing_stores.size());
    VERIFY(!bitmap_ids.is_empty());
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI installing {} backing stores for page {} had_usable_bitmap={}",
        backing_stores.size(), page_id(), m_client_state.has_usable_bitmap);
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_shared_image_buffer = move(m_client_state.front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }
    m_client_state.has_usable_bitmap = false;
    m_client_state.front_bitmap.id = bitmap_ids[0];
    m_client_state.front_bitmap.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[0])));
    m_client_state.other_bitmaps.clear();
    m_client_state.other_bitmaps.ensure_capacity(backing_stores.size() - 1);
    for (size_t i = 1; i < backing_stores.size(); ++i) {
        m_client_state.other_bitmaps.append({
            .id = bitmap_ids[i],
            .last_painted_size = {},
            .shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[i]))),
        });
    }
}

void ViewImplementation::update_zoom()
{
    if (m_zoom_level != 1.0) {
        m_reset_zoom_action->set_text(MUST(String::formatted("{}%", round_to<int>(m_zoom_level * 100))));
        m_reset_zoom_action->set_visible(true);
    } else {
        m_reset_zoom_action->set_visible(false);
    }

    client().async_set_zoom_level(page_id(), m_zoom_level);
}

void ViewImplementation::apply_zoom_for_current_host()
{
    auto zoom_level = Application::settings().zoom_for_host(current_host_for_settings());
    if (zoom_level == m_zoom_level)
        return;

    m_zoom_level = zoom_level;
    update_zoom();
}

void ViewImplementation::handle_resize()
{
    if (!m_client_state.page)
        return;

    client().async_set_viewport(page_id(), viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
    Application::the().update_compositor_viewport(client().compositor_context_id_for_page(page_id()), viewport_size().to_type<int>(), Web::Compositor::WindowResizingInProgress::Yes);
    if (m_debugger_paused) {
        m_debugger_overlay_pointer_state.cancel();
        if (m_debugger_overlay_hovered_action.has_value())
            set_debugger_overlay_hovered_action({});
        else
            update_paused_debugger_overlay();
    }
}

void ViewImplementation::initialize_client(CreateNewClient create_new_client, Optional<Web::HTML::CrossProcessId> initial_document_state_id)
{
    if (create_new_client == CreateNewClient::Yes) {
        fail_pending_debugger_requests();
        // NB: The replacement process has no hovered link and cannot clear the outgoing page's status label.
        if (on_link_unhover)
            on_link_unhover();
    }
    if (m_debugger_paused) {
        set_debugger_paused(false);
        if (on_debugger_resumed)
            on_debugger_resumed();
    }
    m_debugger_overlay_pointer_state.cancel();

    if (create_new_client == CreateNewClient::Yes) {
        reject_pending_selection_requests();

        // A queued session-history reset awaiting the previous process's reply can never complete.
        if (auto queue_promise = move(m_pending_session_history_reset_queue_promise))
            queue_promise->resolve({});

        cancel_all_native_geolocation_requests();

        // Only a view's first process creates its traversable. A process replacing another adopts it
        auto navigable_to_adopt = m_top_level_traversable.has_active_browsing_context()
            ? Optional<Web::HTML::CrossProcessId> { m_top_level_traversable.id() }
            : Optional<Web::HTML::CrossProcessId> {};
        auto client_handle = m_client_state.client_handle;
        auto replaces_existing_client = m_client_state.page != nullptr;
        m_client_state = {};
        m_client_state.client_handle = move(client_handle);

        // FIXME: Fail to open the tab, rather than crashing the whole application if this fails.
        auto client_or_error = Application::the().launch_web_content_process(*this, navigable_to_adopt, initial_document_state_id);
        if (client_or_error.is_error())
            warnln("Failed to launch WebContent during process swap: {}", client_or_error.error());
        // Launching the process assigns this view the process's initial page.
        VERIFY(m_client_state.page);
        if (replaces_existing_client)
            m_top_level_traversable.set_replacement_display_page(page());
    } else {
        // The view was given a page of its parent's process before it asked to be initialized.
        VERIFY(m_client_state.page);
    }

    if (m_client_state.client_handle.is_empty()) {
        m_client_state.client_handle = Web::Crypto::generate_random_uuid();
        Application::the().notify_webdriver_window_created(m_client_state.client_handle);
    }
    client().async_set_window_handle(page_id(), m_client_state.client_handle);
    client().async_set_zoom_level(page_id(), m_zoom_level);
    client().async_set_viewport(page_id(), viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
    client().async_set_maximum_frames_per_second(page_id(), m_maximum_frames_per_second);
    client().async_set_has_focus(page_id(), m_top_level_traversable.has_system_focus());
    if (auto focused_navigable_id = m_top_level_traversable.focused_navigable_id(); focused_navigable_id.has_value())
        client().async_set_focused_navigable(page_id(), *focused_navigable_id);
    if (!m_top_level_traversable.has_pending_host())
        client().async_update_visibility_state(page_id(), m_top_level_traversable.id(), m_top_level_traversable.system_visibility_state());
    auto compositor_context_id = client().compositor_context_id_for_page(page_id());
    Application::the().update_compositor_viewport(compositor_context_id, viewport_size().to_type<int>());
    Application::the().update_compositor_context_visibility(compositor_context_id, m_top_level_traversable.system_visibility_state());
    client().async_set_document_cookie_version_buffer(page_id(), m_document_cookie_version_buffer);

    if (m_debugger_is_attached)
        client().async_attach_debugger(page_id());

    client().async_set_page_mute_state(page_id(), m_mute_state);

    if (Application::browser_options().webdriver_browser_endpoint.has_value())
        Application::the().push_webdriver_session_config(*this);

    Application::the().apply_view_options({}, *this);

    languages_changed();
    content_settings_changed();
    send_browsing_behavior(page());
    send_autoplay_settings(page());
    send_global_privacy_control(page());
    geolocation_settings_changed();

    using GeolocationErrorCode = Web::Geolocation::GeolocationPositionError::ErrorCode;

    auto geolocation_error_code = [](Core::GeolocationError const& error) {
        switch (error.type) {
        case Core::GeolocationError::Type::PermissionDenied:
            return GeolocationErrorCode::PermissionDenied;
        case Core::GeolocationError::Type::Timeout:
            return GeolocationErrorCode::Timeout;
        case Core::GeolocationError::Type::PositionUnavailable:
            return GeolocationErrorCode::PositionUnavailable;
        }
        VERIFY_NOT_REACHED();
    };

    auto make_geolocation_success_handler = [this](WebContentPage& request_page, u64 request_id, bool is_watch) {
        auto weak_this = make_weak_ptr();
        auto key = GeolocationRequestKey { request_page, request_id };

        return [weak_this, request_page = NonnullRefPtr<WebContentPage>(request_page), request_id, key, is_watch](Core::GeolocationCoordinates coords) {
            auto* view = weak_this.ptr();
            if (!view || !request_page->is_live())
                return;

            if (is_watch) {
                if (!view->m_geolocation_watch_ids.contains(key))
                    return;
            } else if (!view->m_geolocation_position_request_ids.remove(key)) {
                return;
            }

            if (!Application::settings().geolocation_enabled()) {
                if (auto provider_watch_id = view->m_geolocation_watch_ids.take(key); provider_watch_id.has_value())
                    Application::the().stop_watching_geolocation_position(*provider_watch_id);
                request_page->async_geolocation_position_response(request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
                return;
            }

            request_page->async_geolocation_position_response(request_id,
                { coords.latitude, coords.longitude, coords.accuracy, coords.altitude, coords.altitude_accuracy, coords.heading, coords.speed }, {});
        };
    };

    auto make_geolocation_error_handler = [this, geolocation_error_code](WebContentPage& request_page, u64 request_id, bool is_watch) {
        auto weak_this = make_weak_ptr();
        auto key = GeolocationRequestKey { request_page, request_id };

        return [weak_this, request_page = NonnullRefPtr<WebContentPage>(request_page), request_id, key, is_watch, geolocation_error_code](Core::GeolocationError error) {
            auto* view = weak_this.ptr();
            if (!view || !request_page->is_live())
                return;

            if (is_watch) {
                if (!view->m_geolocation_watch_ids.contains(key))
                    return;
            } else if (!view->m_geolocation_position_request_ids.remove(key)) {
                return;
            }

            auto code = geolocation_error_code(error);
            if (is_watch && code == GeolocationErrorCode::PermissionDenied) {
                auto provider_watch_id = view->m_geolocation_watch_ids.take(key);
                if (provider_watch_id.has_value())
                    Application::the().stop_watching_geolocation_position(*provider_watch_id);
            }

            request_page->async_geolocation_position_response(request_id, {}, to_underlying(code));
        };
    };

    on_request_geolocation_position = [this, geolocation_error_code, make_geolocation_success_handler, make_geolocation_error_handler](WebContentPage& requesting_page, u64 request_id) {
        if (!Application::settings().geolocation_enabled()) {
            requesting_page.async_geolocation_position_response(request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
            return;
        }

        if (auto previous_provider_request_id = m_geolocation_position_request_ids.take({ requesting_page, request_id }); previous_provider_request_id.has_value())
            Application::the().cancel_geolocation_position_request(*previous_provider_request_id);

        auto provider_request_id = Application::the().request_geolocation_position(
            make_geolocation_success_handler(requesting_page, request_id, false),
            make_geolocation_error_handler(requesting_page, request_id, false));
        if (provider_request_id.is_error()) {
            requesting_page.async_geolocation_position_response(request_id, {}, to_underlying(geolocation_error_code(provider_request_id.error())));
            return;
        }

        m_geolocation_position_request_ids.set({ requesting_page, request_id }, provider_request_id.release_value());
    };

    on_cancel_geolocation_position_request = [this](WebContentPage& requesting_page, u64 request_id) {
        auto provider_request_id = m_geolocation_position_request_ids.take({ requesting_page, request_id });
        if (provider_request_id.has_value())
            Application::the().cancel_geolocation_position_request(*provider_request_id);
    };

    on_start_geolocation_position_watch = [this, geolocation_error_code, make_geolocation_success_handler, make_geolocation_error_handler](WebContentPage& requesting_page, u64 request_id) {
        if (!Application::settings().geolocation_enabled()) {
            requesting_page.async_geolocation_position_response(request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
            return;
        }

        if (auto previous_provider_watch_id = m_geolocation_watch_ids.take({ requesting_page, request_id }); previous_provider_watch_id.has_value())
            Application::the().stop_watching_geolocation_position(*previous_provider_watch_id);

        auto provider_watch_id = Application::the().start_watching_geolocation_position(
            make_geolocation_success_handler(requesting_page, request_id, true),
            make_geolocation_error_handler(requesting_page, request_id, true));

        if (provider_watch_id.is_error()) {
            requesting_page.async_geolocation_position_response(request_id, {}, to_underlying(geolocation_error_code(provider_watch_id.error())));
            return;
        }

        m_geolocation_watch_ids.set({ requesting_page, request_id }, provider_watch_id.release_value());
    };

    on_stop_geolocation_position_watch = [this](WebContentPage& requesting_page, u64 request_id) {
        auto provider_watch_id = m_geolocation_watch_ids.take({ requesting_page, request_id });
        if (!provider_watch_id.has_value())
            return;

        Application::the().stop_watching_geolocation_position(*provider_watch_id);
    };

    // If DevTools is connected, notify the new WebContent process.
    if (m_devtools_connected)
        client().async_did_connect_devtools_client(page_id());
}

void ViewImplementation::cancel_all_native_geolocation_requests()
{
    auto geolocation_position_request_ids = move(m_geolocation_position_request_ids);
    for (auto const& request : geolocation_position_request_ids)
        Application::the().cancel_geolocation_position_request(request.value);

    auto geolocation_watch_ids = move(m_geolocation_watch_ids);
    for (auto const& watch : geolocation_watch_ids)
        Application::the().stop_watching_geolocation_position(watch.value);
}

void ViewImplementation::did_start_navigation(Optional<Utf16String> navigation_id, URL::URL const& url)
{
    auto& ongoing = m_top_level_traversable.ensure_ongoing_navigation();
    if (ongoing.sequence_number == 0)
        ongoing.sequence_number = m_top_level_traversable.next_sequence_number();
    ongoing.navigation_id = move(navigation_id);
    ongoing.url = url;
    ongoing.has_started = true;

    set_loading_state(true);
    dump_session_history("did-start-navigation"sv);
}

bool ViewImplementation::did_cancel_navigation(Optional<Utf16String> const& navigation_id)
{
    // A cancel may arrive before a UI-issued load reports its start. A started navigation's cancel must name it.
    auto const& ongoing = m_top_level_traversable.ongoing_navigation();
    auto stale = ongoing.has_value()
        ? ongoing->has_started && navigation_id != ongoing->navigation_id
        : navigation_id != m_top_level_traversable.active_document_load().navigation_id;
    if (stale)
        return false;

    set_loading_state(false);
    if (cancel_uncommitted_top_level_navigation("did-cancel-navigation"sv, false))
        return true;

    m_top_level_traversable.clear_ongoing_navigation();
    m_top_level_traversable.clear_active_document_load();
    if (m_webdriver_navigation_observation.has_value()) {
        auto webdriver_navigation_id = m_webdriver_navigation_observation->navigation_id;
        complete_webdriver_navigation(webdriver_navigation_id);
        return true;
    }

    dump_session_history("did-cancel-navigation-ignored"sv);
    return true;
}

void ViewImplementation::did_cancel_loading(Optional<Utf16String> const& navigation_id)
{
    if (!did_cancel_navigation(navigation_id))
        return;

    auto const& client_url = url();
    if (on_load_finish)
        on_load_finish(client_url);

    for (auto const& [id, listener] : m_navigation_listeners) {
        if (listener.on_load_finish)
            listener.on_load_finish(client_url);
    }
}

bool ViewImplementation::matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const
{
    return m_top_level_traversable.matches_ongoing_navigation(navigation_id);
}

void ViewImplementation::did_finish_navigation()
{
    set_loading_state(false);
    m_top_level_traversable.clear_ongoing_navigation();
    m_top_level_traversable.clear_active_document_load();

    if (!m_webdriver_navigation_observation.has_value())
        return;

    auto navigation_id = m_webdriver_navigation_observation->navigation_id;
    switch (m_webdriver_navigation_observation->completion_source) {
    case WebDriverNavigationCompletionSource::CrashRecovery:
        m_webdriver_navigation_observation->load_completed = true;
        if (m_webdriver_navigation_observation->history_operation_completed)
            complete_webdriver_navigation(navigation_id);
        break;
    case WebDriverNavigationCompletionSource::Load:
        complete_webdriver_navigation(navigation_id);
        break;
    case WebDriverNavigationCompletionSource::HistoryTraversal:
        break;
    }
}

void ViewImplementation::set_loading_state(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;
    m_is_loading = is_loading;
    if (on_loading_state_change)
        on_loading_state_change(is_loading);
}

bool ViewImplementation::cancel_uncommitted_top_level_navigation(StringView reason, bool stop_loading, ReconstructCanceledNavigation reconstruct)
{
    if (!m_top_level_traversable.has_uncommitted_navigation())
        return false;

    auto process_hosts_committed_entry = !m_top_level_traversable.has_pending_host();
    m_top_level_traversable.clear_ongoing_navigation();
    set_loading_state(false);
    if (stop_loading)
        client().async_stop_loading(page_id());

    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    if (!current_entry) {
        if (m_webdriver_navigation_observation.has_value())
            complete_webdriver_navigation(m_webdriver_navigation_observation->navigation_id);
        dump_session_history(reason);
        return true;
    }

    set_url(current_entry->url);
    if (!process_hosts_committed_entry && reconstruct == ReconstructCanceledNavigation::Yes) {
        reconstruct_current_session_history_entry_with_history_operation(reason);
        return true;
    }

    if (m_webdriver_navigation_observation.has_value())
        complete_webdriver_navigation(m_webdriver_navigation_observation->navigation_id);
    dump_session_history(reason);
    return true;
}

void ViewImplementation::run_webdriver_content_command(u64 command_id, Web::WebDriver::SessionBrowsingContext browsing_context, String const& name, JsonValue payload, Vector<String> arguments)
{
    if (m_crash_state.has_value() && !m_crash_state->recovery_started) {
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent has crashed"sv));
        return;
    }

    Optional<Web::HTML::CrossProcessId> navigable_id;
    if (browsing_context == Web::WebDriver::SessionBrowsingContext::Current)
        navigable_id = m_webdriver_current_navigable_id;

    // NB: A command runs in the process hosting the browsing context it runs against.
    RefPtr<WebContentPage> target = this->page();
    if (navigable_id.has_value()) {
        // https://w3c.github.io/webdriver/#dfn-no-longer-open
        // A browsing context is said to be no longer open if its navigable has been destroyed.
        auto navigable = m_top_level_traversable.find(*navigable_id);
        if (navigable.has_value())
            target = m_top_level_traversable.page_hosting(*navigable);
        if (!navigable.has_value() || !target || !target->is_live()) {
            Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
            return;
        }
    }

    if (name == "crash_current_page"sv)
        m_pending_webdriver_crash_commands.set(command_id, *target);
    else
        m_pending_webdriver_commands.set(command_id, *target);
    target->async_run_webdriver_command(command_id, navigable_id, name, move(payload), move(arguments));
}

void ViewImplementation::did_lose_page(Badge<CanonicalTraversable>, WebContentPage& page)
{
    // NB: The view fails the commands of its own page when it replaces the process or recovers from its crash.
    if (&page == &this->page())
        return;

    m_pending_webdriver_commands.remove_all_matching([&](u64 command_id, NonnullRefPtr<WebContentPage> const& command_page) {
        if (command_page.ptr() != &page)
            return false;
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Browsing context was discarded while executing the command"sv));
        return true;
    });
}

void ViewImplementation::did_set_webdriver_current_browsing_context(Badge<WebContentPage>, u64 command_id, Web::HTML::CrossProcessId navigable_id)
{
    if (!m_pending_webdriver_commands.contains(command_id))
        return;

    if (auto navigable = m_top_level_traversable.find(navigable_id); navigable.has_value())
        set_webdriver_current_browsing_context(*navigable);
}

void ViewImplementation::set_webdriver_current_browsing_context_to_top_level()
{
    set_webdriver_current_browsing_context(m_top_level_traversable);
}

// https://w3c.github.io/webdriver/#dfn-set-the-current-browsing-context
void ViewImplementation::set_webdriver_current_browsing_context(CanonicalNavigable const& navigable)
{
    if (!navigable.parent()) {
        m_webdriver_current_navigable_id = {};
        m_webdriver_current_parent_navigable_id = {};
        return;
    }

    // 1. Set session's current browsing context to context.
    m_webdriver_current_navigable_id = navigable.id();

    // 2. Set the session's current parent browsing context to the parent browsing context of context, if that context
    //    exists, or null otherwise.
    m_webdriver_current_parent_navigable_id = navigable.parent()->id();
}

// 11.7 Switch To Parent Frame, https://w3c.github.io/webdriver/#dfn-switch-to-parent-frame
void ViewImplementation::switch_webdriver_to_parent_frame(Function<void(Web::WebDriver::Response)> on_complete)
{
    // 1. If session's current browsing context is already the top-level browsing context:
    if (!m_webdriver_current_navigable_id.has_value()) {
        // 1. If session's current browsing context is no longer open, return error with error code no such window.
        // NB: The view is the current top-level browsing context, so it is open.

        // 2. Return success with data null.
        on_complete(JsonValue {});
        return;
    }

    // 2. If session's current parent browsing context is no longer open, return error with error code no such window.
    if (!m_webdriver_current_parent_navigable_id.has_value() || !m_top_level_traversable.find(*m_webdriver_current_parent_navigable_id).has_value()) {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    // 3. Try to handle any user prompts with session.
    run_webdriver_user_prompt_handling([weak_this = make_weak_ptr(), on_complete = move(on_complete)](Web::WebDriver::Response response) mutable {
        if (response.is_error() || !weak_this) {
            on_complete(move(response));
            return;
        }

        // 4. If session's current parent browsing context is not null, set the current browsing context with session and
        //    current parent browsing context.
        if (auto parent = weak_this->m_top_level_traversable.find(*weak_this->m_webdriver_current_parent_navigable_id); parent.has_value())
            weak_this->set_webdriver_current_browsing_context(*parent);

        // FIXME: 5. Update any implementation-specific state that would result from the user selecting session's current browsing context for interaction, without altering OS-level focus.

        // 6. Return success with data null.
        on_complete(JsonValue {});
    });
}

void ViewImplementation::did_complete_webdriver_content_command(Badge<WebContentPage>, u64 command_id, Web::WebDriver::Response response)
{
    if (m_pending_webdriver_crash_commands.contains(command_id)) {
        // WebContent acknowledges the command before its deferred process exit. Keep the command pending until the
        // crash handler has created the replacement process and started session-history recovery.
        if (response.is_error()) {
            m_pending_webdriver_crash_commands.remove(command_id);
            Application::the().complete_webdriver_content_command(command_id, move(response));
        }
        return;
    }

    if (m_pending_webdriver_commands.remove(command_id))
        Application::the().complete_webdriver_content_command(command_id, move(response));
}

void ViewImplementation::did_close_browsing_context(Badge<WebContentPage>)
{
    reject_pending_selection_requests();

    auto window_handle = move(m_client_state.client_handle);

    // Headless views retain their closed children. Remove the view from routing immediately so a command racing
    // with the close cannot be sent to a page that no longer exists.
    all_views().remove(m_view_id);
    m_top_level_traversable.discard_displaced_document_host();
    m_top_level_traversable.discard_opener_pages();
    if (m_client_state.page) {
        client().unregister_view(page_id());
        m_client_state.page = nullptr;
    }

    if (!window_handle.is_empty())
        Application::the().notify_webdriver_window_closed(window_handle);

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while handling user prompts"sv));

    auto pending_commands = move(m_pending_webdriver_commands);
    fail_webdriver_content_commands_after_window_close(pending_commands);
    auto pending_crash_commands = move(m_pending_webdriver_crash_commands);
    fail_webdriver_content_commands_after_window_close(pending_crash_commands);

    auto pending_navigation_completion_requests = move(m_pending_webdriver_navigation_completion_requests);
    for (auto& request : pending_navigation_completion_requests) {
        if (request.value->timer)
            request.value->timer->stop();
        request.value->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while waiting for navigation"sv));
    }
}

void ViewImplementation::run_webdriver_user_prompt_handling(Function<void(Web::WebDriver::Response)> on_complete)
{
    // A dormant replacement process cannot have a user prompt belonging to the crashed document.
    if (m_crash_state.has_value()) {
        on_complete(JsonValue {});
        return;
    }

    auto request_id = m_next_webdriver_user_prompt_request_id++;
    m_pending_webdriver_user_prompt_requests.set(request_id, move(on_complete));
    client().async_run_webdriver_user_prompt_handling(page_id(), request_id);
}

void ViewImplementation::did_complete_webdriver_user_prompt_handling(Badge<WebContentPage>, u64 request_id, Web::WebDriver::Response response)
{
    if (auto on_complete = m_pending_webdriver_user_prompt_requests.take(request_id); on_complete.has_value())
        on_complete.value()(move(response));
}

Optional<ViewImplementation&> ViewImplementation::find_view_by_handle(StringView handle)
{
    for (auto& view : all_views()) {
        if (view.value->handle() == handle)
            return *view.value;
    }
    return {};
}

void ViewImplementation::load_for_webdriver_navigation(URL::URL const& url)
{
    prepare_for_navigation_after_crash(url);
    auto navigation_id = generate_navigation_id();
    m_top_level_traversable.set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = url,
        .navigation_id = navigation_id,
        .sequence_number = m_top_level_traversable.next_sequence_number(),
    });
    client().async_load_url(page_id(), url, Web::Bindings::NavigationHistoryBehavior::Auto, move(navigation_id));
}

void ViewImplementation::did_start_webdriver_navigation()
{
    set_loading_state(true);
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::Load);
}

void ViewImplementation::wait_for_webdriver_navigation_completion(Optional<u64> page_load_timeout, Function<void(Web::WebDriver::Response)> on_complete)
{
    if (!m_webdriver_navigation_observation.has_value()) {
        on_complete(JsonValue {});
        return;
    }

    auto request_id = m_next_webdriver_navigation_completion_request_id++;
    auto request = make<WebDriverNavigationCompletionRequest>();
    request->on_complete = move(on_complete);
    m_pending_webdriver_navigation_completion_requests.set(request_id, move(request));

    if (page_load_timeout.has_value()) {
        auto timer_interval = *page_load_timeout > NumericLimits<int>::max() ? NumericLimits<int>::max() : static_cast<int>(*page_load_timeout);
        auto timer = Core::Timer::create_single_shot(timer_interval, [this, request_id] {
            complete_webdriver_navigation_completion(request_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::Timeout, "Navigation timed out"sv));
        });
        timer->start();
        m_pending_webdriver_navigation_completion_requests.get(request_id).value()->timer = move(timer);
    }
}

void ViewImplementation::complete_webdriver_navigation_completion(u64 request_id, Web::WebDriver::Response response)
{
    auto maybe_request = m_pending_webdriver_navigation_completion_requests.take(request_id);
    if (!maybe_request.has_value())
        return;

    auto request = maybe_request.release_value();
    if (request->timer)
        request->timer->stop();
    request->on_complete(move(response));
}

u64 ViewImplementation::begin_webdriver_navigation(WebDriverNavigationCompletionSource completion_source, Optional<Web::HTML::CrossProcessId> history_operation_id)
{
    m_webdriver_navigation_observation = WebDriverNavigationObservation {
        .completion_source = completion_source,
        .navigation_id = m_next_webdriver_navigation_id++,
        .history_operation_id = history_operation_id,
    };
    return m_webdriver_navigation_observation->navigation_id;
}

void ViewImplementation::complete_webdriver_navigation(u64 navigation_id)
{
    if (!m_webdriver_navigation_observation.has_value()
        || m_webdriver_navigation_observation->navigation_id != navigation_id)
        return;

    m_webdriver_navigation_observation.clear();

    Vector<u64> request_ids;
    request_ids.ensure_capacity(m_pending_webdriver_navigation_completion_requests.size());
    for (auto const& request : m_pending_webdriver_navigation_completion_requests)
        request_ids.unchecked_append(request.key);

    auto view_id = m_view_id;
    for (auto request_id : request_ids) {
        Core::EventLoop::current().deferred_invoke([view_id, request_id] {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value())
                return;
            view->complete_webdriver_navigation_completion(request_id, JsonValue {});
        });
    }
}

void ViewImplementation::complete_webdriver_history_traversal(Web::HTML::CrossProcessId operation_id)
{
    if (!m_webdriver_navigation_observation.has_value()
        || m_webdriver_navigation_observation->completion_source != WebDriverNavigationCompletionSource::HistoryTraversal
        || m_webdriver_navigation_observation->history_operation_id != operation_id) {
        return;
    }

    complete_webdriver_navigation(m_webdriver_navigation_observation->navigation_id);
}

JsonValue ViewImplementation::webdriver_session_history() const
{
    JsonObject serialized;
    serialized.set("currentURL"sv, m_url.serialize());
    serialized.set("webContentProcessID"sv, client().pid());
    serialized.set("backButtonEnabled"sv, m_navigate_back_action->enabled());
    serialized.set("forwardButtonEnabled"sv, m_navigate_forward_action->enabled());
    serialized.set("crashOverlayActive"sv, crash_overlay_active());
    serialized.set("hasOnlyTopLevelUsedSteps"sv, m_top_level_traversable.session_history().has_only_top_level_used_steps());

    if (auto current_used_step_index = m_top_level_traversable.session_history().current_used_step_index(); current_used_step_index.has_value())
        serialized.set("currentUsedStepIndex"sv, *current_used_step_index);
    else
        serialized.set("currentUsedStepIndex"sv, JsonValue {});

    if (auto traversal = m_top_level_traversable.browser_history_traversal_for_testing(); traversal.has_value()) {
        JsonObject pending_traversal;
        pending_traversal.set("targetStep"sv, traversal->target_step);
        pending_traversal.set("targetStepIndex"sv, traversal->target_step_index);
        pending_traversal.set("willChangeTopLevelEntry"sv, traversal->changes_top_level_entry);
        pending_traversal.set("stage"sv, CanonicalTraversable::browser_history_traversal_stage_to_string(traversal->stage));
        serialized.set("pendingSessionHistoryTraversal"sv, move(pending_traversal));
    } else {
        serialized.set("pendingSessionHistoryTraversal"sv, JsonValue {});
    }

    serialized.set("entries"sv, history_json_entries(m_top_level_traversable.session_history()));
    serialized.set("usedSteps"sv, history_json_steps(m_top_level_traversable.session_history()));
    return serialized;
}

String ViewImplementation::ui_process_session_history_for_testing(Badge<WebContentClient>) const
{
    return webdriver_session_history().serialized();
}

void ViewImplementation::update_navigation_action_state()
{
    auto effective_current_index = m_top_level_traversable.effective_current_session_history_step_index();
    m_navigate_back_action->set_enabled(effective_current_index.has_value() && *effective_current_index > 0);
    m_navigate_forward_action->set_enabled(effective_current_index.has_value()
        && *effective_current_index + 1 < m_top_level_traversable.session_history().used_step_count());
}

void ViewImplementation::recover_current_session_history_entry_with_history_operation(RefPtr<WebContentPage> crashed_endpoint)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    auto current_url = current_entry ? current_entry->url : m_url;
    set_url(current_url);
    auto navigation_id = begin_webdriver_navigation(WebDriverNavigationCompletionSource::CrashRecovery);
    m_top_level_traversable.recover_from_web_content_process_crash(move(crashed_endpoint), [this, navigation_id](Web::HTML::HistoryStepResult result, Optional<i32> committed_step) {
        if (result == Web::HTML::HistoryStepResult::Applied) {
            if (committed_step.has_value())
                update_navigation_action_state();
            auto const* current_entry = m_top_level_traversable.session_history().current_entry();
            if (current_entry)
                set_url(current_entry->url);

            if (m_webdriver_navigation_observation.has_value()
                && m_webdriver_navigation_observation->navigation_id == navigation_id
                && m_webdriver_navigation_observation->completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
                m_webdriver_navigation_observation->history_operation_completed = true;
                if (m_webdriver_navigation_observation->load_completed)
                    complete_webdriver_navigation(navigation_id);
            }
        } else {
            complete_webdriver_navigation(navigation_id);
        }
        dump_session_history("recovered-web-content-process-with-history-operation"sv);
    });
}

void ViewImplementation::reconstruct_current_session_history_entry_with_history_operation(StringView reason)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    if (!current_entry)
        return;

    auto current_step = m_top_level_traversable.session_history().current_step();
    VERIFY(current_step.has_value());
    set_url(current_entry->url);
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal);
    m_top_level_traversable.reconstruct_the_history_to_step(*current_step);
    dump_session_history(reason);
}

Optional<SessionHistorySnapshot> ViewImplementation::session_history_snapshot() const
{
    auto const& session_history = m_top_level_traversable.session_history();

    auto current_used_step_index = session_history.current_used_step_index();
    if (!current_used_step_index.has_value())
        return {};

    return SessionHistorySnapshot {
        .entries = session_history.entries(),
        .used_steps = session_history.used_steps(),
        .current_used_step_index = *current_used_step_index,
    };
}

ErrorOr<void> ViewImplementation::restore_session_history_from_snapshot(SessionHistorySnapshot snapshot)
{
    TRY(m_top_level_traversable.restore_session_history_from_ui_snapshot(move(snapshot)));

    reconstruct_current_session_history_entry_with_history_operation("restored-session-history-from-ui-snapshot"sv);
    update_navigation_action_state();
    return {};
}

bool ViewImplementation::capture_session_history_snapshot_for_testing(Badge<WebContentClient>)
{
    m_captured_session_history_snapshot_for_testing = session_history_snapshot();
    return m_captured_session_history_snapshot_for_testing.has_value();
}

bool ViewImplementation::restore_captured_session_history_snapshot_for_testing(Badge<WebContentClient>)
{
    if (!m_captured_session_history_snapshot_for_testing.has_value())
        return false;

    auto result = restore_session_history_from_snapshot(m_captured_session_history_snapshot_for_testing.release_value());
    return !result.is_error();
}

bool ViewImplementation::register_session_store_tab_for_testing(Badge<WebContentClient>)
{
    if (m_session_tab_id.has_value())
        return true;

    auto tab_id = m_session->session_store->tab_opened({ .window_id = {}, .initial_url = m_url, .insertion_index = {}, .is_active = SessionStore::IsActive::No });
    if (tab_id.is_error())
        return false;
    set_session_tab_id(tab_id.value());
    notify_session_history_changed();
    return true;
}

static Optional<size_t> current_top_level_history_entry_index_for_step(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Optional<i32> current_step)
{
    if (!current_step.has_value())
        return {};

    Optional<size_t> current_entry_index;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].step > *current_step)
            break;
        current_entry_index = i;
    }
    return current_entry_index;
}

String ViewImplementation::session_store_tab_state_for_testing(Badge<WebContentClient>) const
{
    JsonObject serialized;
    if (!m_session_tab_id.has_value())
        return serialized.serialized();

    auto cached_state = m_session->session_store->cached_tab_state_for_testing(*m_session_tab_id);
    if (!cached_state.has_value())
        return serialized.serialized();

    serialized.set("url"sv, cached_state->url.serialize());
    if (cached_state->history.has_value()) {
        auto const& history = *cached_state->history;
        Optional<i32> current_step;
        if (history.current_used_step_index < history.used_steps.size())
            current_step = history.used_steps[history.current_used_step_index];
        serialized.set("entries"sv, history_json_entries(history.entries, current_top_level_history_entry_index_for_step(history.entries, current_step)));
        serialized.set("usedSteps"sv, history_json_steps(history.used_steps, history.current_used_step_index));
    }
    return serialized.serialized();
}

void ViewImplementation::notify_session_history_changed()
{
    if (!m_session_tab_id.has_value())
        return;
    auto url = m_url;
    if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
        url = current_entry->url;
    SessionStore::TabStateUpdate update {
        .tab_id = *m_session_tab_id,
        .history = session_history_snapshot(),
        .url = move(url),
    };
    m_session->session_store->update_tab_state(move(update));
}

NonnullRefPtr<Core::Promise<Empty>> ViewImplementation::reset_session_history_for_testing()
{
    m_pending_session_history_reset_for_testing = Core::Promise<Empty>::construct();
    // The algorithms this test control replaces run on the session history traversal queue. Keep that ordering by
    // sending the reset at its queue position, and hold the queue until WebContent returns the retained active
    // entry so canonical history is reset before anything queued behind the reset runs.
    m_top_level_traversable.append_history_queue_steps([this](NonnullRefPtr<Core::Promise<Empty>> promise) {
        m_pending_session_history_reset_queue_promise = move(promise);
        if (auto* test_connection = client().test_connection()) {
            client().transport().flush();
            test_connection->async_reset_session_history_for_testing(page_id());
        }
    });
    return *m_pending_session_history_reset_for_testing;
}

void ViewImplementation::request_history_operation(Badge<WebContentPage>, WebContentPage& requesting_page, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters)
{
    auto sequence_number = m_top_level_traversable.next_sequence_number();

    auto reloads_top_level = parameters.visit(
        [this](Web::ReloadHistoryOperationParameters const& parameters) {
            return parameters.navigable_id == m_top_level_traversable.id();
        },
        [](auto const&) { return false; });
    auto finalizes_top_level_cross_document_navigation = parameters.visit(
        [this](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const& parameters) {
            return parameters.navigable_id == m_top_level_traversable.id();
        },
        [](auto const&) { return false; });
    auto requested_operation_completion = [this, reloads_top_level, finalizes_top_level_cross_document_navigation](Web::HTML::HistoryStepResult result, Optional<i32> committed_step) {
        if (reloads_top_level && result != Web::HTML::HistoryStepResult::Applied)
            did_cancel_navigation({});
        if (finalizes_top_level_cross_document_navigation && result == Web::HTML::HistoryStepResult::Applied) {
            if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
                set_url(current_entry->url);
        }
        if (committed_step.has_value())
            update_navigation_action_state();
        dump_session_history("requested-history-operation-complete"sv);
    };

    m_top_level_traversable.enqueue_history_operation(operation_id, move(parameters), requesting_page, sequence_number, move(requested_operation_completion));
}

void ViewImplementation::did_reset_session_history_for_testing(
    Badge<WebContentClient>, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    auto promise = move(m_pending_session_history_reset_for_testing);
    m_top_level_traversable.reset_session_history_for_testing(move(active_entry));
    m_webdriver_navigation_observation.clear();
    // The reset installed the process's own active entry as the canonical current entry.
    m_top_level_traversable.did_activate_document_in_display_page();
    update_navigation_action_state();

    if (auto queue_promise = move(m_pending_session_history_reset_queue_promise))
        queue_promise->resolve({});
    if (promise)
        promise->resolve({});
}

void ViewImplementation::dump_session_history(StringView reason, SessionHistoryDumpMode mode) const
{
    if (mode == SessionHistoryDumpMode::IfDebuggingEnabled && !history_debug_enabled())
        return;

    auto traversal = m_top_level_traversable.browser_history_traversal_for_testing();

    Optional<URL::URL> loading_url;
    if (m_top_level_traversable.ongoing_navigation().has_value())
        loading_url = m_top_level_traversable.ongoing_navigation()->url;

    dbgln("[History] UI session history page={} pid={} reason={} url='{}' uncommitted_navigation={} loading_url={} pending_traversal_target={} pending_traversal_stage={} pending_same_document_entries={} back={} forward={} entries={}",
        page_id(),
        client().pid(),
        reason,
        m_url,
        m_top_level_traversable.has_uncommitted_navigation(),
        loading_url,
        traversal.has_value() ? Optional<i32> { traversal->target_step } : Optional<i32> {},
        traversal.has_value() ? CanonicalTraversable::browser_history_traversal_stage_to_string(traversal->stage) : "none"sv,
        m_top_level_traversable.pending_same_document_session_history_entries_for_debug(),
        m_navigate_back_action->enabled(),
        m_navigate_forward_action->enabled(),
        history_log_entries(m_top_level_traversable.session_history()));
}

void ViewImplementation::handle_web_content_process_crash()
{
    auto failed_url = m_url;
    Optional<URL::URL> navigation_to_retry;
    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    if (!current_entry || current_entry->url != failed_url)
        navigation_to_retry = failed_url;

    reject_pending_selection_requests();
    // Nothing will finish the input events the crashed process still held, and the events another process holds
    // for this tab are stale once its tree is abandoned or restored.
    m_pending_input_events.clear();

    set_loading_state(false);
    m_top_level_traversable.clear_ongoing_navigation();
    m_top_level_traversable.clear_active_document_load();

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while handling user prompts"sv));

    auto pending_commands = move(m_pending_webdriver_commands);
    auto pending_crash_commands = move(m_pending_webdriver_crash_commands);
    for (auto const& command : pending_commands)
        Application::the().complete_webdriver_content_command(command.key, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while executing the command"sv));

    auto crashed_during_recovery = m_webdriver_navigation_observation.has_value()
        && m_webdriver_navigation_observation->completion_source == WebDriverNavigationCompletionSource::CrashRecovery;
    m_webdriver_navigation_observation.clear();
    auto pending_navigation_completion_requests = move(m_pending_webdriver_navigation_completion_requests);
    for (auto& request : pending_navigation_completion_requests) {
        if (request.value->timer)
            request.value->timer->stop();
        request.value->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while waiting for navigation"sv));
    }

    enum class RecoveryMode {
        ShowOverlay,
        Restore,
        PrepareForNextNavigation,
    };

    auto recovery_mode = RecoveryMode::ShowOverlay;
    if (auto const& headless_mode = Application::browser_options().headless_mode; headless_mode.has_value())
        recovery_mode = *headless_mode == HeadlessMode::Test ? RecoveryMode::PrepareForNextNavigation : RecoveryMode::Restore;

    if (recovery_mode == RecoveryMode::ShowOverlay) {
        dbgln("\033[31;1mWebContent process crashed!\033[0m Last page loaded: {}", failed_url);
        dbgln("Consider raising an issue at https://github.com/LadybirdBrowser/ladybird/issues/new/choose");
    }

    reset_page_media_state();

    constexpr size_t max_reasonable_crash_count = 5U;
    auto crashed_repeatedly = false;
    if (recovery_mode == RecoveryMode::Restore) {
        // Only count crashes from the replacement page towards the loop limit. Separate, explicitly triggered
        // crashes may happen close together without indicating that automatic restoration is crash-looping.
        if (!crashed_during_recovery)
            m_crash_count = 0;
        crashed_repeatedly = ++m_crash_count >= max_reasonable_crash_count;
        m_repeated_crash_timer->restart();
    }

    RefPtr<WebContentPage> crashed_endpoint = m_client_state.page;

    respawn_web_content_process_after_crash();

    // A repeatedly crashing headless page is left dormant so the next WebDriver test or explicit navigation can
    // proceed without feeding an automatic restore loop.
    if (recovery_mode == RecoveryMode::Restore && !crashed_repeatedly) {
        recover_current_session_history_entry_with_history_operation(move(crashed_endpoint));
    } else if (recovery_mode == RecoveryMode::ShowOverlay) {
        m_top_level_traversable.abandon_after_web_content_process_crash();
        set_crash_state(CrashState {
            .failed_url = move(failed_url),
            .navigation_to_retry = move(navigation_to_retry),
        });
    } else {
        m_top_level_traversable.abandon_after_web_content_process_crash();
    }

    for (auto const& command : pending_crash_commands)
        Application::the().complete_webdriver_content_command(command.key, JsonValue {});
}

void ViewImplementation::respawn_web_content_process_after_crash()
{
    // NB: In-flight operations are preserved: crash recovery redispatches them onto the replacement process.
    Optional<Web::HTML::CrossProcessId> initial_document_state_id;
    if (auto const* target_entry = m_top_level_traversable.ongoing_browser_history_traversal_target_entry())
        initial_document_state_id = target_entry->document_state.id;
    if (!initial_document_state_id.has_value()) {
        if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
            initial_document_state_id = current_entry->document_state.id;
    }
    initialize_client(CreateNewClient::Yes, initial_document_state_id);
    VERIFY(m_client_state.page);

    // Don't keep a stale backup bitmap around.
    m_backup_shared_image_buffer = nullptr;

    handle_resize();
}

void ViewImplementation::prepare_for_navigation_after_crash(Optional<URL::URL> navigation_to_retry)
{
    if (!m_crash_state.has_value())
        return;

    m_crash_state->navigation_to_retry = move(navigation_to_retry);
    m_crash_state->recovery_started = true;
}

void ViewImplementation::set_crash_state(Optional<CrashState> state)
{
    auto active = state.has_value();
    m_crash_state = move(state);
    if (on_crash_overlay_state_change)
        on_crash_overlay_state_change(active);
}

String ViewImplementation::crash_overlay_failed_url() const
{
    return m_crash_state.has_value() ? m_crash_state->failed_url.serialize() : m_url.serialize();
}

String ViewImplementation::current_host_for_settings() const
{
    if (auto const& state = m_top_level_traversable.replicated_state(); state.has_value()) {
        if (state->active_document_url.host().has_value())
            return MUST(String::from_utf8(state->active_document_url.serialized_host()));
        if (state->active_document_url.scheme() == "about"sv)
            return MUST(String::from_utf8(state->active_document_url.serialize_path()));
    }

    return {};
}

void ViewImplementation::languages_changed()
{
    auto const& languages = Application::settings().languages();
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_languages(languages);
    });
}

void ViewImplementation::content_settings_changed()
{
    apply_zoom_for_current_host();

    // FIXME: This is not a "debug request". Add a proper endpoint for this toggle.
    debug_request("set-force-dark"sv, Application::settings().content_settings().enable_force_dark ? "on"sv : "off"sv);
}

void ViewImplementation::browsing_behavior_changed()
{
    m_top_level_traversable.for_each_hosting_page(send_browsing_behavior);
}

void ViewImplementation::autoplay_settings_changed()
{
    m_top_level_traversable.for_each_hosting_page(send_autoplay_settings);
}

void ViewImplementation::global_privacy_control_changed()
{
    m_top_level_traversable.for_each_hosting_page(send_global_privacy_control);
}

void ViewImplementation::geolocation_settings_changed()
{
    using ErrorCode = Web::Geolocation::GeolocationPositionError::ErrorCode;

    if (Application::web_content_options().is_test_mode != IsTestMode::Yes && !Application::settings().geolocation_enabled()) {
        auto geolocation_position_request_ids = move(m_geolocation_position_request_ids);
        for (auto const& request : geolocation_position_request_ids) {
            Application::the().cancel_geolocation_position_request(request.value);
            if (request.key.page->is_live())
                request.key.page->async_geolocation_position_response(request.key.request_id, {}, to_underlying(ErrorCode::PermissionDenied));
        }

        auto geolocation_watch_ids = move(m_geolocation_watch_ids);
        for (auto const& watch : geolocation_watch_ids) {
            Application::the().stop_watching_geolocation_position(watch.value);
            if (watch.key.page->is_live())
                watch.key.page->async_geolocation_position_response(watch.key.request_id, {}, to_underlying(ErrorCode::PermissionDenied));
        }
    }

    send_geolocation_emulated_position(page());
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        if (&page != &this->page())
            send_geolocation_emulated_position(page);
    });
}

void ViewImplementation::send_geolocation_emulated_position(WebContentPage& page)
{
    using ErrorCode = Web::Geolocation::GeolocationPositionError::ErrorCode;

    if (Application::web_content_options().is_test_mode == IsTestMode::Yes)
        page.async_set_geolocation_emulated_position({ 37.7647658, -122.4345892, 100.0, 0.0, 0.0, 0.0, 0.0 }, {});
    else if (Application::settings().geolocation_enabled())
        page.async_set_geolocation_emulated_position({}, {});
    else
        page.async_set_geolocation_emulated_position({}, to_underlying(ErrorCode::PermissionDenied));
}

void ViewImplementation::bookmarks_changed()
{
    update_bookmark_action();
}

void ViewImplementation::update_bookmark_action()
{
    Application::the().update_bookmark_action_for_current_web_view();

    auto is_bookmarked = Application::bookmark_store().is_bookmarked(url());

    m_toggle_bookmark_action->set_tooltip(is_bookmarked ? "Remove bookmark"sv : "Bookmark this page"sv);
    m_toggle_bookmark_action->set_engaged(is_bookmarked);
}

static ErrorOr<LexicalPath> save_screenshot(Gfx::Bitmap const* bitmap)
{
    if (!bitmap)
        return Error::from_string_literal("Failed to take a screenshot");

    auto path = TRY([] -> ErrorOr<LexicalPath> {
        if (auto const& screenshot_path = Application::browser_options().screenshot_path; screenshot_path.has_value())
            return LexicalPath { *screenshot_path };

        auto file = AK::UnixDateTime::now().to_byte_string("screenshot-%Y-%m-%d-%H-%M-%S.png"sv);
        return Application::the().path_for_downloaded_file(file);
    }());

    auto encoded = TRY(Gfx::PNGWriter::encode(*bitmap));

    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted(encoded));

    return path;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_screenshot(ScreenshotType type)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    switch (type) {
    case ScreenshotType::Visible: {
        RefPtr<Gfx::Bitmap> visible_bitmap;
        if (m_client_state.has_usable_bitmap) {
            VERIFY(m_client_state.front_bitmap.shared_image_buffer);
            visible_bitmap = m_client_state.front_bitmap.shared_image_buffer->bitmap_if_present();
        } else if (m_backup_shared_image_buffer) {
            visible_bitmap = m_backup_shared_image_buffer->bitmap_if_present();
        }
        if (visible_bitmap) {
            if (auto result = save_screenshot(visible_bitmap.ptr()); result.is_error())
                promise->reject(result.release_error());
            else
                promise->resolve(result.release_value());
        } else {
            // GPU-shared backing stores have no CPU-visible pixels, so ask WebContent to paint a screenshot for us.
            m_pending_screenshot = promise;
            client().async_take_document_screenshot(page_id());
        }
        break;
    }

    case ScreenshotType::Full:
        m_pending_screenshot = promise;
        client().async_take_document_screenshot(page_id());
        break;
    }

    return promise;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_dom_node_screenshot(Web::UniqueNodeID node_id)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    m_pending_screenshot = promise;
    client().async_take_dom_node_screenshot(page_id(), node_id);

    return promise;
}

void ViewImplementation::did_receive_screenshot(Badge<WebContentPage>, Gfx::ShareableBitmap const& screenshot)
{
    VERIFY(m_pending_screenshot);

    if (auto result = save_screenshot(screenshot.bitmap()); result.is_error())
        m_pending_screenshot->reject(result.release_error());
    else
        m_pending_screenshot->resolve(result.release_value());

    m_pending_screenshot = nullptr;
}

NonnullRefPtr<Core::Promise<String>> ViewImplementation::request_internal_page_info(PageInfoType type)
{
    auto promise = Core::Promise<String>::construct();

    if (m_pending_info_request) {
        // For simplicity, only allow one info request at a time for now.
        promise->reject(Error::from_string_literal("A page info request is already in progress"));
        return promise;
    }

    m_pending_info_request = promise;
    client().async_request_internal_page_info(page_id(), type);

    return promise;
}

void ViewImplementation::did_receive_internal_page_info(Badge<WebContentPage>, PageInfoType, Optional<Core::AnonymousBuffer> const& info)
{
    VERIFY(m_pending_info_request);

    String info_string;
    if (!info.has_value()) {
        info_string = "(no page)"_string;
    } else {
        info_string = MUST(String::from_utf8(info->bytes()));
    }
    m_pending_info_request->resolve(move(info_string));
    m_pending_info_request = nullptr;
}

ErrorOr<LexicalPath> ViewImplementation::dump_gc_graph()
{
    auto promise = request_internal_page_info(PageInfoType::GCGraph);
    auto gc_graph_json = TRY(promise->await());

    LexicalPath path { Core::StandardPaths::tempfile_directory() };
    path = path.append(TRY(AK::UnixDateTime::now().to_string("gc-graph-%Y-%m-%d-%H-%M-%S.js"sv)));

    // Write as a .js file so gc-heap-explorer.html can load it via <script> tag (avoiding CORS issues with file:// URLs)
    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted("var GC_GRAPH_DUMP = "sv.bytes()));
    TRY(dump_file->write_until_depleted(gc_graph_json.bytes()));
    TRY(dump_file->write_until_depleted(";\n"sv.bytes()));

    return path;
}

void ViewImplementation::set_user_style_sheet(String const& source)
{
    m_user_style_sheet = source;
    m_top_level_traversable.for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_user_style(source);
    });
}

void ViewImplementation::initialize_context_menus()
{
    auto& application = Application::the();

    m_navigate_back_action = Action::create("Go Back"sv, ActionID::NavigateBack, [this]() {
        traverse_the_history_by_delta(-1);
    });
    m_navigate_forward_action = Action::create("Go Forward"sv, ActionID::NavigateForward, [this]() {
        traverse_the_history_by_delta(+1);
    });
    m_navigate_back_action->set_enabled(false);
    m_navigate_forward_action->set_enabled(false);

    m_toggle_bookmark_action = Action::create("Toggle Bookmark"sv, ActionID::ToggleBookmarkViaToolbar, [this]() {
        Application::the().toggle_bookmark_for_view(*this);
    });
    update_bookmark_action();

    m_reset_zoom_action = Action::create("100%"sv, ActionID::ResetZoomViaToolbar, [this]() {
        reset_zoom();
    });
    m_reset_zoom_action->set_tooltip("Reset zoom level"sv);
    m_reset_zoom_action->set_visible(false);

    m_search_selected_text_action = Action::create("Search Selected Text"sv, ActionID::SearchSelectedText, [this]() {
        auto const& search_engine = Application::settings().search_engine();
        if (!search_engine.has_value())
            return;

        auto url_string = search_engine->format_search_query_for_navigation(*m_search_text);
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
    });
    m_search_selected_text_action->set_visible(false);

    m_look_up_selected_text_action = Action::create("Look Up"sv, ActionID::LookUpSelectedText, [this] {
        if (!m_look_up.has_value() || !on_request_dictionary_lookup)
            return;

        on_request_dictionary_lookup(*m_look_up, m_look_up_position);
    });
    m_look_up_selected_text_action->set_visible(false);

    auto take_and_save_screenshot = [this](auto type) {
        take_screenshot(type)
            ->when_resolved([](auto const& path) {
                Application::the().display_download_confirmation_dialog("Screenshot"sv, path);
            })
            .when_rejected([](auto const& error) {
                if (error.is_errno() && error.code() == ECANCELED)
                    return;

                auto error_message = MUST(String::formatted("{}", error));
                Application::the().display_error_dialog(error_message);
            });
    };

    m_take_visible_screenshot_action = Action::create("Take Visible Screenshot"sv, ActionID::TakeVisibleScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Visible);
    });
    m_take_full_screenshot_action = Action::create("Take Full Screenshot"sv, ActionID::TakeFullScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Full);
    });

    m_open_in_new_tab_action = Action::create("Open in New Tab"sv, ActionID::OpenInNewTab, [this]() {
        open_url_in_new_tab(m_context_menu_url, Web::HTML::ActivateTab::No);
    });
    if (m_is_private == IsPrivate::No) {
        m_open_in_new_window_action = Action::create("Open in New Window"sv, ActionID::OpenInNewWindow, [this]() {
            open_url_in_new_window(m_context_menu_url, IsPrivate::No);
        });
    }
    if (application.supports_private_browsing_windows()) {
        m_open_in_new_private_window_action = Action::create("Open in New Private Window"sv, ActionID::OpenInNewPrivateWindow, [this]() {
            open_url_in_new_window(m_context_menu_url, IsPrivate::Yes);
        });
    }
    m_download_linked_file_action = Action::create("Download Linked File"sv, ActionID::DownloadLinkedFile, [this]() {
        download_context_menu_url(PromptForPath::No);
    });
    m_download_linked_file_as_action = Action::create("Download Linked File As..."sv, ActionID::DownloadLinkedFileAs, [this]() {
        download_context_menu_url(PromptForPath::Yes);
    });
    m_copy_url_action = Action::create("Copy URL"sv, ActionID::CopyURL, [this]() {
        Application::the().insert_clipboard_entry({ "text/plain"_string, url_text_to_copy(m_context_menu_url) });
    });

    m_open_image_action = Action::create("Open Image"sv, ActionID::OpenImage, [this]() {
        load(m_context_menu_url);
    });
    m_save_image_action = Action::create("Save Image As..."sv, ActionID::SaveImage, [this]() {
        download_context_menu_url(PromptForPath::Yes);
    });
    m_copy_image_action = Action::create("Copy Image"sv, ActionID::CopyImage, [this]() {
        if (!m_image_context_menu_bitmap.has_value())
            return;

        auto bitmap = m_image_context_menu_bitmap.release_value();
        if (!bitmap.is_valid())
            return;

        auto encoded = Gfx::PNGWriter::encode(*bitmap.bitmap());
        if (encoded.is_error())
            return;

        Application::the().insert_clipboard_entry({ "image/png"_string, ByteString { encoded.value().bytes() } });
    });

    m_open_audio_action = Action::create("Open Audio"sv, ActionID::OpenAudio, [this]() {
        load(m_context_menu_url);
    });
    m_open_video_action = Action::create("Open Video"sv, ActionID::OpenVideo, [this]() {
        load(m_context_menu_url);
    });
    m_media_play_action = Action::create("Play"sv, ActionID::PlayMedia, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_play_state(); });
    });
    m_media_pause_action = Action::create("Pause"sv, ActionID::PauseMedia, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_play_state(); });
    });
    m_media_mute_action = Action::create("Mute"sv, ActionID::MuteMedia, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_mute_state(); });
    });
    m_media_unmute_action = Action::create("Unmute"sv, ActionID::UnmuteMedia, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_mute_state(); });
    });
    m_media_show_controls_action = Action::create("Show Controls"sv, ActionID::ShowControls, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_controls_state(); });
    });
    m_media_hide_controls_action = Action::create("Hide Controls"sv, ActionID::HideControls, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_controls_state(); });
    });
    m_media_loop_action = Action::create_checkable("Loop"sv, ActionID::ToggleMediaLoopState, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_loop_state(); });
    });
    m_media_enter_fullscreen_action = Action::create("Full Screen"sv, ActionID::EnterFullscreen, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_fullscreen_state(); });
    });
    m_media_exit_fullscreen_action = Action::create("Exit Full Screen"sv, ActionID::ExitFullscreen, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_fullscreen_state(); });
    });

    auto add_open_url_actions = [this](Menu& menu) {
        menu.add_action(*m_open_in_new_tab_action);
        if (m_open_in_new_window_action)
            menu.add_action(*m_open_in_new_window_action);
        if (m_open_in_new_private_window_action)
            menu.add_action(*m_open_in_new_private_window_action);
    };

    auto add_text_edit_actions = [&application](Menu& menu) {
        menu.add_action(application.cut_selection_action());
        menu.add_action(application.copy_selection_action());
        menu.add_action(application.paste_action());
        menu.add_action(application.select_all_action());
    };

    m_page_context_menu = Menu::create("Page Context Menu"sv);
    m_page_context_menu->add_action(*m_look_up_selected_text_action);
    m_page_context_menu->add_action(*m_navigate_back_action);
    m_page_context_menu->add_action(*m_navigate_forward_action);
    m_page_context_menu->add_action(application.reload_action());
    m_page_context_menu->add_separator();
    add_text_edit_actions(*m_page_context_menu);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_search_selected_text_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_take_visible_screenshot_action);
    m_page_context_menu->add_action(*m_take_full_screenshot_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.view_source_action());

    m_link_context_menu = Menu::create("Link Context Menu"sv);
    m_link_context_menu->add_action(*m_look_up_selected_text_action);
    add_open_url_actions(*m_link_context_menu);
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(*m_download_linked_file_action);
    m_link_context_menu->add_action(*m_download_linked_file_as_action);
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(*m_copy_url_action);

    m_selected_text_link_context_menu = Menu::create("Selected Text Link Context Menu"sv);
    m_selected_text_link_context_menu->add_action(*m_look_up_selected_text_action);
    add_open_url_actions(*m_selected_text_link_context_menu);
    m_selected_text_link_context_menu->add_separator();
    add_text_edit_actions(*m_selected_text_link_context_menu);
    m_selected_text_link_context_menu->add_separator();
    m_selected_text_link_context_menu->add_action(*m_search_selected_text_action);

    m_image_context_menu = Menu::create("Image Context Menu"sv);
    m_image_context_menu->add_action(*m_look_up_selected_text_action);
    m_image_context_menu->add_action(*m_open_image_action);
    m_image_context_menu->add_action(*m_open_in_new_tab_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_save_image_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_copy_image_action);
    m_image_context_menu->add_action(*m_copy_url_action);

    m_media_context_menu = Menu::create("Media Context Menu"sv);
    m_media_context_menu->add_action(*m_look_up_selected_text_action);
    m_media_context_menu->add_action(*m_media_play_action);
    m_media_context_menu->add_action(*m_media_pause_action);
    m_media_context_menu->add_action(*m_media_mute_action);
    m_media_context_menu->add_action(*m_media_unmute_action);
    m_media_context_menu->add_action(*m_media_show_controls_action);
    m_media_context_menu->add_action(*m_media_hide_controls_action);
    m_media_context_menu->add_action(*m_media_loop_action);
    m_media_context_menu->add_action(*m_media_enter_fullscreen_action);
    m_media_context_menu->add_action(*m_media_exit_fullscreen_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_open_audio_action);
    m_media_context_menu->add_action(*m_open_video_action);
    m_media_context_menu->add_action(*m_open_in_new_tab_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_copy_url_action);

    auto add_bookmark_action = Action::create("Add Bookmark..."sv, ActionID::AddBookmark, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        application.display_add_bookmark_dialog(bookmark_id->target_folder_id)
            ->when_resolved([](Application::AddBookmarkDialogResult result) {
                Application::bookmark_store().add_bookmark(move(result.bookmark.url), move(result.bookmark.title), move(result.bookmark.favicon_hash), result.target_folder_id);
            });
    });
    auto add_bookmark_folder_action = Action::create("Add Folder..."sv, ActionID::AddBookmarkFolder, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        application.display_add_bookmark_folder_dialog()
            ->when_resolved([bookmark_id = bookmark_id.release_value()](BookmarkItem::Folder folder) {
                Application::bookmark_store().add_folder(move(folder.title), bookmark_id.target_folder_id);
            });
    });

    m_bookmarks_bar_context_menu = Menu::create("Bookmarks Bar Context Menu"sv);
    m_bookmarks_bar_context_menu->add_action(add_bookmark_action);
    m_bookmarks_bar_context_menu->add_action(add_bookmark_folder_action);

    m_bookmark_context_menu = Menu::create("Bookmark Context Menu"sv);
    m_bookmark_context_menu->add_action(Action::create("Open in New Tab"sv, ActionID::OpenInNewTab, []() {
        auto& application = Application::the();

        if (auto bookmark_id = application.bookmark_item_id_for_context_menu(); bookmark_id.has_value())
            application.open_bookmark_in_new_tab(bookmark_id->id, Web::HTML::ActivateTab::Yes);
    }));
    if (m_is_private == IsPrivate::No) {
        m_bookmark_context_menu->add_action(Action::create("Open in New Window"sv, ActionID::OpenInNewWindow, []() {
            auto& application = Application::the();

            if (auto bookmark_id = application.bookmark_item_id_for_context_menu(); bookmark_id.has_value())
                application.open_bookmark_in_new_window(bookmark_id->id, IsPrivate::No);
        }));
    }
    if (application.supports_private_browsing_windows()) {
        m_bookmark_context_menu->add_action(Action::create("Open in New Private Window"sv, ActionID::OpenInNewPrivateWindow, []() {
            auto& application = Application::the();

            if (auto bookmark_id = application.bookmark_item_id_for_context_menu(); bookmark_id.has_value())
                application.open_bookmark_in_new_window(bookmark_id->id, IsPrivate::Yes);
        }));
    }
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_action(Action::create("Copy URL"sv, ActionID::CopyURL, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        auto bookmark = Application::bookmark_store().find_item_by_id(bookmark_id->id);
        if (!bookmark.has_value() || !bookmark->is_bookmark())
            return;

        application.insert_clipboard_entry({ "text/plain"_string, url_text_to_copy(bookmark->bookmark().url) });
    }));
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_action(Action::create("Edit Bookmark..."sv, ActionID::EditBookmark, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        auto current_bookmark = Application::bookmark_store().find_item_by_id(bookmark_id->id);
        if (!current_bookmark.has_value() || !current_bookmark->is_bookmark())
            return;

        application.display_edit_bookmark_dialog(current_bookmark->bookmark())
            ->when_resolved([bookmark_id = bookmark_id.release_value()](BookmarkItem::Bookmark bookmark) {
                Application::bookmark_store().edit_bookmark(bookmark_id.id, move(bookmark.url), move(bookmark.title));
            });
    }));
    m_bookmark_context_menu->add_action(Action::create("Delete Bookmark"sv, ActionID::DeleteBookmark, []() {
        if (auto bookmark_id = Application::the().bookmark_item_id_for_context_menu(); bookmark_id.has_value())
            Application::bookmark_store().remove_item(bookmark_id->id);
    }));
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_action(add_bookmark_action);
    m_bookmark_context_menu->add_action(add_bookmark_folder_action);

    m_bookmark_folder_context_menu = Menu::create("Bookmark Folder Context Menu"sv);
    m_bookmark_folder_context_menu->add_action(Action::create("Open All in Tabs"sv, ActionID::OpenAllBookmarksInTabs, []() {
        auto& application = Application::the();

        if (auto bookmark_id = application.bookmark_item_id_for_context_menu(); bookmark_id.has_value())
            application.open_bookmark_folder_in_new_tabs(bookmark_id->id);
    }));
    m_bookmark_folder_context_menu->add_separator();
    m_bookmark_folder_context_menu->add_action(Action::create("Edit Folder..."sv, ActionID::EditBookmarkFolder, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        auto current_folder = Application::bookmark_store().find_item_by_id(bookmark_id->id);
        if (!current_folder.has_value() || !current_folder->is_folder())
            return;

        application.display_edit_bookmark_folder_dialog(current_folder->folder())
            ->when_resolved([bookmark_id = bookmark_id.release_value()](BookmarkItem::Folder folder) {
                Application::bookmark_store().edit_folder(bookmark_id.id, move(folder.title));
            });
    }));
    m_bookmark_folder_context_menu->add_action(Action::create("Delete Folder"sv, ActionID::DeleteBookmarkFolder, []() {
        if (auto bookmark_id = Application::the().bookmark_item_id_for_context_menu(); bookmark_id.has_value())
            Application::bookmark_store().remove_item(bookmark_id->id);
    }));
    m_bookmark_folder_context_menu->add_separator();
    m_bookmark_folder_context_menu->add_action(add_bookmark_action);
    m_bookmark_folder_context_menu->add_action(add_bookmark_folder_action);
}

void ViewImplementation::update_look_up_selected_text_action(Optional<DictionaryLookup> const& lookup, Gfx::IntPoint content_position)
{
    m_look_up = on_request_dictionary_lookup ? lookup : OptionalNone {};
    m_look_up_position = to_widget_position(content_position);
    if (m_look_up.has_value() && m_look_up->baseline_origin.has_value())
        m_look_up_position = to_widget_position(*m_look_up->baseline_origin);
    m_look_up_selected_text_action->set_visible(m_look_up.has_value());
}

void ViewImplementation::request_context_menu_dictionary_lookup(Function<void(Optional<DictionaryLookup> const&)> on_complete)
{
    if (!on_request_dictionary_lookup) {
        Optional<DictionaryLookup> lookup;
        on_complete(lookup);
        return;
    }

    selected_text_for_dictionary_lookup()->when_resolved([on_complete = move(on_complete)](auto& lookup) mutable {
        on_complete(lookup);
    });
}

void ViewImplementation::reject_pending_selection_requests()
{
    ++m_context_menu_request_id;

    auto reject_requests = [](auto& requests) {
        auto pending_requests = move(requests);
        for (auto& request : pending_requests)
            request.value->reject(Error::from_string_literal("WebContent was replaced before completing a selection request"));
    };

    reject_requests(m_pending_selected_text_requests);
    reject_requests(m_pending_selected_text_for_lookup_requests);
    reject_requests(m_pending_select_word_for_dictionary_lookup_requests);
    reject_requests(m_pending_cut_selected_text_requests);
}

void ViewImplementation::did_request_page_context_menu(Badge<WebContentPage>, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target)
{
    auto request_id = ++m_context_menu_request_id;
    auto weak_this = make_weak_ptr();
    request_context_menu_dictionary_lookup([weak_this, request_id, content_position, for_input_events_target](auto const& lookup) {
        if (!weak_this || request_id != weak_this->m_context_menu_request_id)
            return;

        auto show_context_menu = [weak_this, request_id, content_position, for_input_events_target, lookup](Optional<String> selected_text) {
            if (!weak_this || request_id != weak_this->m_context_menu_request_id)
                return;

            auto& cut_selection_action = Application::the().cut_selection_action();
            cut_selection_action.set_visible(for_input_events_target == Web::ContextMenuForInputEventsTarget::Yes);

            auto const& search_engine = Application::settings().search_engine();
            weak_this->m_search_text = search_engine.has_value() ? selected_text : OptionalNone {};
            auto selected_text_url = selected_text.has_value() ? url_from_text(*selected_text) : OptionalNone {};
            weak_this->update_look_up_selected_text_action(lookup, content_position);

            ScopeGuard guard { [&]() {
                cut_selection_action.set_visible(true);
                weak_this->m_search_text.clear();
            } };

            if (weak_this->m_search_text.has_value()) {
                weak_this->m_search_selected_text_action->set_text(search_engine->format_search_query_for_display(*weak_this->m_search_text));
                weak_this->m_search_selected_text_action->set_visible(true);
            } else {
                weak_this->m_search_selected_text_action->set_visible(false);
            }

            if (selected_text_url.has_value() && weak_this->m_selected_text_link_context_menu->on_activation) {
                weak_this->m_context_menu_url = selected_text_url.release_value();
                weak_this->m_open_in_new_tab_action->set_text("Open in New Tab"sv);
                weak_this->m_selected_text_link_context_menu->on_activation(weak_this->to_widget_position(content_position));
                return;
            }

            if (weak_this->m_page_context_menu->on_activation)
                weak_this->m_page_context_menu->on_activation(weak_this->to_widget_position(content_position));
        };

        if (lookup.has_value()) {
            show_context_menu(lookup->text);
            return;
        }

        weak_this->selected_text_with_whitespace_collapsed()->when_resolved([show_context_menu = move(show_context_menu)](auto& selected_text) mutable {
            show_context_menu(selected_text);
        });
    });
}

void ViewImplementation::did_request_link_context_menu(Badge<WebContentPage>, Gfx::IntPoint content_position, URL::URL url)
{
    auto request_id = ++m_context_menu_request_id;
    auto weak_this = make_weak_ptr();
    request_context_menu_dictionary_lookup([weak_this, request_id, content_position, url = move(url)](auto const& lookup) mutable {
        if (!weak_this || request_id != weak_this->m_context_menu_request_id)
            return;

        weak_this->m_context_menu_url = move(url);
        weak_this->update_look_up_selected_text_action(lookup, content_position);

        weak_this->m_open_in_new_tab_action->set_text("Open in New Tab"sv);

        switch (url_type(weak_this->m_context_menu_url)) {
        case URLType::Email:
            weak_this->m_copy_url_action->set_text("Copy Email Address"sv);
            break;
        case URLType::Telephone:
            weak_this->m_copy_url_action->set_text("Copy Phone Number"sv);
            break;
        case URLType::Other:
            weak_this->m_copy_url_action->set_text("Copy Link Address"sv);
            break;
        }

        if (weak_this->m_link_context_menu->on_activation)
            weak_this->m_link_context_menu->on_activation(weak_this->to_widget_position(content_position));
    });
}

void ViewImplementation::download_context_menu_url(PromptForPath prompt_for_path)
{
    // URLs with an opaque path, such as data: URLs, do not contain a usable filename, so the default filename is
    // used instead.
    auto suggested_filename = m_context_menu_url.has_an_opaque_path() ? ByteString {} : m_context_menu_url.basename();
    auto download_path = prompt_for_path == PromptForPath::Yes
        ? Application::the().path_for_downloaded_file(suggested_filename)
        : Application::the().default_path_for_downloaded_file(suggested_filename);
    if (download_path.is_error())
        return;

    Application::the().file_downloader().download_file(is_private(), m_context_menu_url, download_path.release_value());
}

void ViewImplementation::did_request_image_context_menu(Badge<WebContentPage>, Gfx::IntPoint content_position, URL::URL url, Optional<Gfx::ShareableBitmap> bitmap)
{
    auto request_id = ++m_context_menu_request_id;
    auto weak_this = make_weak_ptr();
    request_context_menu_dictionary_lookup([weak_this, request_id, content_position, url = move(url), bitmap = move(bitmap)](auto const& lookup) mutable {
        if (!weak_this || request_id != weak_this->m_context_menu_request_id)
            return;

        weak_this->m_context_menu_url = move(url);
        weak_this->m_image_context_menu_bitmap = move(bitmap);
        weak_this->update_look_up_selected_text_action(lookup, content_position);

        weak_this->m_open_in_new_tab_action->set_text("Open Image in New Tab"sv);
        weak_this->m_copy_url_action->set_text("Copy Image URL"sv);

        weak_this->m_copy_image_action->set_enabled(weak_this->m_image_context_menu_bitmap.has_value());

        if (weak_this->m_image_context_menu->on_activation)
            weak_this->m_image_context_menu->on_activation(weak_this->to_widget_position(content_position));
    });
}

void ViewImplementation::send_to_media_context_menu_page(Function<void(WebContentPage&)> const& send)
{
    NonnullRefPtr<WebContentPage> target = m_media_context_menu_page ? *m_media_context_menu_page : page();
    if (target->is_live())
        send(target);
}

void ViewImplementation::did_request_media_context_menu(Badge<WebContentPage>, WebContentPage& requesting_page, Gfx::IntPoint content_position, Web::Page::MediaContextMenu menu)
{
    m_media_context_menu_page = requesting_page;
    auto request_id = ++m_context_menu_request_id;
    auto weak_this = make_weak_ptr();
    request_context_menu_dictionary_lookup([weak_this, request_id, content_position, menu = move(menu)](auto const& lookup) mutable {
        if (!weak_this || request_id != weak_this->m_context_menu_request_id)
            return;

        weak_this->m_context_menu_url = move(menu.media_url);
        weak_this->update_look_up_selected_text_action(lookup, content_position);

        weak_this->m_open_in_new_tab_action->set_text(menu.is_video ? "Open Video in New Tab"sv : "Open Audio in new Tab"sv);
        weak_this->m_copy_url_action->set_text(menu.is_video ? "Copy Video URL"sv : "Copy Audio URL"sv);

        weak_this->m_open_audio_action->set_visible(!menu.is_video);
        weak_this->m_open_video_action->set_visible(menu.is_video);

        weak_this->m_media_play_action->set_visible(!menu.is_playing);
        weak_this->m_media_pause_action->set_visible(menu.is_playing);

        weak_this->m_media_mute_action->set_visible(!menu.is_muted);
        weak_this->m_media_unmute_action->set_visible(menu.is_muted);

        weak_this->m_media_show_controls_action->set_visible(!menu.has_user_agent_controls);
        weak_this->m_media_hide_controls_action->set_visible(menu.has_user_agent_controls);

        weak_this->m_media_loop_action->set_checked(menu.is_looping);

        weak_this->m_media_enter_fullscreen_action->set_visible(menu.is_video && !menu.is_fullscreen);
        weak_this->m_media_exit_fullscreen_action->set_visible(menu.is_video && menu.is_fullscreen);

        if (weak_this->m_media_context_menu->on_activation)
            weak_this->m_media_context_menu->on_activation(weak_this->to_widget_position(content_position));
    });
}

u64 ViewImplementation::add_navigation_listener(NavigationListener listener)
{
    auto id = m_next_navigation_listener_id++;
    m_navigation_listeners.set(id, move(listener));
    return id;
}

void ViewImplementation::remove_navigation_listener(u64 listener_id)
{
    m_navigation_listeners.remove(listener_id);
}

void ViewImplementation::request_close()
{
    if (m_debugger_paused) {
        resume_debugger(DebuggerResumeMode::Continue);
        set_debugger_paused(false);
    }

    if (needs_beforeunload_check()) {
        client().async_request_close(page_id());
        return;
    }

    client().request_close(page_id());
}

void ViewImplementation::force_close()
{
    client().async_force_close(page_id());
}

Function<void()> ViewImplementation::prepare_for_immediate_close()
{
    VERIFY(!needs_beforeunload_check());

    if (m_debugger_paused) {
        resume_debugger(DebuggerResumeMode::Continue);
        set_debugger_paused(false);
    }

    NonnullRefPtr closing_page = page();
    closing_page->client().prepare_for_detached_close(closing_page->id());
    return [closing_page = move(closing_page)] {
        closing_page->async_request_close();
    };
}

}
