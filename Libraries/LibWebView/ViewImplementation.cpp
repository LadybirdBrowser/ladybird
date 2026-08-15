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
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Menu.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

static HashMap<u64, ViewImplementation*>& all_views()
{
    static NeverDestroyed<HashMap<u64, ViewImplementation*>> views;
    return *views;
}

static void fail_webdriver_content_commands_after_process_replacement(HashTable<u64> const& command_ids)
{
    for (auto command_id : command_ids)
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent was replaced while executing the command"sv));
}

static u64 s_view_count = 1; // This has to start at 1 for Firefox DevTools.

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

Optional<ViewImplementation&> ViewImplementation::find_view_for_traversable(CanonicalTraversable const& traversable)
{
    Optional<ViewImplementation&> result;
    for_each_view([&](auto& view) {
        if (&view.traversable() != &traversable)
            return IterationDecision::Continue;
        result = view;
        return IterationDecision::Break;
    });
    return result;
}

ViewImplementation::ViewImplementation(IsPrivate is_private)
    : m_is_private(is_private)
    , m_document_cookie_version_buffer(Core::create_shared_version_buffer())
    , m_view_id(s_view_count++)
{
    all_views().set(m_view_id, this);

    initialize_context_menus();

    m_repeated_crash_timer = Core::Timer::create_single_shot(1000, [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    });

    m_top_level_traversable.on_session_history_changed = [this] {
        notify_session_history_changed();
    };

    on_request_file = [this](auto const& path, auto request_id) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);

        if (file.is_error())
            client().async_handle_file_return(page_id(), file.error().code(), {}, request_id);
        else
            client().async_handle_file_return(page_id(), 0, IPC::File::adopt_file(file.release_value()), request_id);
    };
}

ViewImplementation::~ViewImplementation()
{
    cancel_all_native_geolocation_requests();

    if (!m_client_state.client_handle.is_empty())
        Application::the().notify_webdriver_window_closed(m_client_state.client_handle);

    all_views().remove(m_view_id);

    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);

    if (m_is_private == IsPrivate::Yes)
        Application::the().maybe_close_private_browsing_session();
}

WebContentClient& ViewImplementation::client()
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

WebContentClient const& ViewImplementation::client() const
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

u64 ViewImplementation::page_id() const
{
    VERIFY(m_client_state.client);
    return m_client_state.page_index;
}

void ViewImplementation::set_url(URL::URL url)
{
    if (m_url == url)
        return;

    auto previous_host = current_host();
    m_url = move(url);
    update_bookmark_action();

    if (current_host() != previous_host)
        apply_zoom_for_current_host();

    if (on_url_change)
        on_url_change(m_url);
}

void ViewImplementation::set_title(Badge<WebContentClient>, Utf16String title)
{
    if (m_title == title)
        return;

    m_title = move(title);

    if (on_title_change)
        on_title_change(m_title);
}

void ViewImplementation::set_favicon(Badge<WebContentClient>, Optional<Gfx::Bitmap const&> favicon)
{
    m_favicon_base64_png.clear();

    if (favicon.has_value()) {
        if (auto favicon_png = Gfx::PNGWriter::encode(*favicon); !favicon_png.is_error()) {
            if (auto favicon_base64_png = encode_base64(favicon_png.value().bytes()); !favicon_base64_png.is_error())
                m_favicon_base64_png = favicon_base64_png.release_value();
        }

        if (m_favicon_base64_png.has_value()) {
            if (m_is_private == IsPrivate::No)
                Application::bookmark_store().update_favicon(m_url, *m_favicon_base64_png);
            if (!m_should_suppress_history_for_current_load)
                Application::history_store(m_is_private).update_favicon(m_url, *m_favicon_base64_png);
        }
    }

    if (on_favicon_change)
        on_favicon_change(favicon);
}

void ViewImplementation::create_new_process_for_cross_site_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling, Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot)
{
    auto pending_webdriver_command_ids = move(m_pending_webdriver_command_ids);
    auto pending_webdriver_crash_command_ids = move(m_pending_webdriver_crash_command_ids);

    dump_session_history("before-process-swap"sv);

    if (m_client_state.has_usable_bitmap) {
        // Keep showing the old page until the new WebContent process paints its first frame.
        m_backup_shared_image_buffer = move(m_client_state.front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }

    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);

    reset_page_media_state();

    Optional<Web::HTML::CrossProcessId> initial_document_state_id;
    if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
        initial_document_state_id = current_entry->document_state.id;
    initialize_client(CreateNewClient::Yes, initial_document_state_id);
    m_client_state.site_url = url;
    VERIFY(m_client_state.client);

    if (on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();

    handle_resize();

    m_should_suppress_history_for_current_load = false;
    m_should_suppress_history_for_next_load = false;
    set_loading_state(true);
    ensure_ongoing_top_level_navigation().load = OngoingTopLevelNavigation::Load {
        .navigation_id = {},
        .url = url,
        .uses_replacement_process = true,
        .is_uncommitted = true,
    };
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::Load);
    m_last_stopped_load_url.clear();
    set_url(url);
    dump_session_history("process-swap-load"sv);
    client().async_load_url_with_document_resource(page_id(), url, document_resource,
        history_handling, move(source_snapshot));
    dump_session_history("after-process-swap-load"sv);

    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_command_ids);
    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_crash_command_ids);
}

void ViewImplementation::replace_web_content_process_for_history_traversal(Web::HTML::CrossProcessId target_document_state_id, URL::URL const& target_url)
{
    auto pending_webdriver_command_ids = move(m_pending_webdriver_command_ids);
    auto pending_webdriver_crash_command_ids = move(m_pending_webdriver_crash_command_ids);

    dump_session_history("before-history-traversal-process-swap"sv);

    if (m_client_state.has_usable_bitmap) {
        m_backup_shared_image_buffer = move(m_client_state.front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }

    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);

    reset_page_media_state();
    m_history_operation_handling_for_next_client = HistoryOperationHandling::Preserve;
    initialize_client(CreateNewClient::Yes, target_document_state_id);
    m_history_operation_handling_for_next_client = HistoryOperationHandling::Abandon;
    m_client_state.site_url = target_url;
    VERIFY(m_client_state.client);

    if (on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();

    handle_resize();
    dump_session_history("after-history-traversal-process-swap"sv);

    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_command_ids);
    fail_webdriver_content_commands_after_process_replacement(pending_webdriver_crash_command_ids);
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
    if (did_swap_bitmap && on_ready_to_paint)
        on_ready_to_paint();
}

void ViewImplementation::release_backing_store(i32 bitmap_id)
{
    client().notify_presented_bitmap_ready_to_paint(page_id(), bitmap_id);
}

void ViewImplementation::set_window_position(Gfx::IntPoint position)
{
    client().async_set_window_position(m_client_state.page_index, position.to_type<Web::DevicePixels>());
}

void ViewImplementation::set_window_size(Gfx::IntSize size)
{
    client().async_set_window_size(m_client_state.page_index, size.to_type<Web::DevicePixels>());
}

void ViewImplementation::did_update_window_rect()
{
    client().async_did_update_window_rect(m_client_state.page_index);
}

void ViewImplementation::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    if (m_top_level_traversable.system_visibility_state() == visibility_state)
        return;

    m_top_level_traversable.set_system_visibility_state(visibility_state);
    client().async_set_system_visibility_state(m_client_state.page_index, visibility_state);
}

void ViewImplementation::load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    set_loading_state(true);
    m_is_showing_crash_page = false;
    m_should_suppress_history_for_current_load = false;
    m_should_suppress_history_for_next_load = false;
    ensure_ongoing_top_level_navigation().load = OngoingTopLevelNavigation::Load {
        .navigation_id = {},
        .url = url,
        .is_uncommitted = true,
    };
    m_last_stopped_load_url.clear();
    if (url.scheme() != "javascript"sv)
        set_url(url);
    dump_session_history("load"sv);
    client().async_load_url(page_id(), url, history_handling);
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
    set_loading_state(true);
    clear_ongoing_top_level_navigation_load();
    m_last_stopped_load_url.clear();
    m_is_showing_crash_page = false;
    m_should_suppress_history_for_current_load = false;
    m_should_suppress_history_for_next_load = false;
    client().async_load_html(page_id(), html);
}

void ViewImplementation::load_crash_page_html(StringView html, URL::URL const& crashed_url)
{
    set_loading_state(true);
    clear_ongoing_top_level_navigation_load();
    m_last_stopped_load_url.clear();
    m_is_showing_crash_page = true;
    m_should_suppress_history_for_current_load = true;
    m_should_suppress_history_for_next_load = true;
    set_url(crashed_url);
    client().async_load_html_with_url(page_id(), html, crashed_url);
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

    set_loading_state(true);
    auto ongoing_url = m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()
        ? move(m_ongoing_top_level_navigation->load->url)
        : Optional<URL::URL> {};
    ensure_ongoing_top_level_navigation().load = OngoingTopLevelNavigation::Load { .navigation_id = {}, .url = move(ongoing_url) };
    if (m_is_showing_crash_page) {
        m_is_showing_crash_page = false;
        m_should_suppress_history_for_current_load = false;
        m_should_suppress_history_for_next_load = false;
        recover_current_session_history_entry_with_history_operation();
        return;
    }

    m_is_showing_crash_page = false;
    m_should_suppress_history_for_current_load = false;
    m_should_suppress_history_for_next_load = false;
    m_top_level_traversable.prepare_for_reload();
    update_navigation_action_state();
    dump_session_history("reload-mark-current-entry-reload-pending"sv);
    client().async_reload(page_id());
}

void ViewImplementation::stop_loading()
{
    if (!m_is_loading)
        return;
    m_last_stopped_load_url = m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()
        ? m_ongoing_top_level_navigation->load->url
        : Optional<URL::URL> {};
    if (cancel_uncommitted_top_level_navigation("stop-loading"sv, true))
        return;
    set_loading_state(false);
    clear_ongoing_top_level_navigation_load();
    client().async_stop_loading(page_id());
}

void ViewImplementation::traverse_the_history_by_delta(
    int delta,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    m_top_level_traversable.traverse_the_history_by_delta(delta, check_for_cancelation, move(on_ready));
}

bool ViewImplementation::cancel_uncommitted_top_level_navigation_for_browser_traversal()
{
    auto navigation_used_replacement_process = m_ongoing_top_level_navigation.has_value()
        && m_ongoing_top_level_navigation->load.has_value()
        && m_ongoing_top_level_navigation->load->uses_replacement_process;
    auto canceled = cancel_uncommitted_top_level_navigation("traverse-canceled-pending-navigation"sv, true, ReconstructCanceledNavigation::No);
    VERIFY(canceled);
    return navigation_used_replacement_process;
}

void ViewImplementation::traverse_the_history_to_step(
    i32 step,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    m_top_level_traversable.traverse_the_history_to_step(step, check_for_cancelation, move(on_ready));
}

void ViewImplementation::will_apply_history_traversal_step(u64 operation_id)
{
    m_should_suppress_history_for_current_load = false;
    m_should_suppress_history_for_next_load = false;
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    if (m_ongoing_top_level_navigation.has_value()
        && m_ongoing_top_level_navigation->webdriver_completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
        m_ongoing_top_level_navigation->history_operation_id = operation_id;
    } else {
        begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal, operation_id);
    }
    update_navigation_action_state();
    dump_session_history("traverse-apply-history-step"sv);
}

void ViewImplementation::did_resume_history_traversal(u64 operation_id)
{
    if (m_ongoing_top_level_navigation.has_value()
        && m_ongoing_top_level_navigation->webdriver_completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
        m_ongoing_top_level_navigation->history_operation_id = operation_id;
        return;
    }

    if (!m_ongoing_top_level_navigation.has_value()
        || m_ongoing_top_level_navigation->webdriver_completion_source != WebDriverNavigationCompletionSource::HistoryTraversal) {
        begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal, operation_id);
        return;
    }

    m_ongoing_top_level_navigation->history_operation_id = operation_id;
}

void ViewImplementation::did_apply_top_level_history_traversal_step(u64 operation_id)
{
    complete_webdriver_history_traversal(operation_id);
}

void ViewImplementation::did_finish_history_traversal(u64 operation_id, Web::HTML::HistoryStepResult result)
{
    if (result == Web::HTML::HistoryStepResult::Applied) {
        if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
            set_url(current_entry->url);
    }

    complete_webdriver_history_traversal(operation_id);
    update_navigation_action_state();
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
        auto history_entry = Application::history_store(m_is_private).entry_for_url(target_entry.url);
        auto url = target_entry.url.serialize();
        auto title = history_entry.has_value() && history_entry->title.has_value() && !history_entry->title->is_empty()
            ? move(*history_entry->title)
            : url;
        items.append({
            target_step,
            move(title),
            move(url),
            history_entry.has_value() ? move(history_entry->favicon_base64_png) : Optional<String> {},
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
        Application::settings().set_zoom_for_host(current_host(), m_zoom_level);
}

void ViewImplementation::zoom_out()
{
    if (m_zoom_level <= ZOOM_MIN_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level - ZOOM_STEP) * 100) / 100.0;
    update_zoom();

    if (m_is_private == IsPrivate::No)
        Application::settings().set_zoom_for_host(current_host(), m_zoom_level);
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
    client().async_reset_zoom(m_client_state.page_index);

    if (m_is_private == IsPrivate::No)
        Application::settings().set_zoom_for_host(current_host(), m_zoom_level);
}

void ViewImplementation::enqueue_input_event(Web::InputEvent event)
{
    auto* key_event = event.get_pointer<Web::KeyEvent>();
    auto* mouse_event = event.get_pointer<Web::MouseEvent>();
    auto* pinch_event = event.get_pointer<Web::PinchEvent>();

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
                m_client_state.page_index, position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            if (client().send_async_scroll_to_compositor(m_client_state.page_index, position, delta_in_device_pixels))
                mouse_event->async_scroll_performed_default_action = true;
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor wheel bypass result for page {}: {}",
                m_client_state.page_index, mouse_event->async_scroll_performed_default_action ? "accepted"sv : "rejected"sv);
        } else if (client().handle_mouse_event_in_compositor(m_client_state.page_index, *mouse_event)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor handled mouse event for page {} at {},{}",
                m_client_state.page_index, mouse_event->position.x().value(), mouse_event->position.y().value());
            return;
        }
    }
    if (Application::web_content_options().enable_async_scrolling == EnableAsyncScrolling::Yes
        && m_client_state.has_usable_bitmap
        && pinch_event) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI attempting compositor pinch bypass for page {} at {},{} scale delta {}",
            m_client_state.page_index, pinch_event->position.x().value(), pinch_event->position.y().value(), pinch_event->scale_delta);
        auto handled = client().handle_pinch_event_in_compositor(m_client_state.page_index, *pinch_event);
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor pinch bypass result for page {}: {}",
            m_client_state.page_index, handled ? "accepted"sv : "rejected"sv);
    }

    // Send the next event over to the WebContent to be handled by JS. We'll later get a message to say whether JS
    // prevented the default event behavior, at which point we either discard or handle that event, and then try to
    // process the next one.
    m_pending_input_events.enqueue(move(event));

    m_pending_input_events.tail().visit(
        [this](Web::KeyEvent const& event) {
            client().async_key_event(m_client_state.page_index, event.clone_without_browser_data());
        },
        [this](Web::MouseEvent const& event) {
            client().dispatch_mouse_event_to_web_content(m_client_state.page_index, event);
        },
        [this](Web::DragEvent& event) {
            auto cloned_event = event.clone_without_browser_data();
            cloned_event.files = move(event.files);

            client().async_drag_event(m_client_state.page_index, cloned_event);
        },
        [this](Web::PinchEvent const& event) {
            client().async_pinch_event(m_client_state.page_index, event);
        });
}

void ViewImplementation::handle_external_url(Badge<WebContentClient>, URL::URL url, URL::Origin initiator_origin, bool has_transient_activation)
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

void ViewImplementation::did_finish_handling_input_event(Badge<WebContentClient>, Web::EventResult event_result)
{
    auto event = m_pending_input_events.dequeue();

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

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    set_page_background_color(preferred_canvas_background_color());

    client().async_set_preferred_color_scheme(page_id(), color_scheme);
}

void ViewImplementation::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    client().async_set_preferred_contrast(page_id(), contrast);
}

void ViewImplementation::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    client().async_set_preferred_motion(page_id(), motion);
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

ErrorOr<Core::SharedVersionIndex> ViewImplementation::ensure_document_cookie_version_index(Badge<WebContentClient>, String const& domain)
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

ByteString ViewImplementation::selected_text()
{
    return client().get_selected_text(page_id());
}

ByteString ViewImplementation::cut_selected_text()
{
    return client().cut_selected_text(page_id());
}

Optional<String> ViewImplementation::selected_text_with_whitespace_collapsed()
{
    auto selected_text = MUST(Web::Infra::strip_and_collapse_whitespace(this->selected_text()));
    if (selected_text.is_empty())
        return OptionalNone {};
    return selected_text;
}

Optional<DictionaryLookup> ViewImplementation::selected_text_for_dictionary_lookup()
{
    auto lookup = client().get_selected_text_for_lookup(page_id());
    if (!lookup.has_value())
        return {};

    auto selected_text = MUST(Web::Infra::strip_and_collapse_whitespace(lookup->text));
    if (selected_text.is_empty())
        return {};

    lookup->text = move(selected_text);
    return lookup;
}

bool ViewImplementation::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    if (!on_request_dictionary_lookup)
        return false;

    auto lookup = selected_text_for_dictionary_lookup();
    if (!lookup.has_value()) {
        if (!client().select_word_for_dictionary_lookup(page_id(), to_content_position(widget_position).to_type<Web::DevicePixels>()))
            return false;

        lookup = selected_text_for_dictionary_lookup();
    }
    if (!lookup.has_value())
        return false;

    auto lookup_position = lookup->baseline_origin.has_value() ? to_widget_position(*lookup->baseline_origin) : widget_position;
    on_request_dictionary_lookup(*lookup, lookup_position);
    return true;
}

void ViewImplementation::select_all()
{
    client().async_select_all(page_id());
}

void ViewImplementation::undo()
{
    client().async_undo(page_id());
}

void ViewImplementation::set_editing_history_state(Badge<WebContentClient>, bool can_undo, bool can_redo)
{
    m_can_undo = can_undo;
    m_can_redo = can_redo;
    Application::the().update_editing_history_actions();
}

void ViewImplementation::redo()
{
    client().async_redo(page_id());
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

void ViewImplementation::set_content_blockers(Core::AnonymousBuffer const& patterns)
{
    client().async_set_content_blockers(page_id(), patterns);
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
    did_update_window_rect();
}

void ViewImplementation::alert_closed()
{
    client().async_alert_closed(page_id());
}

void ViewImplementation::confirm_closed(bool accepted)
{
    client().async_confirm_closed(page_id(), accepted);
}

void ViewImplementation::prompt_closed(Optional<Utf16String> const& response)
{
    client().async_prompt_closed(page_id(), response);
}

void ViewImplementation::color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    client().async_color_picker_update(page_id(), picked_color, state);
}

void ViewImplementation::file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files)
{
    client().async_file_picker_closed(page_id(), move(selected_files));
}

void ViewImplementation::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    client().async_select_dropdown_closed(page_id(), selected_item_id);
}

void ViewImplementation::paste_from_clipboard()
{
    client().async_paste_from_clipboard(page_id());
}

void ViewImplementation::set_marked_text_from_input_method(Utf16String const& text)
{
    client().async_set_marked_text_from_input_method(page_id(), text);
}

void ViewImplementation::commit_text_from_input_method(Utf16String const& text, i32 replacement_start, i32 replacement_length)
{
    client().async_commit_text_from_input_method(page_id(), text, replacement_start, replacement_length);
}

void ViewImplementation::unmark_text_from_input_method()
{
    client().async_unmark_text_from_input_method(page_id());
}

Optional<Web::DevicePixelRect> ViewImplementation::get_input_caret_rect()
{
    // Returns the most-recent caret position pushed by WebContent (see set_input_method_state). Deliberately makes no
    // synchronous IPC request: This is read from inside AppKit text-input callbacks, where blocking can re-enter the
    // run loop and deadlock the input method.
    return m_input_method_state.caret_rect;
}

void ViewImplementation::set_input_method_state(Badge<WebContentClient>, InputMethodState state)
{
    m_input_method_state = move(state);

    if (on_input_method_state_change)
        on_input_method_state_change();
}

void ViewImplementation::retrieved_clipboard_entries(u64 request_id, ReadonlySpan<Web::Clipboard::SystemClipboardItem> items)
{
    client().async_retrieved_clipboard_entries(page_id(), request_id, items);
}

void ViewImplementation::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    Application::the().insert_clipboard_item(move(item));
}

Vector<Web::Clipboard::SystemClipboardRepresentation> ViewImplementation::clipboard_entries() const
{
    return Application::the().clipboard_entries();
}

void ViewImplementation::toggle_page_mute_state()
{
    m_mute_state = Web::HTML::invert_mute_state(m_mute_state);
    client().async_set_page_mute_state(page_id(), m_mute_state);
}

void ViewImplementation::did_change_audio_play_state(Badge<WebContentClient>, Web::HTML::AudioPlayState play_state)
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

void ViewImplementation::did_change_screen_wake_lock_state(Badge<WebContentClient>, Web::ScreenWakeLockState wake_lock_state)
{
    if (m_screen_wake_lock_state == wake_lock_state)
        return;

    m_screen_wake_lock_state = wake_lock_state;
    if (on_screen_wake_lock_state_changed)
        on_screen_wake_lock_state_changed(m_screen_wake_lock_state);
}

void ViewImplementation::did_create_top_level_traversable(Badge<WebContentClient>, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    m_top_level_traversable.did_create_top_level_traversable(move(initial_history_entry));
    update_navigation_action_state();
    dump_session_history("created-top-level-traversable"sv);
}

void ViewImplementation::did_change_needs_beforeunload_check(Badge<WebContentClient>, bool needs_beforeunload_check)
{
    m_needs_beforeunload_check = needs_beforeunload_check;
}

void ViewImplementation::did_change_background_color(Badge<WebContentClient>, Gfx::Color color)
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

    client().async_set_zoom_level(m_client_state.page_index, m_zoom_level);
}

String ViewImplementation::current_host() const
{
    if (!m_url.host().has_value())
        return {};
    return m_url.serialized_host();
}

void ViewImplementation::apply_zoom_for_current_host()
{
    auto& settings = Application::settings();
    auto zoom_level = settings.zoom_for_host(current_host()).value_or(settings.default_zoom_level_factor());
    if (zoom_level == m_zoom_level)
        return;
    m_zoom_level = zoom_level;
    update_zoom();
}

void ViewImplementation::handle_resize()
{
    client().async_set_viewport(page_id(), viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
    Application::the().update_compositor_viewport(client().compositor_context_id_for_page(page_id()), viewport_size().to_type<int>(), Web::Compositor::WindowResizingInProgress::Yes);
}

void ViewImplementation::initialize_client(CreateNewClient create_new_client, Optional<Web::HTML::CrossProcessId> initial_document_state_id)
{
    m_needs_beforeunload_check = true;

    if (create_new_client == CreateNewClient::Yes) {
        if (m_history_operation_handling_for_next_client == HistoryOperationHandling::Abandon) {
            // NB: Replies from the previous process will never arrive; complete the in-flight operations so the
            //     traversal queue can serve the new process.
            m_top_level_traversable.abandon_history_operations();
        }

        cancel_all_native_geolocation_requests();

        auto client_handle = m_client_state.client_handle;
        m_client_state = {};
        m_client_state.client_handle = move(client_handle);

        // FIXME: Fail to open the tab, rather than crashing the whole application if this fails.
        auto root_navigable_id = m_history_operation_handling_for_next_client == HistoryOperationHandling::Preserve
            ? Optional<Web::HTML::CrossProcessId> { m_top_level_traversable.id() }
            : Optional<Web::HTML::CrossProcessId> {};
        m_client_state.client = Application::the().launch_web_content_process(*this, root_navigable_id, initial_document_state_id).release_value_but_fixme_should_propagate_errors();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    if (m_client_state.client_handle.is_empty()) {
        m_client_state.client_handle = Web::Crypto::generate_random_uuid();
        Application::the().notify_webdriver_window_created(m_client_state.client_handle);
    }
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);
    client().async_set_zoom_level(m_client_state.page_index, m_zoom_level);
    client().async_set_viewport(m_client_state.page_index, viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
    client().async_set_maximum_frames_per_second(m_client_state.page_index, m_maximum_frames_per_second);
    client().async_set_system_visibility_state(m_client_state.page_index, m_top_level_traversable.system_visibility_state());
    auto compositor_context_id = client().compositor_context_id_for_page(m_client_state.page_index);
    Application::the().update_compositor_viewport(compositor_context_id, viewport_size().to_type<int>());
    client().async_set_document_cookie_version_buffer(m_client_state.page_index, m_document_cookie_version_buffer);

    client().async_set_page_mute_state(m_client_state.page_index, m_mute_state);

    if (Application::browser_options().webdriver_browser_endpoint.has_value())
        Application::the().push_webdriver_session_config(*this);

    Application::the().apply_view_options({}, *this);

    default_zoom_level_factor_changed();
    languages_changed();
    browsing_behavior_changed();
    autoplay_settings_changed();
    global_privacy_control_changed();
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

    auto make_geolocation_success_handler = [this](u64 request_id, bool is_watch) {
        auto weak_this = make_weak_ptr();
        auto request_page_id = page_id();
        auto request_client_handle = m_client_state.client_handle;

        return [weak_this, request_page_id, request_client_handle, request_id, is_watch](Core::GeolocationCoordinates coords) {
            auto* view = weak_this.ptr();
            if (!view || !view->m_client_state.client || view->m_client_state.page_index != request_page_id || view->m_client_state.client_handle != request_client_handle)
                return;

            if (is_watch) {
                if (!view->m_geolocation_watch_ids.contains(request_id))
                    return;
            } else if (!view->m_geolocation_position_request_ids.remove(request_id)) {
                return;
            }

            if (!Application::settings().geolocation_enabled()) {
                if (auto provider_watch_id = view->m_geolocation_watch_ids.take(request_id); provider_watch_id.has_value())
                    Application::the().stop_watching_geolocation_position(*provider_watch_id);
                view->client().async_geolocation_position_response(request_page_id, request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
                return;
            }

            view->client().async_geolocation_position_response(request_page_id, request_id,
                { coords.latitude, coords.longitude, coords.accuracy, coords.altitude, coords.altitude_accuracy, coords.heading, coords.speed }, {});
        };
    };

    auto make_geolocation_error_handler = [this, geolocation_error_code](u64 request_id, bool is_watch) {
        auto weak_this = make_weak_ptr();
        auto request_page_id = page_id();
        auto request_client_handle = m_client_state.client_handle;

        return [weak_this, request_page_id, request_client_handle, request_id, is_watch, geolocation_error_code](Core::GeolocationError error) {
            auto* view = weak_this.ptr();
            if (!view || !view->m_client_state.client || view->m_client_state.page_index != request_page_id || view->m_client_state.client_handle != request_client_handle)
                return;

            if (is_watch) {
                if (!view->m_geolocation_watch_ids.contains(request_id))
                    return;
            } else if (!view->m_geolocation_position_request_ids.remove(request_id)) {
                return;
            }

            auto code = geolocation_error_code(error);
            if (is_watch && code == GeolocationErrorCode::PermissionDenied) {
                auto provider_watch_id = view->m_geolocation_watch_ids.take(request_id);
                if (provider_watch_id.has_value())
                    Application::the().stop_watching_geolocation_position(*provider_watch_id);
            }

            view->client().async_geolocation_position_response(request_page_id, request_id, {}, to_underlying(code));
        };
    };

    on_request_geolocation_position = [this, geolocation_error_code, make_geolocation_success_handler, make_geolocation_error_handler](u64 request_id) {
        if (!Application::settings().geolocation_enabled()) {
            client().async_geolocation_position_response(page_id(), request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
            return;
        }

        if (auto previous_provider_request_id = m_geolocation_position_request_ids.take(request_id); previous_provider_request_id.has_value())
            Application::the().cancel_geolocation_position_request(*previous_provider_request_id);

        auto provider_request_id = Application::the().request_geolocation_position(
            make_geolocation_success_handler(request_id, false),
            make_geolocation_error_handler(request_id, false));
        if (provider_request_id.is_error()) {
            client().async_geolocation_position_response(page_id(), request_id, {}, to_underlying(geolocation_error_code(provider_request_id.error())));
            return;
        }

        m_geolocation_position_request_ids.set(request_id, provider_request_id.release_value());
    };

    on_cancel_geolocation_position_request = [this](u64 request_id) {
        auto provider_request_id = m_geolocation_position_request_ids.take(request_id);
        if (provider_request_id.has_value())
            Application::the().cancel_geolocation_position_request(*provider_request_id);
    };

    on_start_geolocation_position_watch = [this, geolocation_error_code, make_geolocation_success_handler, make_geolocation_error_handler](u64 request_id) {
        if (!Application::settings().geolocation_enabled()) {
            client().async_geolocation_position_response(page_id(), request_id, {}, to_underlying(GeolocationErrorCode::PermissionDenied));
            return;
        }

        if (auto previous_provider_watch_id = m_geolocation_watch_ids.take(request_id); previous_provider_watch_id.has_value())
            Application::the().stop_watching_geolocation_position(*previous_provider_watch_id);

        auto provider_watch_id = Application::the().start_watching_geolocation_position(
            make_geolocation_success_handler(request_id, true),
            make_geolocation_error_handler(request_id, true));

        if (provider_watch_id.is_error()) {
            client().async_geolocation_position_response(page_id(), request_id, {}, to_underlying(geolocation_error_code(provider_watch_id.error())));
            return;
        }

        m_geolocation_watch_ids.set(request_id, provider_watch_id.release_value());
    };

    on_stop_geolocation_position_watch = [this](u64 request_id) {
        auto provider_watch_id = m_geolocation_watch_ids.take(request_id);
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

void ViewImplementation::did_start_navigation(Optional<Utf16String> navigation_id, URL::URL const& url, bool is_redirect)
{
    auto& ongoing = ensure_ongoing_top_level_navigation();
    if (!ongoing.load.has_value())
        ongoing.load = OngoingTopLevelNavigation::Load {};
    ongoing.load->navigation_id = move(navigation_id);
    ongoing.load->url = url;
    ongoing.load->has_started = true;

    set_loading_state(true);
    if (m_should_suppress_history_for_next_load || m_should_suppress_history_for_current_load)
        return;

    if (ongoing.load->navigation_id.has_value() && !ongoing.load->is_uncommitted) {
        ongoing.load->is_uncommitted = true;
        ongoing.load->uses_replacement_process = false;
    }

    auto was_showing_crash_page = exchange(m_is_showing_crash_page, false);
    auto started_from_crash_page = false;
    if (was_showing_crash_page) {
        auto const* current_entry = m_top_level_traversable.session_history().current_entry();
        started_from_crash_page = current_entry && current_entry->url == url;
    }

    auto dump_reason = started_from_crash_page ? "did-start-navigation-from-crash-page"sv
        : is_redirect                          ? "did-start-navigation-redirect"sv
                                               : "did-start-navigation"sv;

    dump_session_history(dump_reason);
}

bool ViewImplementation::did_cancel_navigation(Optional<Utf16String> const& navigation_id)
{
    // A cancel may arrive before a UI-issued load reports its start. A started navigation's cancel must name it.
    auto stale = m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()
        ? m_ongoing_top_level_navigation->load->has_started && navigation_id != m_ongoing_top_level_navigation->load->navigation_id
        : navigation_id.has_value();
    if (stale)
        return false;

    set_loading_state(false);
    if (cancel_uncommitted_top_level_navigation("did-cancel-navigation"sv, false))
        return true;

    clear_ongoing_top_level_navigation_load();
    if (m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->webdriver_completion_source.has_value()) {
        auto webdriver_navigation_id = m_ongoing_top_level_navigation->webdriver_navigation_id;
        complete_webdriver_navigation(webdriver_navigation_id);
        return true;
    }

    dump_session_history("did-cancel-navigation-ignored"sv);
    return true;
}

bool ViewImplementation::matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const
{
    if (!m_ongoing_top_level_navigation.has_value() || !m_ongoing_top_level_navigation->load.has_value())
        return !navigation_id.has_value();
    return m_ongoing_top_level_navigation->load->has_started
        && navigation_id == m_ongoing_top_level_navigation->load->navigation_id;
}

ViewImplementation::OngoingTopLevelNavigation& ViewImplementation::ensure_ongoing_top_level_navigation()
{
    if (!m_ongoing_top_level_navigation.has_value())
        m_ongoing_top_level_navigation = OngoingTopLevelNavigation {};
    return *m_ongoing_top_level_navigation;
}

void ViewImplementation::clear_ongoing_top_level_navigation_load()
{
    if (!m_ongoing_top_level_navigation.has_value())
        return;
    m_ongoing_top_level_navigation->load.clear();
    if (!m_ongoing_top_level_navigation->webdriver_completion_source.has_value())
        m_ongoing_top_level_navigation.clear();
}

void ViewImplementation::clear_ongoing_navigation_webdriver_observation()
{
    if (!m_ongoing_top_level_navigation.has_value())
        return;
    auto& ongoing = *m_ongoing_top_level_navigation;
    ongoing.webdriver_completion_source.clear();
    ongoing.history_operation_id.clear();
    ongoing.expected_url.clear();
    ongoing.history_operation_completed = false;
    ongoing.load_completed = false;
    if (!ongoing.load.has_value())
        m_ongoing_top_level_navigation.clear();
}

void ViewImplementation::did_finish_navigation(URL::URL const& url)
{
    set_loading_state(false);
    clear_ongoing_top_level_navigation_load();

    if (!m_ongoing_top_level_navigation.has_value() || !m_ongoing_top_level_navigation->webdriver_completion_source.has_value())
        return;

    auto navigation_id = m_ongoing_top_level_navigation->webdriver_navigation_id;
    switch (*m_ongoing_top_level_navigation->webdriver_completion_source) {
    case WebDriverNavigationCompletionSource::CrashRecovery:
        if (m_ongoing_top_level_navigation->expected_url.has_value()
            && url != *m_ongoing_top_level_navigation->expected_url)
            break;
        m_ongoing_top_level_navigation->load_completed = true;
        if (m_ongoing_top_level_navigation->history_operation_completed)
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
    if (!has_uncommitted_top_level_navigation())
        return false;

    auto navigation_used_replacement_process = m_ongoing_top_level_navigation->load->uses_replacement_process;
    clear_ongoing_top_level_navigation_load();
    set_loading_state(false);
    if (stop_loading)
        client().async_stop_loading(page_id());

    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    if (!current_entry) {
        if (m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->webdriver_completion_source.has_value())
            complete_webdriver_navigation(m_ongoing_top_level_navigation->webdriver_navigation_id);
        dump_session_history(reason);
        return true;
    }

    set_url(current_entry->url);
    if (navigation_used_replacement_process && reconstruct == ReconstructCanceledNavigation::Yes) {
        reconstruct_current_session_history_entry_with_history_operation(reason);
        return true;
    }

    if (m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->webdriver_completion_source.has_value())
        complete_webdriver_navigation(m_ongoing_top_level_navigation->webdriver_navigation_id);
    dump_session_history(reason);
    return true;
}

void ViewImplementation::apply_webdriver_session_config(WebDriverSessionConfig const& config)
{
    client().async_set_webdriver_session_config(page_id(), config.user_prompt_handler, config.page_load_strategy, config.strict_file_interactability, config.timeouts);
}

void ViewImplementation::run_webdriver_content_command(u64 command_id, String const& name, JsonValue payload, Vector<String> arguments)
{
    if (name == "crash_current_page"sv)
        m_pending_webdriver_crash_command_ids.set(command_id);
    else
        m_pending_webdriver_command_ids.set(command_id);
    client().async_run_webdriver_command(page_id(), command_id, name, move(payload), move(arguments));
}

void ViewImplementation::did_complete_webdriver_content_command(Badge<WebContentClient>, u64 command_id, Web::WebDriver::Response response)
{
    if (m_pending_webdriver_crash_command_ids.contains(command_id)) {
        // WebContent acknowledges the command before its deferred process exit. Keep the command pending until the
        // crash handler has created the replacement process and started session-history recovery.
        if (response.is_error()) {
            m_pending_webdriver_crash_command_ids.remove(command_id);
            Application::the().complete_webdriver_content_command(command_id, move(response));
        }
        return;
    }

    if (m_pending_webdriver_command_ids.remove(command_id))
        Application::the().complete_webdriver_content_command(command_id, move(response));
}

void ViewImplementation::did_close_browsing_context(Badge<WebContentClient>)
{
    auto window_handle = move(m_client_state.client_handle);

    // Headless views retain their closed children. Remove the view from routing immediately so a command racing
    // with the close cannot be sent to a page that no longer exists.
    all_views().remove(m_view_id);
    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);

    if (!window_handle.is_empty())
        Application::the().notify_webdriver_window_closed(window_handle);

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while handling user prompts"sv));

    auto pending_command_ids = move(m_pending_webdriver_command_ids);
    for (auto command_id : pending_command_ids)
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while executing the command"sv));
    auto pending_crash_command_ids = move(m_pending_webdriver_crash_command_ids);
    for (auto command_id : pending_crash_command_ids)
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while executing the command"sv));

    auto pending_navigation_completion_requests = move(m_pending_webdriver_navigation_completion_requests);
    for (auto& request : pending_navigation_completion_requests) {
        if (request.value->timer)
            request.value->timer->stop();
        request.value->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while waiting for navigation"sv));
    }
}

void ViewImplementation::run_webdriver_user_prompt_handling(Function<void(Web::WebDriver::Response)> on_complete)
{
    auto request_id = m_next_webdriver_user_prompt_request_id++;
    m_pending_webdriver_user_prompt_requests.set(request_id, move(on_complete));
    client().async_run_webdriver_user_prompt_handling(page_id(), request_id);
}

void ViewImplementation::did_complete_webdriver_user_prompt_handling(Badge<WebContentClient>, u64 request_id, Web::WebDriver::Response response)
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
    client().async_load_url(page_id(), url, Web::Bindings::NavigationHistoryBehavior::Auto);
}

void ViewImplementation::did_start_webdriver_navigation()
{
    set_loading_state(true);
    auto& ongoing = ensure_ongoing_top_level_navigation();
    if (!ongoing.load.has_value())
        ongoing.load = OngoingTopLevelNavigation::Load {};
    ongoing.load->navigation_id.clear();
    ongoing.load->has_started = false;
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::Load);
}

void ViewImplementation::wait_for_webdriver_navigation_completion(Optional<u64> page_load_timeout, Function<void(Web::WebDriver::Response)> on_complete)
{
    if (!m_ongoing_top_level_navigation.has_value() || !m_ongoing_top_level_navigation->webdriver_completion_source.has_value()) {
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

u64 ViewImplementation::begin_webdriver_navigation(WebDriverNavigationCompletionSource completion_source, Optional<u64> history_operation_id, Optional<URL::URL> expected_url)
{
    auto& ongoing = ensure_ongoing_top_level_navigation();
    ongoing.webdriver_completion_source = completion_source;
    ongoing.webdriver_navigation_id = m_next_webdriver_navigation_id++;
    ongoing.history_operation_id = history_operation_id;
    ongoing.expected_url = move(expected_url);
    ongoing.history_operation_completed = false;
    ongoing.load_completed = false;
    return ongoing.webdriver_navigation_id;
}

void ViewImplementation::complete_webdriver_navigation(u64 navigation_id)
{
    if (!m_ongoing_top_level_navigation.has_value()
        || !m_ongoing_top_level_navigation->webdriver_completion_source.has_value()
        || m_ongoing_top_level_navigation->webdriver_navigation_id != navigation_id)
        return;

    clear_ongoing_navigation_webdriver_observation();

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

void ViewImplementation::complete_webdriver_history_traversal(u64 operation_id)
{
    if (!m_ongoing_top_level_navigation.has_value()
        || m_ongoing_top_level_navigation->webdriver_completion_source != WebDriverNavigationCompletionSource::HistoryTraversal
        || m_ongoing_top_level_navigation->history_operation_id != operation_id) {
        return;
    }

    complete_webdriver_navigation(m_ongoing_top_level_navigation->webdriver_navigation_id);
}

JsonValue ViewImplementation::webdriver_session_history() const
{
    JsonObject serialized;
    serialized.set("currentURL"sv, m_url.serialize());
    serialized.set("webContentProcessID"sv, client().pid());
    serialized.set("backButtonEnabled"sv, m_navigate_back_action->enabled());
    serialized.set("forwardButtonEnabled"sv, m_navigate_forward_action->enabled());
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

void ViewImplementation::recover_current_session_history_entry_with_history_operation(Optional<CanonicalTraversable::HistoryJobEndpoint> crashed_endpoint)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    auto const* current_entry = m_top_level_traversable.session_history().current_entry();
    auto current_url = current_entry ? current_entry->url : m_url;
    set_url(current_url);
    auto navigation_id = begin_webdriver_navigation(WebDriverNavigationCompletionSource::CrashRecovery, {}, current_url);
    m_top_level_traversable.recover_from_web_content_process_crash(move(crashed_endpoint), [this, navigation_id](Web::HTML::HistoryStepResult result, Optional<i32> committed_step) {
        if (result == Web::HTML::HistoryStepResult::Applied) {
            if (committed_step.has_value())
                update_navigation_action_state();
            auto const* current_entry = m_top_level_traversable.session_history().current_entry();
            if (current_entry)
                set_url(current_entry->url);

            if (m_ongoing_top_level_navigation.has_value()
                && m_ongoing_top_level_navigation->webdriver_navigation_id == navigation_id
                && m_ongoing_top_level_navigation->webdriver_completion_source == WebDriverNavigationCompletionSource::CrashRecovery) {
                m_ongoing_top_level_navigation->history_operation_completed = true;
                if (m_ongoing_top_level_navigation->load_completed)
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

    auto tab_id = Application::session_store(is_private()).tab_opened({ .window_id = {}, .initial_url = m_url, .insertion_index = {}, .is_active = SessionStore::IsActive::No });
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

    auto cached_state = Application::session_store(is_private()).cached_tab_state_for_testing(*m_session_tab_id);
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
    Application::session_store(is_private()).update_tab_state(move(update));
}

NonnullRefPtr<Core::Promise<Empty>> ViewImplementation::reset_session_history_for_testing()
{
    m_pending_session_history_reset_for_testing = Core::Promise<Empty>::construct();
    client().async_reset_session_history_for_testing(page_id());
    return *m_pending_session_history_reset_for_testing;
}

void ViewImplementation::request_history_operation(Badge<WebContentClient>, u64 initiation_id, Web::HistoryOperationParameters parameters)
{
    auto reloads_top_level = parameters.visit(
        [this](Web::ReloadHistoryOperationParameters const& parameters) {
            return parameters.navigable_id == m_top_level_traversable.id();
        },
        [](auto const&) { return false; });
    auto finalizes_top_level_cross_document_navigation = parameters.visit(
        [this](Web::PushHistoryOperationParameters const& parameters) {
            return parameters.navigable_id == m_top_level_traversable.id();
        },
        [this](Web::ReplaceHistoryOperationParameters const& parameters) {
            return parameters.navigable_id == m_top_level_traversable.id();
        },
        [](auto const&) { return false; });
    auto requested_operation_completion = [this, reloads_top_level, finalizes_top_level_cross_document_navigation](Web::HTML::HistoryStepResult result, Optional<i32> committed_step) {
        if (reloads_top_level && result != Web::HTML::HistoryStepResult::Applied)
            did_cancel_navigation({});
        if (finalizes_top_level_cross_document_navigation && result == Web::HTML::HistoryStepResult::Applied) {
            if (m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()) {
                m_ongoing_top_level_navigation->load->is_uncommitted = false;
                m_ongoing_top_level_navigation->load->uses_replacement_process = false;
            }
            if (auto const* current_entry = m_top_level_traversable.session_history().current_entry())
                set_url(current_entry->url);
        }
        if (committed_step.has_value())
            update_navigation_action_state();
        dump_session_history("requested-history-operation-complete"sv);
    };

    parameters.visit(
        [&](Web::TraverseByDeltaHistoryOperationParameters& parameters) {
            // The traversal target is resolved once the queue reaches these steps, so navigations queued ahead of the
            // traversal are part of the session history it resolves against.
            m_top_level_traversable.append_history_queue_steps(
                [this, initiation_id, parameters = move(parameters)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
                    start_requested_history_traversal(initiation_id, move(parameters), move(promise));
                });
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters& parameters) {
            // As with delta traversal, resolve the key only after earlier queued operations have completed.
            m_top_level_traversable.append_history_queue_steps(
                [this, initiation_id, parameters = move(parameters)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
                    start_requested_history_traversal(initiation_id, move(parameters), move(promise));
                });
        },
        [&](auto&) {
            m_top_level_traversable.enqueue_history_operation(initiation_id, move(parameters), client(), page_id(), move(requested_operation_completion));
        });
}

void ViewImplementation::start_requested_history_traversal(u64 initiation_id, Web::TraverseByDeltaHistoryOperationParameters parameters, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto target = m_top_level_traversable.session_history().traversal_target_for_delta(parameters.delta);
    if (!target.has_value()) {
        client().async_complete_history_operation(
            page_id(), 0, Web::HTML::HistoryStepResult::Applied, {},
            m_top_level_traversable.session_history().size(), initiation_id);
        promise->resolve({});
        return;
    }
    start_requested_history_traversal(initiation_id, move(parameters), target.release_value(), move(promise));
}

void ViewImplementation::start_requested_history_traversal(u64 initiation_id, Web::NavigationAPITraverseHistoryOperationParameters parameters, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto navigable = m_top_level_traversable.find(parameters.navigable_id);
    Optional<i32> target_step;
    if (navigable.has_value())
        target_step = m_top_level_traversable.navigation_api_traversal_target(*navigable, parameters.key);
    auto target = target_step.has_value()
        ? m_top_level_traversable.session_history().traversal_target_for_step(*target_step)
        : Optional<TraversableSessionHistory::TraversalTarget> {};
    if (!target.has_value()) {
        client().async_complete_history_operation(page_id(), 0, Web::HTML::HistoryStepResult::NoMatchingEntry, {}, m_top_level_traversable.session_history().size(), initiation_id);
        promise->resolve({});
        return;
    }
    start_requested_history_traversal(initiation_id, move(parameters), target.release_value(), move(promise));
}

void ViewImplementation::start_requested_history_traversal(u64 initiation_id, Web::HistoryOperationParameters parameters, TraversableSessionHistory::TraversalTarget target, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    m_top_level_traversable.run_history_operation_at_queue_position(
        initiation_id,
        move(parameters),
        client(),
        page_id(),
        target.target_step,
        [this](Web::HTML::HistoryStepResult, Optional<i32> committed_step) {
            if (committed_step.has_value())
                update_navigation_action_state();
            dump_session_history("requested-history-traversal-complete"sv);
        },
        move(promise));
}

void ViewImplementation::did_receive_history_operation_ready(Badge<WebContentClient>, u64 operation_id, Web::HistoryOperationReadyResult result)
{
    m_top_level_traversable.did_receive_history_operation_ready(operation_id, move(result));
}

void ViewImplementation::did_receive_history_step_unload_cancelation_result(Badge<WebContentClient>, u64 operation_id, Web::HTML::HistoryStepResult result)
{
    m_top_level_traversable.did_receive_history_step_unload_cancelation_result(operation_id, result);
}

void ViewImplementation::did_receive_changing_navigable_history_job_ready(Badge<WebContentClient>, WebContentClient& source_client, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    m_top_level_traversable.did_receive_changing_navigable_history_job_ready(source_client, source_page_id, operation_id, navigable_id, disposition);
}

void ViewImplementation::did_receive_changing_navigable_continuation_applied(Badge<WebContentClient>, WebContentClient& source_client, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    m_top_level_traversable.did_receive_changing_navigable_continuation_applied(source_client, source_page_id, operation_id, navigable_id, move(activated_navigable_state), move(previous_entry_persisted_state));
}

void ViewImplementation::did_receive_nonchanging_navigable_history_state_updated(Badge<WebContentClient>, WebContentClient& source_client, u64 source_page_id, u64 operation_id, Web::HTML::CrossProcessId navigable_id)
{
    m_top_level_traversable.did_receive_nonchanging_navigable_history_state_updated(source_client, source_page_id, operation_id, navigable_id);
}

void ViewImplementation::did_reset_session_history_for_testing(
    Badge<WebContentClient>, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    auto promise = move(m_pending_session_history_reset_for_testing);
    m_top_level_traversable.reset_session_history_for_testing(move(active_entry));
    clear_ongoing_navigation_webdriver_observation();
    if (m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()) {
        m_ongoing_top_level_navigation->load->is_uncommitted = false;
        m_ongoing_top_level_navigation->load->uses_replacement_process = false;
    }
    update_navigation_action_state();

    if (promise)
        promise->resolve({});
}

void ViewImplementation::dump_session_history(StringView reason, SessionHistoryDumpMode mode) const
{
    if (mode == SessionHistoryDumpMode::IfDebuggingEnabled && !history_debug_enabled())
        return;

    auto traversal = m_top_level_traversable.browser_history_traversal_for_testing();

    dbgln("[History] UI session history page={} pid={} reason={} url='{}' uncommitted_navigation={} loading_url={} pending_traversal_target={} pending_traversal_stage={} back={} forward={} entries={}",
        page_id(),
        client().pid(),
        reason,
        m_url,
        has_uncommitted_top_level_navigation(),
        m_ongoing_top_level_navigation.has_value() && m_ongoing_top_level_navigation->load.has_value()
            ? m_ongoing_top_level_navigation->load->url
            : Optional<URL::URL> {},
        traversal.has_value() ? Optional<i32> { traversal->target_step } : Optional<i32> {},
        traversal.has_value() ? CanonicalTraversable::browser_history_traversal_stage_to_string(traversal->stage) : "none"sv,
        m_navigate_back_action->enabled(),
        m_navigate_forward_action->enabled(),
        history_log_entries(m_top_level_traversable.session_history()));
}

void ViewImplementation::handle_web_content_process_crash(LoadErrorPage load_error_page)
{
    set_loading_state(false);
    clear_ongoing_top_level_navigation_load();

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while handling user prompts"sv));

    auto pending_command_ids = move(m_pending_webdriver_command_ids);
    auto pending_crash_command_ids = move(m_pending_webdriver_crash_command_ids);
    for (auto command_id : pending_command_ids)
        Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while executing the command"sv));

    auto const headless_mode = Application::browser_options().headless_mode.has_value();

    if (!headless_mode) {
        dbgln("\033[31;1mWebContent process crashed!\033[0m Last page loaded: {}", m_url);
        dbgln("Consider raising an issue at https://github.com/LadybirdBrowser/ladybird/issues/new/choose");
    }

    ++m_crash_count;
    reset_page_media_state();

    constexpr size_t max_reasonable_crash_count = 5U;
    if (m_crash_count >= max_reasonable_crash_count) {
        if (!headless_mode) {
            dbgln("WebContent has crashed {} times in quick succession! Not restarting...", m_crash_count);
            m_repeated_crash_timer->stop();
            for (auto command_id : pending_crash_command_ids)
                Application::the().complete_webdriver_content_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed repeatedly and was not restarted"sv));
            return;
        }
        // In headless mode, always respawn - tests need a working WebContent for each test.
        // Reset the crash count so we can continue running tests.
        m_crash_count = 0;
    }
    m_repeated_crash_timer->restart();

    // In headless mode, respawn WebContent but skip the error page.
    if (headless_mode)
        load_error_page = LoadErrorPage::No;

    auto crashed_endpoint = CanonicalTraversable::HistoryJobEndpoint {
        m_client_state.client,
        page_id(),
    };

    m_history_operation_handling_for_next_client = HistoryOperationHandling::Preserve;
    Optional<Web::HTML::CrossProcessId> initial_document_state_id;
    Optional<URL::URL> process_site_url;
    if (auto const* target_entry = m_top_level_traversable.ongoing_browser_history_traversal_target_entry()) {
        initial_document_state_id = target_entry->document_state.id;
        process_site_url = target_entry->url;
    }
    if (!initial_document_state_id.has_value()) {
        if (auto const* current_entry = m_top_level_traversable.session_history().current_entry()) {
            initial_document_state_id = current_entry->document_state.id;
            process_site_url = current_entry->url;
        }
    }
    initialize_client(CreateNewClient::Yes, initial_document_state_id);
    m_client_state.site_url = move(process_site_url);
    m_history_operation_handling_for_next_client = HistoryOperationHandling::Abandon;
    VERIFY(m_client_state.client);

    // Don't keep a stale backup bitmap around.
    m_backup_shared_image_buffer = nullptr;

    handle_resize();

    if (load_error_page == LoadErrorPage::Yes) {
        m_top_level_traversable.abandon_after_web_content_process_crash();
        auto escaped_url = escape_html_entities(m_url.serialize());

        StringBuilder builder;
        builder.appendff(ERROR_HTML_HEADER, NO_FALLBACK_FAVICON_LINK, CRASH_ERROR_SVG, "Ladybird flew off-course!"sv);
        builder.appendff("<p>The web page <a href=\"{}\">{}</a> has crashed.<br><br>You can reload the page to try again.</p>", escaped_url, escaped_url);
        builder.append(ERROR_HTML_FOOTER);
        load_crash_page_html(builder.string_view(), m_url);
    } else {
        m_should_suppress_history_for_current_load = false;
        m_should_suppress_history_for_next_load = false;
        recover_current_session_history_entry_with_history_operation(move(crashed_endpoint));
    }

    for (auto command_id : pending_crash_command_ids)
        Application::the().complete_webdriver_content_command(command_id, JsonValue {});
}

void ViewImplementation::default_zoom_level_factor_changed()
{
    apply_zoom_for_current_host();
}

void ViewImplementation::zoom_per_host_changed(StringView host)
{
    if (current_host() != host)
        return;
    apply_zoom_for_current_host();
}

void ViewImplementation::languages_changed()
{
    auto const& languages = Application::settings().languages();
    client().async_set_preferred_languages(page_id(), languages);
}

void ViewImplementation::browsing_behavior_changed()
{
    auto const& browsing_behavior = Application::settings().browsing_behavior();
    client().async_set_browsing_behavior(page_id(), browsing_behavior);
}

void ViewImplementation::autoplay_settings_changed()
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

    client().async_set_autoplay_settings(page_id(), policy, move(allowlist));
}

void ViewImplementation::global_privacy_control_changed()
{
    auto global_privacy_control = Application::settings().global_privacy_control();
    client().async_set_enable_global_privacy_control(page_id(), global_privacy_control == GlobalPrivacyControl::Yes);
}

void ViewImplementation::geolocation_settings_changed()
{
    using ErrorCode = Web::Geolocation::GeolocationPositionError::ErrorCode;

    auto cancel_native_geolocation_requests = [this] {
        auto geolocation_position_request_ids = move(m_geolocation_position_request_ids);
        for (auto const& request : geolocation_position_request_ids) {
            Application::the().cancel_geolocation_position_request(request.value);
            client().async_geolocation_position_response(page_id(), request.key, {}, to_underlying(ErrorCode::PermissionDenied));
        }

        auto geolocation_watch_ids = move(m_geolocation_watch_ids);
        for (auto const& watch : geolocation_watch_ids) {
            Application::the().stop_watching_geolocation_position(watch.value);
            client().async_geolocation_position_response(page_id(), watch.key, {}, to_underlying(ErrorCode::PermissionDenied));
        }
    };

    if (Application::web_content_options().is_test_mode == IsTestMode::Yes) {
        client().async_set_geolocation_emulated_position(page_id(), { 37.7647658, -122.4345892, 100.0, 0.0, 0.0, 0.0, 0.0 }, {});
        return;
    }

    if (Application::settings().geolocation_enabled()) {
        client().async_set_geolocation_emulated_position(page_id(), {}, {});
    } else {
        cancel_native_geolocation_requests();
        client().async_set_geolocation_emulated_position(page_id(), {}, to_underlying(ErrorCode::PermissionDenied));
    }
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

void ViewImplementation::did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const& screenshot)
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

void ViewImplementation::did_receive_internal_page_info(Badge<WebContentClient>, PageInfoType, Optional<Core::AnonymousBuffer> const& info)
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
    client().async_set_user_style(page_id(), source);
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
        Application::the().insert_clipboard_entry({ url_text_to_copy(m_context_menu_url), "text/plain"_string });
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

        Application::the().insert_clipboard_entry({ ByteString { encoded.value().bytes() }, "image/png"_string });
    });

    m_open_audio_action = Action::create("Open Audio"sv, ActionID::OpenAudio, [this]() {
        load(m_context_menu_url);
    });
    m_open_video_action = Action::create("Open Video"sv, ActionID::OpenVideo, [this]() {
        load(m_context_menu_url);
    });
    m_media_play_action = Action::create("Play"sv, ActionID::PlayMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_pause_action = Action::create("Pause"sv, ActionID::PauseMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_mute_action = Action::create("Mute"sv, ActionID::MuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_unmute_action = Action::create("Unmute"sv, ActionID::UnmuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_show_controls_action = Action::create("Show Controls"sv, ActionID::ShowControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_hide_controls_action = Action::create("Hide Controls"sv, ActionID::HideControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_loop_action = Action::create_checkable("Loop"sv, ActionID::ToggleMediaLoopState, [this]() {
        client().async_toggle_media_loop_state(page_id());
    });
    m_media_enter_fullscreen_action = Action::create("Full Screen"sv, ActionID::EnterFullscreen, [this]() {
        client().async_toggle_media_fullscreen_state(page_id());
    });
    m_media_exit_fullscreen_action = Action::create("Exit Full Screen"sv, ActionID::ExitFullscreen, [this]() {
        client().async_toggle_media_fullscreen_state(page_id());
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
                Application::bookmark_store().add_bookmark(move(result.bookmark.url), move(result.bookmark.title), move(result.bookmark.favicon_base64_png), result.target_folder_id);
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

        application.insert_clipboard_entry({ url_text_to_copy(bookmark->bookmark().url), "text/plain"_string });
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

void ViewImplementation::did_request_page_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target)
{
    auto& cut_selection_action = Application::the().cut_selection_action();
    cut_selection_action.set_visible(for_input_events_target == Web::ContextMenuForInputEventsTarget::Yes);

    auto const& search_engine = Application::settings().search_engine();
    auto lookup = on_request_dictionary_lookup ? selected_text_for_dictionary_lookup() : OptionalNone {};
    auto selected_text = lookup.map([](auto const& lookup) { return lookup.text; });
    if (!selected_text.has_value())
        selected_text = selected_text_with_whitespace_collapsed();
    m_search_text = search_engine.has_value() ? selected_text : OptionalNone {};
    auto selected_text_url = selected_text.has_value() ? url_from_text(*selected_text) : OptionalNone {};
    update_look_up_selected_text_action(lookup, content_position);

    ScopeGuard guard { [&]() {
        cut_selection_action.set_visible(true);
        m_search_text.clear();
    } };

    if (m_search_text.has_value()) {
        m_search_selected_text_action->set_text(search_engine->format_search_query_for_display(*m_search_text));
        m_search_selected_text_action->set_visible(true);
    } else {
        m_search_selected_text_action->set_visible(false);
    }

    if (selected_text_url.has_value() && m_selected_text_link_context_menu->on_activation) {
        m_context_menu_url = selected_text_url.release_value();
        m_open_in_new_tab_action->set_text("Open in New Tab"sv);
        m_selected_text_link_context_menu->on_activation(to_widget_position(content_position));
        return;
    }

    if (m_page_context_menu->on_activation)
        m_page_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_link_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url)
{
    m_context_menu_url = move(url);
    update_look_up_selected_text_action(on_request_dictionary_lookup ? selected_text_for_dictionary_lookup() : OptionalNone {}, content_position);

    m_open_in_new_tab_action->set_text("Open in New Tab"sv);

    switch (url_type(m_context_menu_url)) {
    case URLType::Email:
        m_copy_url_action->set_text("Copy Email Address"sv);
        break;
    case URLType::Telephone:
        m_copy_url_action->set_text("Copy Phone Number"sv);
        break;
    case URLType::Other:
        m_copy_url_action->set_text("Copy Link Address"sv);
        break;
    }

    if (m_link_context_menu->on_activation)
        m_link_context_menu->on_activation(to_widget_position(content_position));
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

void ViewImplementation::did_request_image_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url, Optional<Gfx::ShareableBitmap> bitmap)
{
    m_context_menu_url = move(url);
    m_image_context_menu_bitmap = move(bitmap);
    update_look_up_selected_text_action(on_request_dictionary_lookup ? selected_text_for_dictionary_lookup() : OptionalNone {}, content_position);

    m_open_in_new_tab_action->set_text("Open Image in New Tab"sv);
    m_copy_url_action->set_text("Copy Image URL"sv);

    m_copy_image_action->set_enabled(m_image_context_menu_bitmap.has_value());

    if (m_image_context_menu->on_activation)
        m_image_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_media_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::Page::MediaContextMenu menu)
{
    m_context_menu_url = move(menu.media_url);
    update_look_up_selected_text_action(on_request_dictionary_lookup ? selected_text_for_dictionary_lookup() : OptionalNone {}, content_position);

    m_open_in_new_tab_action->set_text(menu.is_video ? "Open Video in New Tab"sv : "Open Audio in new Tab"sv);
    m_copy_url_action->set_text(menu.is_video ? "Copy Video URL"sv : "Copy Audio URL"sv);

    m_open_audio_action->set_visible(!menu.is_video);
    m_open_video_action->set_visible(menu.is_video);

    m_media_play_action->set_visible(!menu.is_playing);
    m_media_pause_action->set_visible(menu.is_playing);

    m_media_mute_action->set_visible(!menu.is_muted);
    m_media_unmute_action->set_visible(menu.is_muted);

    m_media_show_controls_action->set_visible(!menu.has_user_agent_controls);
    m_media_hide_controls_action->set_visible(menu.has_user_agent_controls);

    m_media_loop_action->set_checked(menu.is_looping);

    m_media_enter_fullscreen_action->set_visible(menu.is_video && !menu.is_fullscreen);
    m_media_exit_fullscreen_action->set_visible(menu.is_video && menu.is_fullscreen);

    if (m_media_context_menu->on_activation)
        m_media_context_menu->on_activation(to_widget_position(content_position));
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
    if (needs_beforeunload_check()) {
        client().async_request_close(page_id());
        return;
    }

    client().request_close(page_id());
}

Function<void()> ViewImplementation::prepare_for_immediate_close()
{
    VERIFY(!needs_beforeunload_check());

    auto client = m_client_state.client;
    auto page_id = m_client_state.page_index;
    client->prepare_for_detached_close(page_id);
    return [client = move(client), page_id] {
        client->async_request_close(page_id);
    };
}

}
