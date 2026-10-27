/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCompositing/PausedDebuggerOverlay.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/CSS/SystemColor.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>
#include <LibWebCommon/Infra/Strings.h>
#include <LibWebCommon/Loader/ContentBlocking.h>
#include <LibWebCommon/WebDriver/Error.h>
#include <LibWebCommon/WebDriver/Input.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/CrashReportReview.h>
#include <LibWebView/ErrorHTML.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Menu.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebDriverBiDi.h>
#include <math.h>

namespace WebView {

static HashMap<u64, ViewImplementation*>& all_views()
{
    static NeverDestroyed<HashMap<u64, ViewImplementation*>> views;
    return *views;
}

// NB: WebDriver BiDi names the navigable that is gone a browsing context, whose error code is no such frame.
static Web::WebDriver::ErrorCode error_code_for_a_navigable_that_is_gone(auto protocol)
{
    return protocol == decltype(protocol)::BiDi ? Web::WebDriver::ErrorCode::NoSuchFrame : Web::WebDriver::ErrorCode::NoSuchWindow;
}

static void fail_webdriver_content_commands_after_window_close(auto const& commands)
{
    for (auto const& command : commands)
        Application::the().complete_webdriver_content_command(command.key, Web::WebDriver::Error::from_code(error_code_for_a_navigable_that_is_gone(command.value.protocol), "Window closed while executing the command"sv));
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

ViewImplementation::ViewImplementation(IsPrivate is_private)
    : m_is_private(is_private)
    , m_session(Application::session_for_new_view(is_private))
    , m_document_cookie_version_buffer(Core::create_shared_version_buffer())
    , m_view_id(s_view_count++)
{
    all_views().set(m_view_id, this);

    initialize_context_menus();

    m_repeated_crash_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1000), [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    });
}

ViewImplementation::~ViewImplementation()
{
    // The frontend may be torn down already, so clearing the ongoing navigation must not report a URL change to it.
    on_url_change = nullptr;
    TabPerformanceMonitor::forget_view(view_id());
    if (m_top_level_traversable)
        m_top_level_traversable->clear_ongoing_navigation();
    cancel_all_native_geolocation_requests();

    if (!m_window_handle.is_empty() && !m_owner_view_id.has_value())
        Application::the().notify_webdriver_window_closed(m_window_handle);

    reset_spell_checking();
    all_views().remove(m_view_id);

    // NB: WebDriver BiDi may activate another top-level traversable, which must not be this one that is going away.
    if (!m_window_handle.is_empty() && !m_owner_view_id.has_value()) {
        if (auto* webdriver_bidi = WebDriverBiDi::the())
            webdriver_bidi->top_level_traversable_did_close(m_window_handle);
    }

    if (m_top_level_traversable) {
        m_top_level_traversable->discard_pending_host();
        m_top_level_traversable->discard_representing_pages();
        if (page().is_open())
            page().client().close_page_of_closed_tab(page().id());
    }

    // A headless parent can own and destroy its child view without the child receiving a browsing-context-close
    // notification. Do not strand a WebDriver command which raced with that teardown.
    discard_webdriver_input_commands();
    fail_webdriver_content_commands_after_window_close(m_pending_webdriver_commands);
    fail_webdriver_content_commands_after_window_close(m_pending_webdriver_crash_commands);
    fail_webdriver_content_commands_after_window_close(m_webdriver_commands_waiting_for_a_document);

    if (m_top_level_traversable)
        CanonicalTraversable::remove_from_user_agent_top_level_traversable_set(*m_top_level_traversable);
}

pid_t ViewImplementation::web_content_pid() const
{
    return page().client().pid();
}

Vector<HistoryEntry> ViewImplementation::recent_history_entries(size_t limit) const
{
    return m_session->history_store->list_entries({}, 0, limit);
}

CanonicalTraversable& ViewImplementation::traversable() const
{
    VERIFY(m_top_level_traversable);
    return *m_top_level_traversable;
}

void ViewImplementation::display_traversable(CanonicalTraversable& traversable, Web::HTML::VisibilityState system_visibility_state)
{
    VERIFY(!m_top_level_traversable);
    m_top_level_traversable = &traversable;
    traversable.set_view({}, *this);
    traversable.set_system_visibility_state(system_visibility_state);
    traversable.on_session_history_changed = [this] {
        notify_session_history_changed();
    };
    notify_session_history_changed();
    update_navigation_action_state();
}

WebContentPage& ViewImplementation::page() const
{
    auto page = traversable().display_page();
    VERIFY(page);
    return *page;
}

URL::URL ViewImplementation::url() const
{
    if (!m_top_level_traversable)
        return URL::about_blank();
    return m_top_level_traversable->active_document_url();
}

void ViewImplementation::did_update_url()
{
    update_bookmark_action();
    update_report_website_issue_action();
    if (auto page = traversable().display_page(); page && page->routed_connection())
        page->routed_connection()->update_audio_capture_permission();
    update_displayed_url();
}

Optional<URL::URL const&> ViewImplementation::browser_ui_load_url() const
{
    if (!m_browser_ui_load.has_value() || !m_top_level_traversable)
        return {};
    auto ongoing_navigation_id = m_top_level_traversable->ongoing_navigation_id();
    if (!ongoing_navigation_id.has_value() || *ongoing_navigation_id != m_browser_ui_load->navigation_id)
        return {};
    return m_browser_ui_load->url;
}

void ViewImplementation::did_change_ongoing_navigation(Badge<CanonicalNavigable>)
{
    update_displayed_url();
}

void ViewImplementation::did_change_active_document(Badge<CanonicalTraversable>)
{
    set_title(Utf16String::from_utf8(url().serialize()));
    set_favicon({});
    did_update_url();
}

void ViewImplementation::update_displayed_url()
{
    auto displayed_url = url();
    if (auto browser_ui_load_url = this->browser_ui_load_url(); browser_ui_load_url.has_value())
        displayed_url = *browser_ui_load_url;
    auto displayed_url_without_fragment_directive = displayed_url;
    if (auto const& raw_fragment = displayed_url_without_fragment_directive.fragment(); raw_fragment.has_value()) {
        if (auto position = raw_fragment->find(":~:"sv); position.has_value())
            displayed_url_without_fragment_directive.set_fragment(raw_fragment->substring_view(0, *position));
    }
    if (!m_text_fragment_indication_is_visible)
        displayed_url = displayed_url_without_fragment_directive;

    if (m_top_level_traversable) {
        auto const& current_entry = traversable().session_history().current_entry();
        if (current_entry.url == displayed_url_without_fragment_directive)
            displayed_url = display_url_for_session_history_entry(current_entry.descriptor(), m_text_fragment_indication_is_visible);
    }

    if (m_displayed_url == displayed_url)
        return;
    m_displayed_url = move(displayed_url);
    if (on_url_change)
        on_url_change(m_displayed_url);
}

void ViewImplementation::set_text_fragment_indication_visibility(Badge<WebContentPage>, bool visible)
{
    if (m_text_fragment_indication_is_visible == visible)
        return;
    m_text_fragment_indication_is_visible = visible;
    update_displayed_url();
}

void ViewImplementation::set_title(Utf16String title)
{
    if (m_title == title)
        return;

    m_title = move(title);

    if (on_title_change)
        on_title_change(m_title);
}

void ViewImplementation::set_favicon(Optional<Gfx::Bitmap const&> favicon)
{
    m_favicon_hash.clear();

    if (favicon.has_value()) {
        if (auto favicon_png = Gfx::PNGWriter::encode(*favicon); !favicon_png.is_error())
            m_favicon_hash = m_session->favicon_store->add_favicon(favicon_png.release_value());

        if (m_favicon_hash.has_value()) {
            if (m_is_private == IsPrivate::No)
                Application::bookmark_store().update_favicon(url(), *m_favicon_hash);
            m_session->history_store->update_favicon(url(), *m_favicon_hash);
        }
    }

    if (on_favicon_change)
        on_favicon_change(favicon);
}

// The tab is displayed in a process of its own, whose page starts from a document standing in for the entry of the
// document state given. The outgoing process keeps displaying the tab's document until the UI process has unloaded
// it there, before a document activates in the new page.
void ViewImplementation::server_did_paint(Badge<WebContentPage>, i32 bitmap_id, Gfx::IntSize size, Gfx::IntRect damage_rect, double device_pixel_ratio)
{
    bool did_swap_bitmap = false;
    auto previous_front_bitmap_id = m_front_bitmap.id;
    auto bitmap_index = m_other_bitmaps.find_first_index_if([bitmap_id](auto const& bitmap) { return bitmap_id == bitmap.id; });
    if (bitmap_index.has_value()) {
        m_has_usable_bitmap = true;
        m_other_bitmaps[*bitmap_index].last_painted_size = size.to_type<Web::DevicePixels>();
        m_other_bitmaps[*bitmap_index].last_painted_device_pixel_ratio = device_pixel_ratio;
        swap(m_other_bitmaps[*bitmap_index], m_front_bitmap);
        m_backup_shared_image_buffer = nullptr;
        did_swap_bitmap = true;
    }

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received presented bitmap {} for page {} size={}x{} did_swap={} front={}",
        bitmap_id, page().id(), size.width(), size.height(), did_swap_bitmap, m_front_bitmap.id);

    release_backing_store(did_swap_bitmap ? previous_front_bitmap_id : bitmap_id);

    if (did_swap_bitmap)
        did_accept_presented_backing_store(bitmap_id, damage_rect);
    if (did_swap_bitmap && m_crash_state.has_value() && m_crash_state->recovery_started)
        set_crash_state({});
    if (did_swap_bitmap)
        TabPerformanceMonitor::did_present(view_id());

    if (did_swap_bitmap && on_ready_to_paint)
        on_ready_to_paint();
}

void ViewImplementation::release_backing_store(i32 bitmap_id)
{
    page().release_presented_bitmap(bitmap_id);
}

void ViewImplementation::set_window_position(Gfx::IntPoint position)
{
    m_window_position = position;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_window_position(position.to_type<Web::DevicePixels>());
    });
}

void ViewImplementation::set_window_size(Gfx::IntSize size)
{
    m_window_size = size;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_window_size(size.to_type<Web::DevicePixels>());
    });
}

void ViewImplementation::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    if (traversable().system_visibility_state() == visibility_state)
        return;

    traversable().set_system_visibility_state(visibility_state);
    Application::the().update_compositor_context_visibility(page().compositor_context_id(), visibility_state);
}

void ViewImplementation::set_has_system_focus(bool has_system_focus)
{
    traversable().set_has_system_focus(has_system_focus, {});
}

void ViewImplementation::load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    load(traversable().prepare_navigation(url, {}, history_handling));
}

// A navigation a page asked the browser's UI to start for it — a link opened with a middle-click, e.g. — whose
// source is still that page.
void ViewImplementation::load(Web::HTML::PreparedNavigationDescriptor navigation)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash(NavigationToRetry { navigation.url, prepare_navigation_to_retry(navigation) });
    m_last_stopped_navigation.clear();
    if (navigation.url.scheme() != "javascript"sv) {
        if (navigation.url.fragment().has_value() && navigation.url.fragment()->contains(":~:"sv))
            m_text_fragment_indication_is_visible = true;
        m_browser_ui_load = BrowserUILoad { navigation.navigation_id, navigation.url };
    }
    traversable().begin_navigation(move(navigation));
    if (traversable().has_uncommitted_navigation())
        set_loading_state(true);
    dump_session_history("load"sv);
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
    load_from_user_input(input, sanitize_url(input, Application::settings().search_engine_settings().engine));
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

    if (auto* view = create_view_for_new_tab_or_window(is_private())) {
        view->load(url);
        return;
    }

    Application::the().open_url_in_new_tab(url, activate_tab);
}

void ViewImplementation::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab)
{
    if (!is_url_handled_internally(navigation.url)) {
        handle_external_url_from_user_input(navigation.url);
        return;
    }

    if (auto* view = create_view_for_new_tab_or_window(is_private())) {
        view->load(move(navigation));
        return;
    }

    Application::the().open_navigation_in_new_tab(move(navigation), activate_tab);
}

void ViewImplementation::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, IsPrivate is_private)
{
    if (!is_url_handled_internally(navigation.url)) {
        handle_external_url_from_user_input(navigation.url);
        return;
    }

    if (auto* view = create_view_for_new_tab_or_window(is_private)) {
        view->load(move(navigation));
        return;
    }

    Application::the().open_navigation_in_new_window(move(navigation), is_private);
}

void ViewImplementation::open_url_in_new_window(URL::URL const& url, IsPrivate is_private)
{
    if (!is_url_handled_internally(url)) {
        handle_external_url_from_user_input(url);
        return;
    }

    if (auto* view = create_view_for_new_tab_or_window(is_private)) {
        view->load(url);
        return;
    }

    Application::the().open_url_in_new_window(url, is_private);
}

void ViewImplementation::load_html(StringView html)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    m_last_stopped_navigation.clear();
    traversable().navigate(URL::about_srcdoc(), Utf16String::from_utf8(html));
    if (traversable().has_uncommitted_navigation())
        set_loading_state(true);
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

void ViewImplementation::reload(BypassCache bypass_cache)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Reload;

    // A load stopped before its document was activated is loaded again, rather than the document it was to replace.
    if (m_last_stopped_navigation.has_value()) {
        retry_navigation(m_last_stopped_navigation.release_value(), Web::Bindings::NavigationHistoryBehavior::Replace);
        return;
    }

    if (m_crash_state.has_value() && m_crash_state->navigation_to_retry.has_value()) {
        retry_navigation(m_crash_state->navigation_to_retry.release_value(), Web::Bindings::NavigationHistoryBehavior::Auto);
        return;
    }

    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    set_loading_state(true);
    if (m_crash_state.has_value()) {
        prepare_for_navigation_after_crash();
        recover_current_session_history_entry_with_history_operation();
        return;
    }

    traversable().reload(bypass_cache);
    update_navigation_action_state();
    dump_session_history("reload-mark-current-entry-reload-pending"sv);
}

void ViewImplementation::reload_for_webdriver_bidi(BypassCache bypass_cache, Utf16String navigation_id)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Reload;
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    // NB: BiDi reloads the active document, rather than retrying a stopped or failed navigation.
    m_last_stopped_navigation.clear();
    prepare_for_navigation_after_crash();
    set_loading_state(true);
    traversable().reload(bypass_cache, move(navigation_id));
    update_navigation_action_state();
    dump_session_history("reload-mark-current-entry-reload-pending"sv);
}

void ViewImplementation::stop_loading()
{
    if (!m_is_loading)
        return;
    // Only a stopped navigation that never activated its document needs reissuing on reload; a stopped
    // active-document load reloads through the session history. A navigation a history operation is finalizing is
    // not stopped, and activates its document.
    m_last_stopped_navigation = traversable().has_uncommitted_navigation() ? navigation_to_retry_for_ongoing_navigation() : Optional<NavigationToRetry> {};
    traversable().stop_loading();
}

void ViewImplementation::did_stop_loading(Badge<CanonicalTraversable>, bool stopped_navigation)
{
    set_loading_state(false);
    if (!stopped_navigation) {
        m_last_stopped_navigation.clear();
        return;
    }
    did_end_uncommitted_navigation("stop-loading"sv);
}

// The address shown is the active entry's again, and a navigation WebDriver waits on is over.
void ViewImplementation::did_end_uncommitted_navigation(StringView reason)
{
    update_displayed_url();
    if (m_webdriver_navigation_observation.has_value())
        complete_webdriver_navigation();
    dump_session_history(reason);
}

void ViewImplementation::traverse_the_history_by_delta(
    int delta,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    traversable().traverse_the_history_by_delta(delta, check_for_cancelation, move(on_ready));
}

void ViewImplementation::traverse_the_history_to_step(
    i32 step,
    CheckForCancelation check_for_cancelation,
    Function<void()> on_ready)
{
    if (on_before_browser_initiated_navigation)
        on_before_browser_initiated_navigation();

    prepare_for_navigation_after_crash();
    traversable().traverse_the_history_to_step(step, check_for_cancelation, move(on_ready));
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

void ViewImplementation::did_apply_top_level_history_traversal_step(Web::HTML::CrossProcessId operation_id)
{
    complete_webdriver_history_traversal(operation_id);
}

// A reload of the tab's document that did not apply leaves the tab loading only if something else is.
void ViewImplementation::did_not_apply_reload()
{
    if (traversable().tracked_load_navigation_id().has_value())
        return;
    set_loading_state(false);
    if (m_webdriver_navigation_observation.has_value())
        complete_webdriver_navigation();
}

void ViewImplementation::did_commit_history_step()
{
    did_update_url();
    update_navigation_action_state();
}

void ViewImplementation::did_finish_history_traversal(Web::HTML::CrossProcessId operation_id)
{
    complete_webdriver_history_traversal(operation_id);
    update_navigation_action_state();
    if (on_browser_history_traversal_complete)
        on_browser_history_traversal_complete();
    dump_session_history("webcontent-history-traversal-complete"sv);
}

Vector<ViewImplementation::SessionHistoryTraversalMenuItem> ViewImplementation::session_history_traversal_menu_items(int direction) const
{
    VERIFY(direction == -1 || direction == 1);

    auto const& session_history = traversable().session_history();
    auto used_steps = session_history.used_steps();
    auto current_used_step_index = session_history.current_used_step_index();

    Vector<SessionHistoryTraversalMenuItem> items;
    auto append_item = [&](size_t target_step_index) {
        auto target_step = used_steps[target_step_index];
        auto const& target_entry = *session_history.top_level_entry_for_step(target_step);
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
        for (size_t target_step_index = current_used_step_index; target_step_index > 0; --target_step_index)
            append_item(target_step_index - 1);
    } else {
        for (size_t target_step_index = current_used_step_index + 1; target_step_index < used_steps.size(); ++target_step_index)
            append_item(target_step_index);
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
    m_zoom_level = clamp_zoom_level(zoom_level);
    update_zoom();
}

void ViewImplementation::set_display_metadata(Optional<u64> display_id, double maximum_frames_per_second)
{
    m_display_id = display_id;
    m_maximum_frames_per_second = maximum_frames_per_second;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_maximum_frames_per_second(m_maximum_frames_per_second);
        update_compositor_display_metadata(page);
    });
}

void ViewImplementation::set_frame_rate_capped(bool capped)
{
    if (m_frame_rate_capped == capped)
        return;
    m_frame_rate_capped = capped;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_frame_rate_capped(m_frame_rate_capped);
    });
}

void ViewImplementation::set_viewport_corner_radius(double radius)
{
    if (m_viewport_corner_radius == radius)
        return;
    m_viewport_corner_radius = radius;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_viewport_corner_radius(m_viewport_corner_radius);
    });
}

void ViewImplementation::update_compositor_display_metadata(WebContentPage& page)
{
    Application::the().update_compositor_display_metadata(page.compositor_context_id(), m_display_id, m_maximum_frames_per_second);
}

static void send_screen_rects_to_page(WebContentPage& page, ViewImplementation::ScreenRects const& screens)
{
    page.async_update_screen_rects(screens.rects, screens.available_rects, screens.main_screen_index);
}

void ViewImplementation::update_screen_rects()
{
    auto screens = screen_rects();
    if (!screens.has_value())
        return;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        send_screen_rects_to_page(page, *screens);
    });
}

void ViewImplementation::update_system_theme(Core::AnonymousBuffer const& theme)
{
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_update_system_theme(theme);
    });
}

void ViewImplementation::send_system_theme_to_page(WebContentPage& page)
{
    if (auto theme = system_theme(); theme.is_valid())
        page.async_update_system_theme(theme);
}

void ViewImplementation::reset_zoom()
{
    m_zoom_level = 1.0;
    update_zoom();
    page().async_reset_zoom();

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

// https://html.spec.whatwg.org/multipage/interaction.html#activation-triggering-input-event
static bool is_activation_triggering_input_event(Web::InputEvent const& event)
{
    // An activation triggering input event is any event whose isTrusted attribute is true and whose type is one of:
    // - "keydown", provided the key is neither the Esc key nor a shortcut key reserved by the user agent;
    // - "mousedown";
    // NB: The pointer and touch events of the rest of the list follow the mouse events the view delivers.
    if (auto const* key_event = event.get_pointer<Web::KeyEvent>())
        return key_event->type == Web::KeyEvent::Type::KeyDown && key_event->key != Web::UIEvents::KeyCode::Key_Escape;
    if (auto const* mouse_event = event.get_pointer<Web::MouseEvent>())
        return mouse_event->type == Web::MouseEvent::Type::MouseDown;
    return false;
}

// A page busy for a while handles input late, and its activation counts from then. A page that never finishes handling
// the input keeps the activation no longer than this.
static constexpr auto longest_user_activation_delivery = AK::Duration::from_seconds(30);

void ViewImplementation::enqueue_input_event(Web::InputEvent event)
{
    // NB: A pinch event's scale delta must be finite to be sent over IPC. Drop a pinch event the platform gave us
    //     with a scale delta that is not finite.
    if (auto const* pinch_event = event.get_pointer<Web::PinchEvent>(); pinch_event && !isfinite(pinch_event->scale_delta))
        return;

    auto* key_event = event.get_pointer<Web::KeyEvent>();
    auto* mouse_event = event.get_pointer<Web::MouseEvent>();
    if (m_debugger_paused) {
        if (mouse_event) {
            auto activated_action = handle_ui_overlay_mouse_event(m_debugger_overlay_pointer_state, *mouse_event, [](Gfx::IntSize viewport_size, double device_pixel_ratio) {
                return Compositing::paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio).buttons();
            });
            if (activated_action.has_value()) {
                resume_debugger(*activated_action == Compositing::PausedDebuggerOverlayAction::StepOver
                        ? DebuggerResumeMode::StepOver
                        : DebuggerResumeMode::Continue);
            }
        }
        return;
    }

    if (m_has_picture_in_picture_controls && mouse_event && !mouse_event->pointer_data.ptr() && handle_picture_in_picture_controls_mouse_event(*mouse_event))
        return;

    // User input enables a single request for an external URL.
    if ((key_event && key_event->type == Web::KeyEvent::Type::KeyDown && !key_event->repeat
            && first_is_one_of(key_event->key, Web::UIEvents::KeyCode::Key_Return, Web::UIEvents::KeyCode::Key_Space))
        || (mouse_event && mouse_event->type == Web::MouseEvent::Type::MouseDown
            && first_is_one_of(mouse_event->button, Web::UIEvents::MouseButton::Primary, Web::UIEvents::MouseButton::Middle))) {
        m_external_url_request_policy.allow_next_request();
    }

    // The page the input goes to gains activation, and so does the page displaying the tab, which hosts the top-level
    // traversable that is an ancestor of every navigable of the tab. A mouse event the page displaying the tab hands
    // to a child frame goes to the page hosting that frame once it does. The input gives activation while the page
    // handles it, and the pages keep it for the transient activation duration once the page finishes.
    auto gives_user_activation = is_activation_triggering_input_event(event);

    if (mouse_event && mouse_event->type == Web::MouseEvent::Type::MouseWheel) {
        mouse_event->wheel_delta_x /= zoom_level();
        mouse_event->wheel_delta_y /= zoom_level();
        mouse_event->wheel_tick_120ths_x /= zoom_level();
        mouse_event->wheel_tick_120ths_y /= zoom_level();
    }

    // Send the next event over to the WebContent to be handled by JS. We'll later get a message to say whether JS
    // prevented the default event behavior, at which point we either discard or handle that event, and then try to
    // process the next one.
    Web::set_input_event_id(event, m_next_input_event_id++);
    m_pending_input_events.append({ move(event), page() });

    auto& pending = m_pending_input_events.last();
    if (gives_user_activation) {
        auto now = MonotonicTime::now();
        pending.user_activation_time = now;
        m_inputs_to_open_links_in_new_tabs.remove_all_matching([&](auto const& input) {
            return now - input.delivered_at >= longest_user_activation_delivery;
        });
        m_inputs_to_open_links_in_new_tabs.append({ Web::input_event_id(pending.event), now });
    }
    // A focused navigable another page hosts takes key events directly.
    if (pending.event.has<Web::KeyEvent>())
        pending.endpoint = focused_navigable_host();
    send_queued_input_events();
}

// Input waits while the compositor decides whether it forwards a key event, so a key it hands back is not overtaken.
void ViewImplementation::send_queued_input_events()
{
    for (size_t index = 0; index < m_pending_input_events.size(); ++index) {
        if (!m_pending_input_events[index].sent)
            send_input_event(index);
        if (m_pending_input_events[index].awaits_compositor_dispatch())
            return;
    }
}

// A key press may scroll in the compositor only while no earlier input the pages have yet to handle can change its
// target. Key release always reaches the compositor's keyboard scrolling, even if it could no longer accept a new
// scroll.
bool ViewImplementation::may_scroll_for_key_event_in_compositor(size_t index) const
{
    auto const& key_event = m_pending_input_events[index].event.get<Web::KeyEvent>();
    if (key_event.native_editing_binding_resolved || !Web::is_keyboard_scroll_key(key_event.key, Web::UIEvents::Mod_None))
        return false;
    if (key_event.type == Web::KeyEvent::Type::KeyUp)
        return true;
    if (!m_has_usable_bitmap)
        return false;
    for (size_t preceding = 0; preceding < index; ++preceding) {
        auto const* key = m_pending_input_events[preceding].event.get_pointer<Web::KeyEvent>();
        if (!key || key->type != Web::KeyEvent::Type::KeyDown || !key->async_scroll_performed_default_action)
            return false;
    }
    return true;
}

void ViewImplementation::send_input_event(size_t index)
{
    auto& pending = m_pending_input_events[index];
    pending.sent = true;
    pending.event.visit(
        [&](Web::KeyEvent const& event) {
            // The compositor scrolls for the event when it may, then forwards the event to WebContent behind the scroll
            // updates that produced, or hands it back for direct dispatch.
            if (pending.endpoint == page() && page().handle_and_dispatch_key_event_in_compositor(event, may_scroll_for_key_event_in_compositor(index)))
                pending.routed_through_compositor = true;
            else
                pending.endpoint->async_key_event(event.clone_without_browser_data());
        },
        [&](Web::MouseEvent const& event) {
            // The compositor scrolls or drags a scrollbar for the event when it can, then forwards the event to
            // WebContent behind the scroll updates that produced, consumes it, or hands it back for direct dispatch.
            if (!event.pointer_data.ptr() && pending.endpoint->handle_and_dispatch_mouse_event_in_compositor(event))
                pending.routed_through_compositor = true;
            else
                pending.endpoint->async_mouse_event(event.clone_without_browser_data());
        },
        [&](Web::DragEvent& event) {
            auto cloned_event = event.clone_without_browser_data();
            cloned_event.files = move(event.files);

            pending.endpoint->async_drag_event(cloned_event);
        },
        [&](Web::PinchEvent const& event) {
            pending.endpoint->handle_pinch_event_in_compositor(event);
            pending.endpoint->async_pinch_event(event);
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
    m_pending_external_url_requests.enqueue({ url, this->url().origin(), move(on_handler_unavailable) });
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

void ViewImplementation::did_finish_handling_input_event(Badge<WebContentPage>, WebContentPage const& reporting_page, u64 event_id, Web::EventResult event_result)
{
    // Adjacent events can be handled by different processes, which finish them in no particular order.
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    if (!index.has_value() || m_pending_input_events[*index].endpoint.ptr() != &reporting_page)
        return;
    auto pending = m_pending_input_events.take(*index);
    auto event = move(pending.event);
    send_queued_input_events();

    // The renderer's activation counts from when it handled the event, so the activation the event gives does too. An
    // event the page dropped gives none, and opens no link.
    if (event_result == Web::EventResult::Dropped) {
        m_inputs_to_open_links_in_new_tabs.remove_all_matching([&](auto const& input) { return input.event_id == event_id; });
    } else if (pending.user_activation_time.has_value()) {
        page().give_user_activation();
        pending.endpoint->give_user_activation();
    }

    // A view can decide what a mouse event goes on to do by whether the page handled it, such as whether the page
    // claimed the press that starts a drag.
    if (auto const* mouse_event = event.get_pointer<Web::MouseEvent>()) {
        if (mouse_event->pointer_data.ptr())
            return;
        if (on_finish_handling_mouse_event)
            on_finish_handling_mouse_event(*mouse_event, event_result);

        // The back and forward buttons traverse the session history once released, unless the page cancelled the
        // mouseup.
        if (mouse_event->type == Web::MouseEvent::Type::MouseUp && event_result != Web::EventResult::Cancelled) {
            if (mouse_event->button == Web::UIEvents::MouseButton::Backward)
                traverse_the_history_by_delta(-1);
            else if (mouse_event->button == Web::UIEvents::MouseButton::Forward)
                traverse_the_history_by_delta(1);
        }
        return;
    }

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
        [](auto const&) { });
}

void ViewImplementation::did_forward_input_event(Badge<WebContentPage>, WebContentPage const& reporting_page, u64 event_id, WebContentPage& endpoint)
{
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    if (!index.has_value() || m_pending_input_events[*index].endpoint.ptr() != &reporting_page)
        return;
    m_pending_input_events[*index].endpoint = endpoint;
}

// The earliest input the page has yet to finish handling that gives it user activation: input the page handles, or
// input to the page displaying the tab, which hosts an ancestor of every navigable of the tab.
Optional<size_t> ViewImplementation::index_of_input_in_flight_giving_user_activation_to(WebContentPage const& page) const
{
    auto now = MonotonicTime::now();
    return m_pending_input_events.find_first_index_if([&](auto const& pending) {
        return pending.user_activation_time.has_value()
            && now - *pending.user_activation_time < longest_user_activation_delivery
            && (pending.endpoint.ptr() == &page || &this->page() == &page);
    });
}

bool ViewImplementation::is_delivering_user_activation_to(WebContentPage const& page) const
{
    return index_of_input_in_flight_giving_user_activation_to(page).has_value();
}

void ViewImplementation::consume_user_activation_of_input_in_flight_to(Badge<WebContentPage>, WebContentPage const& page)
{
    if (auto index = index_of_input_in_flight_giving_user_activation_to(page); index.has_value())
        m_pending_input_events[*index].user_activation_time.clear();
}

// NB: A page handles its input in the order the view delivered it, so the earliest input to the page that the page has
//     yet to finish is the input it is handling, if any.
void ViewImplementation::consume_user_activation_of_input_handled_by(Badge<WebContentPage>, WebContentPage const& page)
{
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return pending.endpoint.ptr() == &page; });
    if (index.has_value())
        m_pending_input_events[*index].user_activation_time.clear();
}

bool ViewImplementation::take_input_to_open_link_in_new_tab(Badge<WebContentPage>)
{
    // NB: Automation lets a renderer synthesize the input that opens a link.
    if (WebContentClient::renderers_may_synthesize_input())
        return true;
    // NB: Each press opens at most one link, the earliest press first, and only while a page could still be handling it.
    auto now = MonotonicTime::now();
    m_inputs_to_open_links_in_new_tabs.remove_all_matching([&](auto const& input) {
        return now - input.delivered_at >= longest_user_activation_delivery;
    });
    if (m_inputs_to_open_links_in_new_tabs.is_empty())
        return false;
    m_inputs_to_open_links_in_new_tabs.remove(0);
    return true;
}

void ViewImplementation::did_lose_input_event_endpoint(Badge<WebContentPage>, WebContentPage& page)
{
    // Nothing will finish the events the closed page held, and a pending event holds back compositor input.
    m_pending_input_events.remove_all_matching([&](auto const& pending) { return pending.endpoint == page; });
    send_queued_input_events();
}

void ViewImplementation::did_consume_input_event_in_compositor(Badge<WebContentPage>, u64 event_id)
{
    // The compositor performed the default action itself, so there is no result to hand to the view, and the page got
    // no input to open a link with.
    m_pending_input_events.remove_first_matching([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    m_inputs_to_open_links_in_new_tabs.remove_all_matching([&](auto const& input) { return input.event_id == event_id; });
    send_queued_input_events();
}

void ViewImplementation::did_not_dispatch_input_event_through_compositor(Badge<WebContentPage>, u64 event_id)
{
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    if (!index.has_value())
        return;

    auto& pending = m_pending_input_events[*index];
    if (!pending.routed_through_compositor)
        return;
    pending.routed_through_compositor = false;
    pending.event.visit(
        [&](Web::KeyEvent const& event) {
            pending.endpoint->async_key_event(event.clone_without_browser_data());
        },
        [&](Web::MouseEvent const& event) {
            pending.endpoint->async_mouse_event(event.clone_without_browser_data());
        },
        [](auto const&) {
            VERIFY_NOT_REACHED();
        });
    send_queued_input_events();
}

void ViewImplementation::did_dispatch_input_event_through_compositor(Badge<WebContentPage>, u64 event_id, bool async_scroll_performed_default_action)
{
    auto index = m_pending_input_events.find_first_index_if([&](auto const& pending) { return Web::input_event_id(pending.event) == event_id; });
    if (!index.has_value())
        return;

    auto& pending = m_pending_input_events[*index];
    if (!pending.awaits_compositor_dispatch())
        return;
    // WebContent finishes the event from here on, as it does an event the view sends it directly.
    pending.routed_through_compositor = false;
    pending.event.get<Web::KeyEvent>().async_scroll_performed_default_action = async_scroll_performed_default_action;
    send_queued_input_events();
}

void ViewImplementation::discard_input_events_routed_through_lost_compositor(Badge<Application>)
{
    // The compositor may have forwarded some of these before it died, and WebContent does not de-duplicate event ids,
    // so sending them again could run an event twice. Acknowledgements that still arrive for them are ignored.
    m_pending_input_events.remove_all_matching([](auto const& pending) { return pending.routed_through_compositor; });
    send_queued_input_events();
}

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    set_page_background_color(preferred_canvas_background_color());

    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_color_scheme(color_scheme);
    });
}

void ViewImplementation::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_contrast(contrast);
    });
}

void ViewImplementation::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    traversable().for_each_hosting_page([&](WebContentPage& page) {
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

// Only a page hosting the tab's traversable takes this request: It applies to the page's local traversable, which a page
// embedding an out-of-process iframe doesn't have.
static void send_force_dark(WebContentPage const& page)
{
    // FIXME: This is not a "debug request". Add a proper endpoint for this toggle.
    page.async_debug_request("set-force-dark"sv, Application::settings().content_settings().enable_force_dark ? "on"sv : "off"sv);
}

void ViewImplementation::send_preferences_to_page(WebContentPage& page)
{
    send_system_theme_to_page(page);
    page.async_set_preferred_color_scheme(m_preferred_color_scheme);
    page.async_set_preferred_contrast(m_preferred_contrast);
    page.async_set_preferred_motion(m_preferred_motion);
    page.async_set_preferred_languages(Application::settings().languages());
    page.async_set_zoom_level(m_zoom_level);
    send_browsing_behavior(page);
    send_autoplay_settings(page);
    send_global_privacy_control(page);
    auto* spell_checker = Application::the().spell_checker();
    page.async_set_spell_checking_available(spell_checker != nullptr);
    page.async_set_writing_suggestions_available(spell_checker && spell_checker->can_suggest());
    send_geolocation_emulated_position(page);
    page.async_set_has_picture_in_picture_support(Application::the().supports_picture_in_picture());
}

void ViewImplementation::notify_cookies_changed(HashTable<String> const& changed_domains, ReadonlySpan<HTTP::Cookie::Cookie> page_cookies, ReadonlySpan<HTTP::Cookie::Cookie> host_cookies)
{
    for (auto const& domain : changed_domains) {
        if (auto document_index = m_document_cookie_version_indices.get(domain); document_index.has_value())
            Core::increment_shared_version(m_document_cookie_version_buffer, *document_index);
    }

    if (!page_cookies.is_empty())
        page().async_cookies_changed(page_cookies);
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
            dbgln("Reached maximum document cookie version count for {}, cannot create new version for {}", url(), domain);
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
    auto position = to_content_position(widget_position).to_type<Web::DevicePixels>() - traversable().focused_navigable_host_offset();
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

void ViewImplementation::find_in_page(Utf16String const& query, CaseSensitivity case_sensitivity, bool highlight_all_matches)
{
    page().async_find_in_page(query, case_sensitivity, highlight_all_matches);
}

void ViewImplementation::find_in_page_next_match()
{
    page().async_find_in_page_next_match();
}

void ViewImplementation::find_in_page_previous_match()
{
    page().async_find_in_page_previous_match();
}

void ViewImplementation::find_in_page_end()
{
    page().async_find_in_page_end();
}

void ViewImplementation::get_source()
{
    page().async_get_source();
}

// Only a website the page is showing can be reported, so not a browser page or a page that crashed.
bool ViewImplementation::can_report_website_issue() const
{
    auto url = this->url();
    return (url.scheme() == "http"sv || url.scheme() == "https"sv) && !crash_overlay_active();
}

void ViewImplementation::report_website_issue()
{
    if (can_report_website_issue() && on_request_website_issue_report)
        on_request_website_issue_report();
}

void ViewImplementation::update_report_website_issue_action()
{
    m_report_website_issue_action->set_enabled(can_report_website_issue());
    Application::the().update_report_website_issue_action();
}

void ViewImplementation::inspect_dom_tree()
{
    page().async_inspect_dom_tree();
}

void ViewImplementation::inspect_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, u64 request_id)
{
    page().async_inspect_storage(storage_endpoint, request_id);
}

void ViewImplementation::inspect_accessibility_tree()
{
    page().async_inspect_accessibility_tree();
}

void ViewImplementation::request_accessibility_tree()
{
    m_accessibility_tree_requested = true;
    page().async_request_accessibility_tree();
}

void ViewImplementation::perform_accessibility_action(i64 node_id, String action)
{
    page().async_perform_accessibility_action(node_id, move(action));
}

void ViewImplementation::perform_accessibility_text_action(i64 node_id, String action, i32 offset_start,
    i32 offset_end, String text)
{
    page().async_perform_accessibility_text_action(node_id, move(action), offset_start, offset_end, move(text));
}

void ViewImplementation::get_hovered_node_id()
{
    page().async_get_hovered_node_id();
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
    page().async_get_node_id_at_position(request_id, position);
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
    page().async_inspect_dom_node(property_type, node_id, pseudo_element, move(options));
}

void ViewImplementation::inspect_grid_layouts(Web::UniqueNodeID root_node_id)
{
    page().async_inspect_grid_layouts(root_node_id);
}

void ViewImplementation::inspect_current_grid(Web::UniqueNodeID node_id)
{
    page().async_inspect_current_grid(node_id);
}

void ViewImplementation::inspect_current_flexbox(Web::UniqueNodeID node_id, bool only_look_at_parents)
{
    page().async_inspect_current_flexbox(node_id, only_look_at_parents);
}

void ViewImplementation::inspect_indexed_database_storage(DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    page().async_inspect_indexed_database_storage(request_id);
}

void ViewImplementation::inspect_indexed_database_objects(String const& host, Optional<JsonArray> names, JsonObject options, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    if (names.has_value())
        page().async_inspect_indexed_database_objects(request_id, host, JsonValue { names.release_value() }, JsonValue { move(options) });
    else
        page().async_inspect_indexed_database_objects(request_id, host, JsonValue {}, JsonValue { move(options) });
}

void ViewImplementation::delete_indexed_database(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    page().async_delete_indexed_database(request_id, host, name);
}

void ViewImplementation::clear_indexed_database_object_store(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    page().async_clear_indexed_database_object_store(request_id, host, name);
}

void ViewImplementation::delete_indexed_database_record(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete on_complete)
{
    auto request_id = m_next_indexed_database_inspection_request_id++;
    m_pending_indexed_database_inspection_requests.set(request_id, move(on_complete));
    page().async_delete_indexed_database_record(request_id, host, name);
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
    page().async_list_devtools_sources(request_id);
}

void ViewImplementation::request_devtools_source(Web::HTML::ScriptRegistryIdentifier const& source_id)
{
    page().async_request_devtools_source(source_id);
}

void ViewImplementation::attach_debugger(DevTools::DevToolsDelegate::OnDebuggerPaused on_paused, DevTools::DevToolsDelegate::OnDebuggerResumed on_resumed)
{
    on_debugger_paused = move(on_paused);
    on_debugger_resumed = move(on_resumed);
    m_debugger_is_attached = true;
    page().async_attach_debugger();
}

void ViewImplementation::configure_debugger(DebuggerConfiguration configuration)
{
    page().async_configure_debugger(configuration);
}

void ViewImplementation::detach_debugger()
{
    fail_pending_debugger_requests();
    m_debugger_is_attached = false;
    on_debugger_paused = nullptr;
    on_debugger_resumed = nullptr;
    page().async_detach_debugger();
}

void ViewImplementation::interrupt_debugger()
{
    page().async_interrupt_debugger();
}

void ViewImplementation::resume_debugger(DebuggerResumeMode mode)
{
    page().async_resume_debugger(mode);
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
    apply_cursor();
}

void ViewImplementation::did_enter_tooltip_area(Badge<WebContentPage>, ByteString title)
{
    m_page_tooltip = move(title);
    apply_tooltip();
}

void ViewImplementation::did_leave_tooltip_area(Badge<WebContentPage>)
{
    m_page_tooltip.clear();
    apply_tooltip();
}

// Returns the action of the button that the event activated.
template<typename Action, typename ButtonsForViewport>
Optional<Action> ViewImplementation::handle_ui_overlay_mouse_event(Compositing::OverlayPointerState<Action>& pointer_state, Web::MouseEvent const& event, ButtonsForViewport const& buttons_for_viewport)
{
    auto overlay_position = ui_overlay_position(event.position);
    auto buttons = buttons_for_viewport(overlay_position.viewport_size, overlay_position.device_pixel_ratio);
    auto outcome = pointer_state.handle_mouse_event(event, Compositing::overlay_action_at(buttons, overlay_position.position));
    if (outcome.hover_changed) {
        update_ui_overlays();
        apply_cursor();
        apply_tooltip();
    }
    return outcome.activated_action;
}

void ViewImplementation::apply_cursor()
{
    if (on_cursor_change)
        on_cursor_change(is_ui_overlay_hovered() ? Gfx::Cursor { Gfx::StandardCursor::Hand } : m_page_cursor);
}

static StringView paused_debugger_overlay_action_tooltip(Compositing::PausedDebuggerOverlayAction action)
{
    switch (action) {
    case Compositing::PausedDebuggerOverlayAction::StepOver:
        return "Step over"sv;
    case Compositing::PausedDebuggerOverlayAction::Continue:
        return "Resume"sv;
    }
    VERIFY_NOT_REACHED();
}

static StringView picture_in_picture_control_tooltip(Compositing::PictureInPictureControl control)
{
    switch (control) {
    case Compositing::PictureInPictureControl::BackToTab:
        return "Back to tab"sv;
    case Compositing::PictureInPictureControl::Close:
        return "Close"sv;
    }
    VERIFY_NOT_REACHED();
}

void ViewImplementation::apply_tooltip()
{
    auto tooltip = m_page_tooltip;
    if (auto action = m_debugger_overlay_pointer_state.hovered_action(); action.has_value())
        tooltip = paused_debugger_overlay_action_tooltip(*action);
    if (auto control = m_picture_in_picture_controls_pointer_state.hovered_action(); control.has_value())
        tooltip = picture_in_picture_control_tooltip(*control);

    if (tooltip.has_value()) {
        if (on_enter_tooltip_area)
            on_enter_tooltip_area(*tooltip);
    } else if (on_leave_tooltip_area) {
        on_leave_tooltip_area();
    }
}

void ViewImplementation::set_debugger_paused(bool paused)
{
    if (m_debugger_paused == paused)
        return;

    m_debugger_paused = paused;
    if (!paused) {
        m_debugger_overlay_pointer_state.reset();
        apply_cursor();
        apply_tooltip();
    }
    update_ui_overlays();
}

// The compositor draws the controls and the view handles them, rather than the page, so that they work whatever the
// page does.
// FIXME: Expose the controls to accessibility clients, which only see the page's tree.
void ViewImplementation::enable_picture_in_picture_controls()
{
    m_has_picture_in_picture_controls = true;

    // The same delay as the page's media controls.
    static constexpr int hide_delay_ms = 1000;
    m_picture_in_picture_controls_hide_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(hide_delay_ms), [this] {
        if (!m_picture_in_picture_controls_pointer_state.hovered_action().has_value())
            set_picture_in_picture_controls_shown(false);
    });

    update_ui_overlays();
}

// Returns whether the event was the controls', which the page then never sees.
bool ViewImplementation::handle_picture_in_picture_controls_mouse_event(Web::MouseEvent const& event)
{
    // A cancelled press counts as the pointer leaving the controls, as it does for the page.
    Optional<Web::MouseEvent> leave_for_cancel;
    if (event.type == Web::MouseEvent::Type::MouseCancel) {
        leave_for_cancel = event.clone_without_browser_data();
        leave_for_cancel->type = Web::MouseEvent::Type::MouseLeave;
    }
    auto const& controls_event = leave_for_cancel.has_value() ? *leave_for_cancel : event;

    if (controls_event.type == Web::MouseEvent::Type::MouseMove) {
        set_picture_in_picture_controls_shown(true);
        m_picture_in_picture_controls_hide_timer->restart();
    } else if (controls_event.type == Web::MouseEvent::Type::MouseLeave) {
        m_picture_in_picture_controls_hide_timer->stop();
        set_picture_in_picture_controls_shown(false);
    }

    if (event.type == Web::MouseEvent::Type::MouseDown && !picture_in_picture_controls_are_shown())
        return false;

    // A control is in use while it is hovered, and for the rest of a press on it, as the page sees none of that press.
    auto& pointer_state = m_picture_in_picture_controls_pointer_state;
    auto controls_in_use = [&] { return pointer_state.hovered_action().has_value() || pointer_state.is_pressing(); };
    auto was_pressing = pointer_state.is_pressing();
    auto was_in_use = controls_in_use();
    auto activated_control = handle_ui_overlay_mouse_event(pointer_state, controls_event, [](Gfx::IntSize viewport_size, double device_pixel_ratio) {
        return Compositing::picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio).buttons();
    });
    if (auto is_in_use = controls_in_use(); is_in_use != was_in_use)
        page().async_set_picture_in_picture_window_controls_in_use(is_in_use);
    if (activated_control.has_value() && on_picture_in_picture_control_activated)
        on_picture_in_picture_control_activated(*activated_control);

    // The page still learns that the pointer left, even during a press on a control.
    if (event.type == Web::MouseEvent::Type::MouseLeave)
        return false;
    return was_pressing || pointer_state.is_pressing();
}

void ViewImplementation::set_picture_in_picture_controls_shown(bool shown)
{
    if (m_picture_in_picture_controls_shown == shown)
        return;

    m_picture_in_picture_controls_shown = shown;
    update_ui_overlays();
}

// The controls show while the player's own controls do, as the player's stay shown while the controls are in use, so
// that the two fade together. The page can only keep the controls up, which then still show and work without it.
void ViewImplementation::did_change_picture_in_picture_player_controls_visibility(Badge<WebContentPage>, bool shown)
{
    if (m_picture_in_picture_player_controls_shown == shown)
        return;

    m_picture_in_picture_player_controls_shown = shown;
    if (m_has_picture_in_picture_controls)
        update_ui_overlays();
}

Optional<ViewImplementation::DisplayedFrame> ViewImplementation::displayed_frame() const
{
    if (m_has_usable_bitmap)
        return DisplayedFrame { m_front_bitmap.last_painted_size.to_type<int>(), m_front_bitmap.last_painted_device_pixel_ratio };
    if (m_backup_shared_image_buffer)
        return DisplayedFrame { m_backup_bitmap_size.to_type<int>(), m_backup_bitmap_device_pixel_ratio };
    return {};
}

// The compositor draws the browser's overlays over the frame it presents, at the pixel ratio it painted the frame at,
// and the frame may be scaled to fit the view.
ViewImplementation::UIOverlayPosition ViewImplementation::ui_overlay_position(Web::DevicePixelPoint position) const
{
    auto view_size = viewport_size().to_type<int>();
    auto view_position = position.to_type<int>();
    auto frame = displayed_frame();
    if (!frame.has_value() || frame->size.is_empty())
        return { view_position, view_size, m_device_pixel_ratio };
    if (!m_scales_frames_to_fit || frame->size == view_size)
        return { view_position, frame->size, frame->device_pixel_ratio };

    auto fitted_frame = rect_fitting_frame(frame->size, view_size);
    if (fitted_frame.is_empty())
        return { view_position, view_size, m_device_pixel_ratio };

    return {
        .position = {
            (view_position.x() - fitted_frame.x()) * frame->size.width() / fitted_frame.width(),
            (view_position.y() - fitted_frame.y()) * frame->size.height() / fitted_frame.height(),
        },
        .viewport_size = frame->size,
        .device_pixel_ratio = frame->device_pixel_ratio,
    };
}

void ViewImplementation::update_ui_overlays()
{
    Compositing::UIOverlays overlays;
    if (m_debugger_paused) {
        overlays.paused_debugger = Compositing::PausedDebuggerOverlayState {
            .font_family = Application::the().ui_font_family(),
            .hovered_action = m_debugger_overlay_pointer_state.hovered_action(),
        };
    }
    if (m_has_picture_in_picture_controls) {
        overlays.picture_in_picture_controls = Compositing::PictureInPictureControlsState {
            .shown = picture_in_picture_controls_are_shown(),
            .hovered_control = m_picture_in_picture_controls_pointer_state.hovered_action(),
        };
    }
    Application::the().update_compositor_ui_overlays(page().compositor_context_id(), overlays);
}

void ViewImplementation::update_debugger_blackboxing(Utf16String url, Vector<DebuggerBlackboxRange> ranges, DebuggerBlackboxingOperation operation)
{
    page().async_update_debugger_blackboxing(move(url), move(ranges), operation);
}

void ViewImplementation::set_debugger_breakpoint(DebuggerBreakpointLocation location, DebuggerBreakpointOptions options, DevTools::DevToolsDelegate::OnDebuggerBreakpointOperationComplete on_complete)
{
    auto request_id = m_next_debugger_breakpoint_request_id++;
    m_pending_debugger_breakpoint_requests.set(request_id, move(on_complete));
    page().async_set_debugger_breakpoint(request_id, move(location), move(options));
}

void ViewImplementation::remove_debugger_breakpoint(DebuggerBreakpointLocation location, DevTools::DevToolsDelegate::OnDebuggerBreakpointOperationComplete on_complete)
{
    auto request_id = m_next_debugger_breakpoint_request_id++;
    m_pending_debugger_breakpoint_requests.set(request_id, move(on_complete));
    page().async_remove_debugger_breakpoint(request_id, move(location));
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
    page().async_get_debugger_environments(request_id, frame_id);
}

void ViewImplementation::evaluate_javascript_in_debugger_frame(u64 frame_id, String const& source_text, DevTools::DevToolsDelegate::OnDebuggerEvaluationComplete on_complete)
{
    auto request_id = m_next_debugger_evaluation_request_id++;
    m_pending_debugger_evaluation_requests.set(request_id, move(on_complete));
    page().async_evaluate_javascript_in_debugger_frame(request_id, frame_id, Utf16String::from_utf8(source_text));
}

void ViewImplementation::retrieve_debugger_object_properties(u64 object_id, DevTools::DevToolsDelegate::OnDebuggerObjectPropertiesReceived on_complete)
{
    auto request_id = m_next_debugger_object_properties_request_id++;
    m_pending_debugger_object_properties_requests.set(request_id, move(on_complete));
    page().async_get_debugger_object_properties(request_id, object_id);
}

void ViewImplementation::retrieve_debugger_source_positions(Web::HTML::ScriptRegistryIdentifier source_id, DevTools::DevToolsDelegate::OnDebuggerSourcePositionsReceived on_complete)
{
    auto request_id = m_next_debugger_source_positions_request_id++;
    m_pending_debugger_source_positions_requests.set(request_id, move(on_complete));
    page().async_get_debugger_source_positions(request_id, source_id);
}

void ViewImplementation::resolve_dom_node_url(Optional<Web::UniqueNodeID> node_id, String const& url, DevTools::DevToolsDelegate::OnResolvedURLReceived on_complete)
{
    auto request_id = m_next_resolve_dom_node_url_request_id++;
    on_resolved_dom_node_url.set(request_id, move(on_complete));
    page().async_resolve_dom_node_url(request_id, node_id, url);
}

void ViewImplementation::clear_inspected_dom_node()
{
    page().async_clear_inspected_dom_node();
}

void ViewImplementation::highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    page().async_highlight_dom_node(node_id, pseudo_element);
}

void ViewImplementation::clear_highlighted_dom_node()
{
    highlight_dom_node(0, {});
}

void ViewImplementation::highlight_flexbox(Web::UniqueNodeID node_id, JsonValue options)
{
    page().async_highlight_flexbox(node_id, move(options));
}

void ViewImplementation::clear_flexbox_highlight(Web::UniqueNodeID node_id)
{
    page().async_clear_flexbox_highlight(node_id);
}

void ViewImplementation::highlight_grid(Web::UniqueNodeID node_id, JsonValue options)
{
    page().async_highlight_grid(node_id, move(options));
}

void ViewImplementation::clear_grid_highlight(Web::UniqueNodeID node_id)
{
    page().async_clear_grid_highlight(node_id);
}

void ViewImplementation::set_listen_for_dom_mutations(bool listen_for_dom_mutations)
{
    page().async_set_listen_for_dom_mutations(listen_for_dom_mutations);
}

void ViewImplementation::did_connect_devtools_client()
{
    m_devtools_connected = true;
    page().async_set_has_devtools_client(true);
}

void ViewImplementation::did_disconnect_devtools_client()
{
    m_devtools_connected = false;
    page().async_set_has_devtools_client(false);
}

void ViewImplementation::get_dom_node_inner_html(Web::UniqueNodeID node_id)
{
    page().async_get_dom_node_inner_html(node_id);
}

void ViewImplementation::get_dom_node_outer_html(Web::UniqueNodeID node_id)
{
    page().async_get_dom_node_outer_html(node_id);
}

void ViewImplementation::set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html)
{
    page().async_set_dom_node_outer_html(node_id, html);
}

void ViewImplementation::set_dom_node_text(Web::UniqueNodeID node_id, String const& text)
{
    page().async_set_dom_node_text(node_id, text);
}

void ViewImplementation::set_dom_node_tag(Web::UniqueNodeID node_id, Utf16FlyString const& name)
{
    page().async_set_dom_node_tag(node_id, name);
}

void ViewImplementation::add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes)
{
    page().async_add_dom_node_attributes(node_id, attributes);
}

void ViewImplementation::replace_dom_node_attribute(Web::UniqueNodeID node_id, Utf16FlyString const& name, ReadonlySpan<Attribute> replacement_attributes)
{
    page().async_replace_dom_node_attribute(node_id, name, replacement_attributes);
}

void ViewImplementation::create_child_element(Web::UniqueNodeID node_id)
{
    page().async_create_child_element(node_id);
}

void ViewImplementation::create_child_text_node(Web::UniqueNodeID node_id)
{
    page().async_create_child_text_node(node_id);
}

void ViewImplementation::insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id)
{
    page().async_insert_dom_node_before(node_id, parent_node_id, sibling_node_id);
}

void ViewImplementation::clone_dom_node(Web::UniqueNodeID node_id)
{
    page().async_clone_dom_node(node_id);
}

void ViewImplementation::remove_dom_node(Web::UniqueNodeID node_id)
{
    page().async_remove_dom_node(node_id);
}

void ViewImplementation::list_style_sheets()
{
    page().async_list_style_sheets();
}

void ViewImplementation::request_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier)
{
    page().async_request_style_sheet_source(identifier);
}

void ViewImplementation::did_block_resource(Badge<WebContentPage>)
{
    if (m_blocked_request_count != NumericLimits<u64>::max())
        ++m_blocked_request_count;
    notify_content_blocking_changed();
}

void ViewImplementation::reset_blocked_request_count()
{
    m_blocked_request_count = 0;
    notify_content_blocking_changed();
}

void ViewImplementation::notify_content_blocking_changed()
{
    if (on_content_blocking_change)
        on_content_blocking_change(content_blocking_snapshot());
}

ContentBlockingSnapshot ViewImplementation::content_blocking_snapshot() const
{
    ContentBlockingSnapshot snapshot;
    snapshot.blocked_request_count = m_blocked_request_count;

    if (m_crash_state.has_value() || !traversable().active_document().host())
        return snapshot;

    auto entry = traversable().active_session_history_entry();
    if (!entry)
        return snapshot;

    snapshot.site = Web::content_blocking_site(entry->url);
    if (!snapshot.site.has_value())
        return snapshot;

    if (!Application::the().content_blocking_enabled())
        snapshot.state = ContentBlockingState::Disabled;
    else if (!Application::settings().content_blocker_enabled_for_site(*snapshot.site))
        snapshot.state = ContentBlockingState::SiteDisabled;
    else
        snapshot.state = Application::the().has_content_blocking_rules() ? ContentBlockingState::Enabled : ContentBlockingState::NoRules;

    return snapshot;
}

void ViewImplementation::debug_request(ByteString const& request, ByteString const& argument)
{
    if (request == "dump-session-history"sv)
        dump_session_history("debug-request"sv, SessionHistoryDumpMode::Always);
    if (request == "dump-site-isolation-process-tree"sv) {
        dbgln("{}", page().dump_process_tree());
        return;
    }

    page().async_debug_request(request, argument);
}

void ViewImplementation::run_javascript(String const& js_source)
{
    page().async_run_javascript(js_source);
}

void ViewImplementation::js_console_input(String const& js_source)
{
    page().async_js_console_input(js_source);
}

void ViewImplementation::exit_fullscreen()
{
    page().async_exit_fullscreen();
}

void ViewImplementation::set_is_fullscreen(Web::ViewportIsFullscreen is_fullscreen)
{
    if (m_is_fullscreen == is_fullscreen)
        return;
    m_is_fullscreen = is_fullscreen;

    // NB: handle_resize() carries the state to the page displaying the tab. A page holding only part of the tab has
    //     no viewport of its own to resize, and a fullscreen request its document made waits on the state.
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (!page.displays_tab())
            page.async_set_viewport_is_fullscreen(is_fullscreen);
    });

    handle_resize();
}

// NB: Every page of the tab holds the dialog a document of one of them opened.
void ViewImplementation::alert_closed()
{
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_alert_closed();
    });
}

void ViewImplementation::before_unload_closed(bool accepted)
{
    page().routed_proxy()->async_before_unload_closed(page().id(), accepted);
}

void ViewImplementation::confirm_closed(bool accepted)
{
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_confirm_closed(accepted);
    });
}

void ViewImplementation::prompt_closed(Optional<Utf16String> const& response)
{
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_prompt_closed(response);
    });
}

// The UI shows one dialog of a kind at a time, so a page asking while another page's is open is told it closed.
static bool another_page_awaits(RefPtr<WebContentPage> const& owner, WebContentPage& requesting_page)
{
    return owner && owner->is_open() && owner.ptr() != &requesting_page;
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
    target->async_select_dropdown_closed(selected_item_id);
}

// The page hosting the tab's focused navigable, which the view's own page is when no other process hosts it.
WebContentPage& ViewImplementation::focused_navigable_host() const
{
    auto host = traversable().focused_navigable_host();
    return host ? *host : page();
}

void ViewImplementation::paste_from_clipboard()
{
    // The user asked the browser to paste, which lets the page read the clipboard.
    auto& host = focused_navigable_host();
    host.give_user_activation();
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
    page().async_set_page_mute_state(m_mute_state);
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
    traversable().for_each_hosting_page([&](WebContentPage& page) {
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

void ViewImplementation::did_allocate_backing_stores(Badge<WebContentPage>, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    install_backing_stores(move(bitmap_ids), move(backing_stores));
}

void ViewImplementation::did_add_backing_stores(Badge<WebContentPage>, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    VERIFY(bitmap_ids.size() == backing_stores.size());
    for (size_t i = 0; i < backing_stores.size(); ++i) {
        m_other_bitmaps.append({
            .id = bitmap_ids[i],
            .last_painted_size = {},
            .shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[i]))),
        });
    }
}

void ViewImplementation::did_retire_backing_stores(Badge<WebContentPage>, ReadonlySpan<i32> bitmap_ids)
{
    m_other_bitmaps.remove_all_matching([&](auto const& bitmap) {
        return bitmap_ids.contains_slow(bitmap.id);
    });
}

void ViewImplementation::install_backing_stores(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    VERIFY(bitmap_ids.size() == backing_stores.size());
    VERIFY(!bitmap_ids.is_empty());
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI installing {} backing stores for page {} had_usable_bitmap={}",
        backing_stores.size(), page().id(), m_has_usable_bitmap);
    if (m_has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_shared_image_buffer = move(m_front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_front_bitmap.last_painted_size;
        m_backup_bitmap_device_pixel_ratio = m_front_bitmap.last_painted_device_pixel_ratio;
    }
    m_has_usable_bitmap = false;
    m_front_bitmap.id = bitmap_ids[0];
    m_front_bitmap.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[0])));
    m_other_bitmaps.clear();
    m_other_bitmaps.ensure_capacity(backing_stores.size() - 1);
    for (size_t i = 1; i < backing_stores.size(); ++i) {
        m_other_bitmaps.append({
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

    // Every process showing part of the tab lays out and converts input at the tab's zoom level.
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_zoom_level(m_zoom_level);
    });
}

void ViewImplementation::apply_zoom_for_current_host()
{
    auto zoom_level = Application::settings().zoom_for_host(current_host_for_settings());
    if (zoom_level == m_zoom_level)
        return;

    m_zoom_level = zoom_level;
    update_zoom();
}

// The device pixel ratio a test set in `page`, which displays the tab: the pages that show the tab later lay out at it
// too, and `page` takes it after any viewport sent to it before.
void ViewImplementation::set_device_pixel_ratio_for_testing(WebContentPage& page, double device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
    page.async_set_viewport(viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
}

Gfx::IntRect ViewImplementation::rect_fitting_frame(Gfx::IntSize frame_size, Gfx::IntSize target_size)
{
    auto scale = min(static_cast<float>(target_size.width()) / frame_size.width(), static_cast<float>(target_size.height()) / frame_size.height());
    auto fitted_size = frame_size.to_type<float>().scaled(scale).to_rounded<int>();
    return Gfx::IntRect { {}, fitted_size }.centered_within({ {}, target_size });
}

void ViewImplementation::handle_resize()
{
    page().async_set_viewport(viewport_size(), m_device_pixel_ratio, m_is_fullscreen);

    // The page forwards its new size to the compositor once it has laid it out. Until then, scaling frames to fit needs
    // the compositor to keep presenting them whole at the size they were laid out for.
    if (!m_scales_frames_to_fit)
        Application::the().update_compositor_viewport(page().compositor_context_id(), viewport_size().to_type<int>(), m_device_pixel_ratio, Compositing::WindowResizingInProgress::Yes);

    if (m_debugger_paused) {
        m_debugger_overlay_pointer_state.reset();
        update_ui_overlays();
        apply_cursor();
        apply_tooltip();
    }
}

void ViewImplementation::initialize_tab(Web::HTML::VisibilityState system_visibility_state, Optional<CanonicalTraversable&> traversable)
{
    if (!traversable.has_value()) {
        // FIXME: Fail to open the tab, rather than crashing the whole application if this fails.
        auto page = MUST(Application::the().open_page_for_new_tab(m_is_private));
        traversable = page->traversable();
    }
    display_traversable(*traversable, system_visibility_state);
    // The launched process's initial page, or the page the opener's process opened for the tab, displays it.
    VERIFY(page().is_open());
    VERIFY(page().client().is_private() == m_is_private);

    if (m_window_handle.is_empty()) {
        m_window_handle = generate_random_uuid();
        if (!m_owner_view_id.has_value()) {
            Application::the().notify_webdriver_window_created(m_window_handle);

            // NB: WebDriver BiDi names a top-level traversable by its window handle, so the traversable is reported as
            //     created once it has one.
            if (auto* webdriver_bidi = WebDriverBiDi::the())
                webdriver_bidi->top_level_traversable_did_get_window_handle(*traversable);
        }
    }
    prepare_page_for_tab(page());
    display_page_changed({});

    languages_changed();
    content_settings_changed();
    geolocation_settings_changed();

    using GeolocationErrorCode = Web::Geolocation::GeolocationPositionErrorCode;

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
            if (!view || !request_page->is_open())
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
            if (!view || !request_page->is_open())
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

void ViewImplementation::did_start_navigation()
{
    reset_blocked_request_count();
    set_loading_state(true);
    dump_session_history("did-start-navigation"sv);
}

void ViewImplementation::did_cancel_loading(Badge<CanonicalNavigable>, bool canceled_navigation)
{
    set_loading_state(false);
    if (canceled_navigation)
        did_end_uncommitted_navigation("did-cancel-navigation"sv);
    else if (m_webdriver_navigation_observation.has_value())
        complete_webdriver_navigation();

    auto const& client_url = url();
    if (on_load_finish)
        on_load_finish(client_url);

    for (auto const& [id, listener] : m_navigation_listeners) {
        if (listener.on_load_finish)
            listener.on_load_finish(client_url);
    }
}

void ViewImplementation::did_finish_navigation()
{
    m_last_load_finished = true;
    set_loading_state(false);

    if (!m_webdriver_navigation_observation.has_value())
        return;

    switch (m_webdriver_navigation_observation->completion_source) {
    case WebDriverNavigationCompletionSource::CrashRecovery:
        m_webdriver_navigation_observation->load_completed = true;
        if (m_webdriver_navigation_observation->history_operation_completed)
            complete_webdriver_navigation();
        break;
    case WebDriverNavigationCompletionSource::Load:
        complete_webdriver_navigation();
        break;
    case WebDriverNavigationCompletionSource::HistoryTraversal:
        break;
    }
}

void ViewImplementation::set_loading_state(bool is_loading)
{
    if (is_loading)
        m_last_load_finished = false;
    if (m_is_loading == is_loading)
        return;
    m_is_loading = is_loading;
    if (on_loading_state_change)
        on_loading_state_change(is_loading);
}

void ViewImplementation::run_webdriver_content_command(u64 command_id, Web::WebDriver::SessionBrowsingContext browsing_context, String const& name, JsonValue payload, Vector<String> arguments)
{
    Optional<Web::HTML::CrossProcessId> navigable_id;
    if (browsing_context == Web::WebDriver::SessionBrowsingContext::Current)
        navigable_id = m_webdriver_current_navigable_id;

    run_webdriver_content_command(command_id, navigable_id, name, move(payload), move(arguments));
}

void ViewImplementation::run_webdriver_bidi_content_command(u64 command_id, Web::HTML::CrossProcessId navigable_id, String const& method, JsonValue params)
{
    run_webdriver_content_command(command_id, navigable_id, method, move(params), {}, WebDriverProtocol::BiDi);
}

void ViewImplementation::run_webdriver_content_command(u64 command_id, Optional<Web::HTML::CrossProcessId> navigable_id, String const& name, JsonValue payload, Vector<String> arguments, WebDriverProtocol protocol)
{
    if (Web::WebDriver::is_input_state_command(name)) {
        m_webdriver_input_commands.append({ command_id, { navigable_id, name, move(payload), move(arguments), protocol } });
        if (m_webdriver_input_commands.size() == 1)
            dispatch_next_webdriver_input_command();
        return;
    }
    dispatch_webdriver_content_command(command_id, navigable_id, name, move(payload), move(arguments), protocol);
}

void ViewImplementation::discard_webdriver_input_commands(bool process_crashed)
{
    auto queued = move(m_webdriver_input_commands);
    m_webdriver_input_command_running = false;
    m_webdriver_input_state = JsonValue {};
    for (auto const& command : queued) {
        m_pending_webdriver_commands.remove(command.command_id);
        m_webdriver_commands_waiting_for_a_document.remove(command.command_id);
        Application::the().complete_webdriver_content_command(command.command_id, Web::WebDriver::Error::from_code(process_crashed ? Web::WebDriver::ErrorCode::UnknownError : error_code_for_a_navigable_that_is_gone(command.command.protocol), process_crashed ? "WebContent crashed while waiting for input"sv : "Browsing context was discarded while waiting for input"sv));
    }
}

void ViewImplementation::dispatch_next_webdriver_input_command()
{
    if (m_webdriver_input_commands.is_empty() || m_webdriver_input_command_running)
        return;
    m_webdriver_input_command_running = true;
    m_webdriver_input_state_was_lost = false;
    auto const& queued = m_webdriver_input_commands.first();
    auto const& command = queued.command;
    JsonObject envelope;
    envelope.set("__webdriver_input_state"sv, m_webdriver_input_state);
    envelope.set("__webdriver_input_payload"sv, command.payload);
    dispatch_webdriver_content_command(queued.command_id, command.navigable_id, command.name, move(envelope), command.arguments, command.protocol);
}

void ViewImplementation::complete_webdriver_input_command(u64 command_id, Web::WebDriver::Response response)
{
    if (!m_webdriver_input_commands.is_empty() && m_webdriver_input_commands.first().command_id == command_id) {
        if (!response.is_error()) {
            // NB: A click's navigation wait can finish in the browser or a replacement page without an envelope.
            bool is_navigation_completion = m_webdriver_input_commands.first().command.name == "element_click"sv && response.value().is_null();
            if (!is_navigation_completion) {
                JsonValue discarded_state;
                auto& input_state = m_webdriver_input_state_was_lost ? discarded_state : m_webdriver_input_state;
                response = Web::WebDriver::unwrap_input_command_response(response.value(), input_state);
            }
        }
        if (m_webdriver_input_state_was_lost)
            response = Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Input state was lost while executing the command"sv);
        m_webdriver_input_commands.take_first();
        m_webdriver_input_command_running = false;
        // The content executor may complete before removing its local action-queue token.
        Core::deferred_invoke([weak_this = make_weak_ptr()] {
            if (weak_this)
                weak_this->dispatch_next_webdriver_input_command();
        });
    }
    Application::the().complete_webdriver_content_command(command_id, move(response));
}

void ViewImplementation::dispatch_webdriver_content_command(u64 command_id, Optional<Web::HTML::CrossProcessId> navigable_id, String const& name, JsonValue payload, Vector<String> arguments, WebDriverProtocol protocol)
{
    if (m_crash_state.has_value() && !m_crash_state->recovery_started) {
        complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent has crashed"sv));
        return;
    }

    // NB: A command runs in the process hosting the browsing context it runs against.
    RefPtr<WebContentPage> target = this->page();
    Optional<CanonicalNavigable&> navigable = traversable();
    if (navigable_id.has_value()) {
        // https://w3c.github.io/webdriver/#dfn-no-longer-open
        // A browsing context is said to be no longer open if its navigable has been destroyed.
        navigable = traversable().find(*navigable_id);
        if (!navigable.has_value()) {
            complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(error_code_for_a_navigable_that_is_gone(protocol), "Window not found"sv));
            return;
        }
        target = traversable().page_hosting(*navigable);
    }

    // No page holds the navigable's document between the displayed document's unload and the activation of the
    // document another page populated to replace it, so the command waits for that document.
    if (traversable().is_handing_navigable_to_another_page(*navigable)) {
        m_webdriver_commands_waiting_for_a_document.set(command_id, { navigable_id, name, move(payload), move(arguments), protocol });
        return;
    }

    if (!target || !target->is_open()) {
        complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(error_code_for_a_navigable_that_is_gone(protocol), "Window not found"sv));
        return;
    }

    if (protocol == WebDriverProtocol::BiDi) {
        m_pending_webdriver_commands.set(command_id, { *target, name, navigable_id, protocol });
        target->async_run_webdriver_bidi_command(command_id, *navigable_id, name, move(payload));
        return;
    }

    if (name == "crash_current_page"sv)
        m_pending_webdriver_crash_commands.set(command_id, { *target, name, navigable_id });
    else
        m_pending_webdriver_commands.set(command_id, { *target, name, navigable_id });
    target->async_run_webdriver_command(command_id, navigable_id, name, move(payload), move(arguments));
}

void ViewImplementation::run_webdriver_commands_waiting_for_a_document(Badge<CanonicalTraversable>)
{
    auto commands = move(m_webdriver_commands_waiting_for_a_document);
    for (auto& [command_id, command] : commands)
        dispatch_webdriver_content_command(command_id, command.navigable_id, command.name, move(command.payload), move(command.arguments), command.protocol);
}

void ViewImplementation::prepare_page_for_navigable(WebContentPage& page)
{
    page.async_set_zoom_level(m_zoom_level);
    if (m_window_position.has_value())
        page.async_set_window_position(m_window_position->to_type<Web::DevicePixels>());
    if (m_window_size.has_value())
        page.async_set_window_size(m_window_size->to_type<Web::DevicePixels>());
    page.async_set_maximum_frames_per_second(m_maximum_frames_per_second);
    if (m_frame_rate_capped)
        page.async_set_frame_rate_capped(true);
    page.async_set_viewport_corner_radius(m_viewport_corner_radius);
    page.async_set_has_focus(traversable().has_system_focus());
    if (auto focused_navigable_id = traversable().focused_navigable_id(); focused_navigable_id.has_value())
        page.async_set_focused_navigable(*focused_navigable_id);
    page.async_set_document_cookie_version_buffer(m_document_cookie_version_buffer);
    if (m_debugger_is_attached)
        page.async_attach_debugger();
    page.async_set_page_mute_state(m_mute_state);
    if (Application::browser_options().webdriver_browser_endpoint.has_value())
        Application::the().push_webdriver_session_config(page);
    Application::the().apply_view_options({}, *this, page);
    send_force_dark(page);
    send_preferences_to_page(page);
    if (auto screens = screen_rects(); screens.has_value())
        send_screen_rects_to_page(page, *screens);
}

void ViewImplementation::prepare_page_for_tab(WebContentPage& page)
{
    prepare_page_for_navigable(page);
    // The window handle names the tab to WebDriver, and the viewport is the tab's: both belong to the page displaying
    // it, whose traversable is local.
    page.async_set_window_handle(m_window_handle);
    page.async_set_viewport(viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
    Application::the().update_compositor_viewport(page.compositor_context_id(), viewport_size().to_type<int>(), m_device_pixel_ratio);
}

void ViewImplementation::did_change_display_page(Badge<CanonicalNavigable>, RefPtr<WebContentPage> previous_page)
{
    display_page_changed(move(previous_page));
}

// The tab is displayed by the page hosting its document from now on.
void ViewImplementation::display_page_changed(RefPtr<WebContentPage> previous_page)
{
    auto new_page_id = page().id();
    Vector<u64> stale_close_preflight_requests;
    for (auto const& [request_id, request] : m_pending_close_preflight_requests) {
        if (request.page_id != new_page_id)
            stale_close_preflight_requests.append(request_id);
    }
    for (auto request_id : stale_close_preflight_requests) {
        if (auto request = m_pending_close_preflight_requests.take(request_id); request.has_value())
            request->on_complete(false);
    }

    // Screenshot and page info requests went to the outgoing page, so only it could answer them.
    if (auto pending_screenshot = move(m_pending_screenshot))
        pending_screenshot->reject(Error::from_string_literal("The displayed page changed before taking the screenshot"));
    if (auto pending_info_request = move(m_pending_info_request))
        pending_info_request->reject(Error::from_string_literal("The displayed page changed before sending its info"));

    auto& page = this->page();
    if (previous_page) {
        // NB: The page has no hovered link and cannot clear the outgoing page's status label.
        if (on_link_unhover)
            on_link_unhover();
        if (m_debugger_paused) {
            set_debugger_paused(false);
            if (on_debugger_resumed)
                on_debugger_resumed();
        }
        m_debugger_overlay_pointer_state.reset();
        fail_pending_debugger_requests();
        reject_pending_selection_requests();
        reset_page_media_state();

        // Keep showing the outgoing page until the page paints its first frame.
        if (m_has_usable_bitmap) {
            m_backup_shared_image_buffer = move(m_front_bitmap.shared_image_buffer);
            m_backup_bitmap_size = m_front_bitmap.last_painted_size;
            m_backup_bitmap_device_pixel_ratio = m_front_bitmap.last_painted_device_pixel_ratio;
        }
        if (previous_page->is_open()) {
            Application::the().update_compositor_context_visibility(previous_page->compositor_context_id(), Web::HTML::VisibilityState::Hidden);
            previous_page->async_set_has_devtools_client(false);
        }

        auto pending_webdriver_commands = move(m_pending_webdriver_commands);
        auto pending_webdriver_crash_commands = move(m_pending_webdriver_crash_commands);
        complete_webdriver_content_commands_after_process_replacement(pending_webdriver_commands);
        complete_webdriver_content_commands_after_process_replacement(pending_webdriver_crash_commands);
    }
    m_front_bitmap = {};
    m_other_bitmaps.clear();
    m_has_usable_bitmap = false;
    if (auto backing_stores = page.take_presented_backing_stores(); backing_stores.has_value())
        install_backing_stores(move(backing_stores->bitmap_ids), move(backing_stores->backing_stores));

    auto compositor_context_id = page.compositor_context_id();
    Application::the().update_compositor_viewport(compositor_context_id, viewport_size().to_type<int>(), m_device_pixel_ratio);
    Application::the().update_compositor_context_visibility(compositor_context_id, traversable().system_visibility_state());
    page.async_update_visibility_state(traversable().id(), traversable().system_visibility_state());

    // An assistive technology's interest is per-page state in WebContent, and a cross-site navigation moves the display
    // page to another process. So ask the new page for its tree too, or the assistive technology keeps the outgoing
    // page's tree for good. Blink hands every new RenderFrameHost the AX mode (RenderFrameHostImpl::
    // UpdateAccessibilityMode), Gecko activates a11y in each content process it starts (ContentParent::SendActivateA11y),
    // and WebKit re-registers its accessibility tokens with the process that takes over the page (WebPageProxy::
    // registerUIProcessAccessibilityTokens).
    if (m_accessibility_tree_requested)
        page.async_request_accessibility_tree();

    // DevTools inspects the page that displays the tab, and no other.
    page.async_set_has_devtools_client(m_devtools_connected);
    handle_resize();
    update_ui_overlays();

    if (previous_page && &previous_page->client() != &page.client() && on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();
    dump_session_history("display-page-changed"sv);
}

void ViewImplementation::did_lose_page(Badge<CanonicalTraversable>, WebContentPage& page, WebContentProcessLost process_lost)
{
    bool lost_active_input_host = process_lost == WebContentProcessLost::Yes && !m_webdriver_input_commands.is_empty()
        && m_webdriver_input_commands.first().command.name != "element_click"sv
        && m_pending_webdriver_commands.contains(m_webdriver_input_commands.first().command_id)
        && m_pending_webdriver_commands.get(m_webdriver_input_commands.first().command_id)->page.ptr() == &page;
    if (lost_active_input_host) {
        m_webdriver_input_state = JsonValue {};
        m_webdriver_input_state_was_lost = true;
        auto queued = move(m_webdriver_input_commands);
        // Keep an executing command serialized until its host completes or is discarded.
        if (m_webdriver_input_command_running)
            m_webdriver_input_commands.append(queued.take_first());
        for (auto const& command : queued)
            Application::the().complete_webdriver_content_command(command.command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Input state was lost while waiting for input"sv));
        auto command_id = m_webdriver_input_commands.first().command_id;
        m_pending_webdriver_commands.remove(command_id);
        m_webdriver_commands_waiting_for_a_document.remove(command_id);
        complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Input host was lost while executing the command"sv));
    }
    Vector<u64> lost_close_preflight_requests;
    for (auto const& [request_id, request] : m_pending_close_preflight_requests) {
        if (request.page_id == page.id())
            lost_close_preflight_requests.append(request_id);
    }
    for (auto request_id : lost_close_preflight_requests) {
        if (auto request = m_pending_close_preflight_requests.take(request_id); request.has_value())
            request->on_complete(false);
    }

    // NB: The view fails the commands of its own page when it recovers from its process's crash.
    if (traversable().display_page() == &page)
        return;

    m_pending_webdriver_commands.remove_all_matching([&](u64 command_id, PendingWebDriverCommand const& command) {
        if (command.page.ptr() != &page)
            return false;
        if (process_lost == WebContentProcessLost::No && complete_webdriver_content_command_after_navigation(command_id, command))
            return true;
        complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(error_code_for_a_navigable_that_is_gone(command.protocol), "Browsing context was discarded while executing the command"sv));
        return true;
    });
}

void ViewImplementation::move_pending_webdriver_commands_to_new_host(Badge<CanonicalNavigable>, Web::HTML::CrossProcessId navigable_id, WebContentPage& old_host, WebContentPage& new_host)
{
    if (&old_host == &new_host || !new_host.is_open())
        return;

    for (auto& [command_id, command] : m_pending_webdriver_commands) {
        if (command.page.ptr() != &old_host || command.navigable_id != navigable_id)
            continue;
        if (!webdriver_content_command_outlives_its_page(command.name))
            continue;

        command.page = new_host;
        new_host.async_run_webdriver_command(command_id, navigable_id, "wait_for_navigation"_string, JsonValue {}, {});
    }
}

// https://w3c.github.io/webdriver/#dfn-element-click
// The click has been dispatched by the time the navigation it started takes the page away, so only step 11, waiting
// for that navigation to complete, remains.
bool ViewImplementation::webdriver_content_command_outlives_its_page(StringView name)
{
    // NB: Element Click only mutates a fresh temporary pointer source and removes that source and its cancel
    //     records before waiting for navigation. The canonical input state remains unchanged across this handoff.
    return name == "element_click"sv;
}

bool ViewImplementation::complete_webdriver_content_command_after_navigation(u64 command_id, PendingWebDriverCommand const& command)
{
    if (!webdriver_content_command_outlives_its_page(command.name))
        return false;

    wait_for_webdriver_navigation_completion(Application::the().webdriver_page_load_timeout(), [weak_this = make_weak_ptr(), command_id](Web::WebDriver::Response response) {
        if (!weak_this || weak_this->m_webdriver_input_commands.is_empty() || weak_this->m_webdriver_input_commands.first().command_id != command_id)
            return;
        weak_this->complete_webdriver_input_command(command_id, move(response));
    });
    return true;
}

void ViewImplementation::complete_webdriver_content_commands_after_process_replacement(HashMap<u64, PendingWebDriverCommand> const& commands)
{
    for (auto const& [command_id, command] : commands) {
        if (complete_webdriver_content_command_after_navigation(command_id, command))
            continue;

        complete_webdriver_input_command(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent was replaced while executing the command"sv));
    }
}

void ViewImplementation::did_set_webdriver_current_browsing_context(Badge<WebContentPage>, u64 command_id, Web::HTML::CrossProcessId navigable_id)
{
    if (!m_pending_webdriver_commands.contains(command_id))
        return;

    if (auto navigable = traversable().find(navigable_id); navigable.has_value())
        set_webdriver_current_browsing_context(*navigable);
}

void ViewImplementation::set_webdriver_current_browsing_context_to_top_level()
{
    set_webdriver_current_browsing_context(traversable());
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
    if (!m_webdriver_current_parent_navigable_id.has_value() || !traversable().find(*m_webdriver_current_parent_navigable_id).has_value()) {
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
        if (auto parent = weak_this->traversable().find(*weak_this->m_webdriver_current_parent_navigable_id); parent.has_value())
            weak_this->set_webdriver_current_browsing_context(*parent);

        // FIXME: 5. Update any implementation-specific state that would result from the user selecting session's current browsing context for interaction, without altering OS-level focus.

        // 6. Return success with data null.
        on_complete(JsonValue {});
    });
}

void ViewImplementation::did_complete_webdriver_content_command(WebContentPage& page, u64 command_id, Web::WebDriver::Response response)
{
    // NB: Only the page that runs a command completes it. Another page of this view, such as one hosting a cross-site
    //     frame, cannot.
    auto is_running_command = [&](auto const& commands) {
        auto command = commands.find(command_id);
        return command != commands.end() && command->value.page.ptr() == &page;
    };

    if (is_running_command(m_pending_webdriver_crash_commands)) {
        // WebContent acknowledges the command before its deferred process exit. Keep the command pending until the
        // crash handler has obtained the page standing in for the crashed document and started session-history recovery.
        if (response.is_error()) {
            m_pending_webdriver_crash_commands.remove(command_id);
            complete_webdriver_input_command(command_id, move(response));
        }
        return;
    }

    if (is_running_command(m_pending_webdriver_commands)) {
        m_pending_webdriver_commands.remove(command_id);
        complete_webdriver_input_command(command_id, move(response));
    }
}

void ViewImplementation::did_close_browsing_context(Badge<WebContentPage>)
{
    discard_webdriver_input_commands();
    reject_pending_selection_requests();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
    // 6. Invoke WebDriver BiDi navigable destroyed with traversable.
    auto* webdriver_bidi = WebDriverBiDi::the();
    if (webdriver_bidi && !m_window_handle.is_empty() && !m_owner_view_id.has_value())
        webdriver_bidi->navigable_destroyed(traversable());

    auto window_handle = move(m_window_handle);

    // Headless views retain their closed children. Remove the view from routing immediately so a command racing
    // with the close cannot reach the closed page.
    all_views().remove(m_view_id);
    traversable().discard_pending_host();
    traversable().discard_representing_pages();

    if (!window_handle.is_empty() && !m_owner_view_id.has_value()) {
        Application::the().notify_webdriver_window_closed(window_handle);
        if (webdriver_bidi)
            webdriver_bidi->top_level_traversable_did_close(window_handle);
    }

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while handling user prompts"sv));

    auto pending_commands = move(m_pending_webdriver_commands);
    fail_webdriver_content_commands_after_window_close(pending_commands);
    auto pending_crash_commands = move(m_pending_webdriver_crash_commands);
    fail_webdriver_content_commands_after_window_close(pending_crash_commands);
    auto commands_waiting_for_a_document = move(m_webdriver_commands_waiting_for_a_document);
    fail_webdriver_content_commands_after_window_close(commands_waiting_for_a_document);

    auto pending_navigation_completion_requests = move(m_pending_webdriver_navigation_completion_requests);
    for (auto& request : pending_navigation_completion_requests) {
        if (request.value->timer)
            request.value->timer->stop();
        request.value->on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window closed while waiting for navigation"sv));
    }
}

void ViewImplementation::run_webdriver_user_prompt_handling(Function<void(Web::WebDriver::Response)> on_complete)
{
    // The page standing in for the crashed document cannot have a user prompt belonging to it.
    if (m_crash_state.has_value()) {
        on_complete(JsonValue {});
        return;
    }

    auto request_id = m_next_webdriver_user_prompt_request_id++;
    m_pending_webdriver_user_prompt_requests.set(request_id, move(on_complete));
    page().async_run_webdriver_user_prompt_handling(request_id);
}

void ViewImplementation::did_complete_webdriver_user_prompt_handling(Badge<WebContentPage>, u64 request_id, Web::WebDriver::Response response)
{
    if (auto on_complete = m_pending_webdriver_user_prompt_requests.take(request_id); on_complete.has_value())
        on_complete.value()(move(response));
}

void ViewImplementation::did_open_user_prompt(Badge<WebContentPage>)
{
    complete_webdriver_navigation();
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
    auto navigation = traversable().prepare_navigation(url);
    prepare_for_navigation_after_crash(NavigationToRetry { url, prepare_navigation_to_retry(navigation) });
    traversable().begin_navigation(move(navigation));
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
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timer_interval), [this, request_id] {
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

void ViewImplementation::begin_webdriver_navigation(WebDriverNavigationCompletionSource completion_source, Optional<Web::HTML::CrossProcessId> history_operation_id)
{
    m_webdriver_navigation_observation = WebDriverNavigationObservation {
        .completion_source = completion_source,
        .history_operation_id = history_operation_id,
    };
}

void ViewImplementation::complete_webdriver_navigation()
{
    if (!m_webdriver_navigation_observation.has_value())
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
    if (!m_webdriver_navigation_observation.has_value() || m_webdriver_navigation_observation->history_operation_id != operation_id)
        return;

    auto& observation = *m_webdriver_navigation_observation;
    switch (observation.completion_source) {
    case WebDriverNavigationCompletionSource::HistoryTraversal:
        complete_webdriver_navigation();
        break;
    // A crash recovery is over once both its traversal and the load it starts are.
    case WebDriverNavigationCompletionSource::CrashRecovery:
        observation.history_operation_completed = true;
        if (observation.load_completed)
            complete_webdriver_navigation();
        break;
    case WebDriverNavigationCompletionSource::Load:
        break;
    }
}

JsonValue ViewImplementation::webdriver_session_history() const
{
    JsonObject serialized;
    serialized.set("currentURL"sv, url().serialize());
    serialized.set("displayedURL"sv, m_displayed_url.serialize());
    serialized.set("title"sv, m_title.to_utf8());
    serialized.set("webContentProcessID"sv, page().client().pid());
    serialized.set("backButtonEnabled"sv, m_navigate_back_action->enabled());
    serialized.set("forwardButtonEnabled"sv, m_navigate_forward_action->enabled());
    serialized.set("crashOverlayActive"sv, crash_overlay_active());
    serialized.set("hasOnlyTopLevelUsedSteps"sv, traversable().session_history().has_only_top_level_used_steps());

    serialized.set("currentUsedStepIndex"sv, traversable().session_history().current_used_step_index());

    if (auto traversal = traversable().browser_history_traversal_for_testing(); traversal.has_value()) {
        JsonObject pending_traversal;
        pending_traversal.set("targetStep"sv, traversal->target_step);
        pending_traversal.set("targetStepIndex"sv, traversal->target_step_index);
        pending_traversal.set("willChangeTopLevelEntry"sv, traversal->changes_top_level_entry);
        pending_traversal.set("stage"sv, CanonicalTraversable::browser_history_traversal_stage_to_string(traversal->stage));
        serialized.set("pendingSessionHistoryTraversal"sv, move(pending_traversal));
    } else {
        serialized.set("pendingSessionHistoryTraversal"sv, JsonValue {});
    }

    serialized.set("entries"sv, history_json_entries(traversable().session_history()));
    serialized.set("usedSteps"sv, history_json_steps(traversable().session_history()));
    return serialized;
}

String ViewImplementation::ui_process_session_history_for_testing(Badge<WebContentPage>) const
{
    return webdriver_session_history().serialized();
}

void ViewImplementation::update_navigation_action_state()
{
    auto effective_current_index = traversable().effective_current_session_history_step_index();
    m_navigate_back_action->set_enabled(effective_current_index > 0);
    m_navigate_forward_action->set_enabled(effective_current_index + 1 < traversable().session_history().used_step_count());
}

void ViewImplementation::recover_current_session_history_entry_with_history_operation()
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::CrashRecovery);
    traversable().recover_from_web_content_process_crash();
}

void ViewImplementation::reconstruct_current_session_history_entry_with_history_operation(StringView reason)
{
    m_history_visit_transition_for_next_load = HistoryVisitTransition::Restore;
    begin_webdriver_navigation(WebDriverNavigationCompletionSource::HistoryTraversal);
    traversable().reconstruct_the_history_to_step(traversable().session_history().current_step());
    dump_session_history(reason);
}

SessionHistorySnapshot ViewImplementation::session_history_snapshot() const
{
    auto const& session_history = traversable().session_history();
    return SessionHistorySnapshot {
        .entries = session_history.entries(),
        .used_steps = session_history.used_steps(),
        .current_used_step_index = session_history.current_used_step_index(),
    };
}

ErrorOr<void> ViewImplementation::restore_session_history_from_snapshot(SessionHistorySnapshot snapshot)
{
    TRY(traversable().restore_session_history_from_ui_snapshot(move(snapshot)));

    reconstruct_current_session_history_entry_with_history_operation("restored-session-history-from-ui-snapshot"sv);
    update_navigation_action_state();
    return {};
}

bool ViewImplementation::capture_session_history_snapshot_for_testing(Badge<WebContentPage>)
{
    m_captured_session_history_snapshot_for_testing = session_history_snapshot();
    return true;
}

bool ViewImplementation::restore_captured_session_history_snapshot_for_testing(Badge<WebContentPage>)
{
    if (!m_captured_session_history_snapshot_for_testing.has_value())
        return false;

    auto result = restore_session_history_from_snapshot(m_captured_session_history_snapshot_for_testing.release_value());
    return !result.is_error();
}

bool ViewImplementation::register_session_store_tab_for_testing(Badge<WebContentPage>)
{
    if (m_session_tab_id.has_value())
        return true;

    auto tab_id = m_session->session_store->tab_opened({ .window_id = {}, .initial_url = url(), .insertion_index = {}, .is_active = SessionStore::IsActive::No });
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

String ViewImplementation::session_store_tab_state_for_testing(Badge<WebContentPage>) const
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
    auto url = traversable().session_history().current_entry().url;
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
    traversable().append_history_queue_steps([this](NonnullRefPtr<Core::Promise<Empty>> promise) {
        m_pending_session_history_reset_queue_promise = move(promise);
        page().reset_session_history_for_testing();
    });
    return *m_pending_session_history_reset_for_testing;
}

void ViewImplementation::did_reset_session_history_for_testing(
    Badge<WebContentPage>, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    auto promise = move(m_pending_session_history_reset_for_testing);
    traversable().reset_session_history_for_testing(move(active_entry));
    m_webdriver_navigation_observation.clear();
    // The reset installed the process's own active entry as the canonical current entry.
    traversable().active_document().set_host(page());
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

    auto traversal = traversable().browser_history_traversal_for_testing();

    Optional<URL::URL> loading_url;
    if (auto navigation = traversable().uncommitted_navigation())
        loading_url = navigation->url;

    // The page displaying the tab can have lost its process, as when a crash recovery ends.
    auto const* connection = page().routed_connection();
    dbgln("[History] UI session history page={} pid={} reason={} url='{}' uncommitted_navigation={} loading_url={} pending_traversal_target={} pending_traversal_stage={} queued_same_document_entries={} back={} forward={} entries={}",
        page().id(),
        connection ? connection->pid() : -1,
        reason,
        url(),
        traversable().has_uncommitted_navigation(),
        loading_url,
        traversal.has_value() ? Optional<i32> { traversal->target_step } : Optional<i32> {},
        traversal.has_value() ? CanonicalTraversable::browser_history_traversal_stage_to_string(traversal->stage) : "none"sv,
        traversable().queued_same_document_session_history_entries_for_debug(),
        m_navigate_back_action->enabled(),
        m_navigate_forward_action->enabled(),
        history_log_entries(traversable().session_history()));
}

void ViewImplementation::handle_web_content_process_crash()
{
    auto failed_url = url();
    if (auto browser_ui_load_url = this->browser_ui_load_url(); browser_ui_load_url.has_value())
        failed_url = *browser_ui_load_url;
    Optional<NavigationToRetry> navigation_to_retry;
    if (traversable().session_history().current_entry().url != failed_url) {
        navigation_to_retry = NavigationToRetry { failed_url, {} };
        if (auto ongoing = navigation_to_retry_for_ongoing_navigation(); ongoing.has_value() && ongoing->url == failed_url)
            navigation_to_retry = ongoing.release_value();
    }

    reject_pending_selection_requests();
    // Nothing will finish the input events the crashed process still held, and the events another process holds
    // for this tab are stale once its tree is abandoned or restored.
    m_pending_input_events.clear();

    set_loading_state(false);
    traversable().clear_ongoing_navigation();
    traversable().clear_active_document_load();

    auto pending_user_prompt_requests = move(m_pending_webdriver_user_prompt_requests);
    for (auto& request : pending_user_prompt_requests)
        request.value(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while handling user prompts"sv));

    discard_webdriver_input_commands(true);
    auto pending_commands = move(m_pending_webdriver_commands);
    auto pending_crash_commands = move(m_pending_webdriver_crash_commands);
    for (auto const& command : pending_commands)
        complete_webdriver_input_command(command.key, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "WebContent crashed while executing the command"sv));
    if (auto* webdriver_bidi = WebDriverBiDi::the())
        webdriver_bidi->top_level_traversable_did_crash(traversable());

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

    if (recovery_mode == RecoveryMode::ShowOverlay)
        dbgln("\033[31;1mWebContent process crashed!\033[0m Last page loaded: {}", failed_url);

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

    respawn_web_content_process_after_crash();

    // A repeatedly crashing headless page is left dormant so the next WebDriver test or explicit navigation can
    // proceed without feeding an automatic restore loop.
    if (recovery_mode == RecoveryMode::Restore && !crashed_repeatedly) {
        recover_current_session_history_entry_with_history_operation();
    } else if (recovery_mode == RecoveryMode::ShowOverlay) {
        traversable().abandon_history_operations();
        set_crash_state(CrashState {
            .failed_url = move(failed_url),
            .navigation_to_retry = move(navigation_to_retry),
        });
    } else {
        traversable().abandon_history_operations();
    }

    for (auto const& command : pending_crash_commands)
        complete_webdriver_input_command(command.key, JsonValue {});
}

void ViewImplementation::respawn_web_content_process_after_crash()
{
    // Nothing answers the crashed page's requests.
    fail_pending_debugger_requests();
    reject_pending_selection_requests();
    // A queued session-history reset awaiting the crashed process's reply can never complete.
    if (auto queue_promise = move(m_pending_session_history_reset_queue_promise))
        queue_promise->resolve({});
    cancel_all_native_geolocation_requests();
    // Don't keep a stale backup bitmap around.
    m_backup_shared_image_buffer = nullptr;

    // The tab is displayed by a page of a new process, standing in for the document the crashed process destroyed
    // until a document activates in the tab: the traversal recovering the tab populates one in that page.
    // FIXME: Fail the tab, rather than crashing the whole application, if no process can be launched.
    auto page = MUST(traversable().obtain_page_to_host_traversable({}));
    traversable().set_page_standing_in_for_lost_document({}, page);
    display_page_changed({});
}

Optional<ViewImplementation::NavigationToRetry> ViewImplementation::navigation_to_retry_for_ongoing_navigation() const
{
    auto navigation = traversable().uncommitted_navigation();
    if (!navigation)
        navigation = traversable().navigation_being_finalized();
    if (!navigation)
        return {};
    return NavigationToRetry { navigation->url, navigation->retry };
}

void ViewImplementation::retry_navigation(NavigationToRetry retry, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (!retry.navigation.has_value()) {
        load(retry.url, history_handling);
        return;
    }
    auto navigation = retry.navigation.release_value();
    navigation.history_handling = history_handling;
    load(move(navigation));
}

void ViewImplementation::prepare_for_navigation_after_crash(Optional<NavigationToRetry> navigation_to_retry)
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
    notify_content_blocking_changed();
    update_report_website_issue_action();
    if (on_crash_overlay_state_change)
        on_crash_overlay_state_change(active);
}

// The report is written once the process has exited, which can be after this view was told the process was lost.
void ViewImplementation::did_save_crash_report(ByteString report_name)
{
    if (!m_crash_state.has_value())
        return;
    m_crash_state->report_name = move(report_name);
    if (on_crash_report_saved)
        on_crash_report_saved();
}

String ViewImplementation::crash_overlay_failed_url() const
{
    return m_crash_state.has_value() ? m_crash_state->failed_url.serialize() : url().serialize();
}

Optional<ByteString> ViewImplementation::crash_report_name() const
{
    if (!m_crash_state.has_value() || m_crash_state->report_name.is_empty())
        return {};
    return m_crash_state->report_name;
}

// The crash screen offers the failed URL for the user to include in the report; nothing is sent unless they do.
Optional<String> ViewImplementation::crash_report_website() const
{
    if (!m_crash_state.has_value())
        return {};
    auto const& failed_url = m_crash_state->failed_url;
    if (failed_url.scheme() != "http"sv && failed_url.scheme() != "https"sv)
        return {};
    auto serialized_url = failed_url.serialize();
    if (serialized_url.bytes().size() > CrashReportReview::maximum_url_bytes)
        return {};
    return serialized_url;
}

String ViewImplementation::current_host_for_settings() const
{
    auto url = this->url();
    if (url.host().has_value())
        return MUST(String::from_utf8(url.serialized_host()));
    if (url.scheme() == "about"sv)
        return MUST(String::from_utf8(url.serialize_path()));
    return {};
}

void ViewImplementation::languages_changed()
{
    auto const& languages = Application::settings().languages();
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_preferred_languages(languages);
    });
}

void ViewImplementation::content_settings_changed()
{
    apply_zoom_for_current_host();

    // The page displaying the tab, and any page chosen to host its next document, host the tab's traversable.
    send_force_dark(page());
    traversable().for_each_pending_host(send_force_dark);
}

void ViewImplementation::browsing_behavior_changed()
{
    traversable().for_each_hosting_page(send_browsing_behavior);
}

void ViewImplementation::autoplay_settings_changed()
{
    traversable().for_each_hosting_page(send_autoplay_settings);
}

void ViewImplementation::global_privacy_control_changed()
{
    traversable().for_each_hosting_page(send_global_privacy_control);
}

void ViewImplementation::geolocation_settings_changed()
{
    using ErrorCode = Web::Geolocation::GeolocationPositionErrorCode;

    if (Application::web_content_options().is_test_mode != IsTestMode::Yes && !Application::settings().geolocation_enabled()) {
        auto geolocation_position_request_ids = move(m_geolocation_position_request_ids);
        for (auto const& request : geolocation_position_request_ids) {
            Application::the().cancel_geolocation_position_request(request.value);
            request.key.page->async_geolocation_position_response(request.key.request_id, {}, to_underlying(ErrorCode::PermissionDenied));
        }

        auto geolocation_watch_ids = move(m_geolocation_watch_ids);
        for (auto const& watch : geolocation_watch_ids) {
            Application::the().stop_watching_geolocation_position(watch.value);
            watch.key.page->async_geolocation_position_response(watch.key.request_id, {}, to_underlying(ErrorCode::PermissionDenied));
        }
    }

    send_geolocation_emulated_position(page());
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (&page != &this->page())
            send_geolocation_emulated_position(page);
    });
}

void ViewImplementation::send_geolocation_emulated_position(WebContentPage& page)
{
    using ErrorCode = Web::Geolocation::GeolocationPositionErrorCode;

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
        if (m_has_usable_bitmap) {
            VERIFY(m_front_bitmap.shared_image_buffer);
            visible_bitmap = m_front_bitmap.shared_image_buffer->bitmap_if_present();
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
            page().async_take_document_screenshot();
        }
        break;
    }

    case ScreenshotType::Full:
        m_pending_screenshot = promise;
        page().async_take_document_screenshot();
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
    page().async_take_dom_node_screenshot(node_id);

    return promise;
}

void ViewImplementation::did_receive_screenshot(Badge<WebContentPage>, Gfx::ShareableBitmap const& screenshot)
{
    // A renderer answers a screenshot request; any other screenshot it sends is misbehavior.
    if (!m_pending_screenshot) {
        page().client().did_misbehave("did_take_screenshot"sv, "no screenshot was requested"sv);
        return;
    }

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
    page().async_request_internal_page_info(type);

    return promise;
}

void ViewImplementation::did_receive_internal_page_info(Badge<WebContentPage>, PageInfoType, Optional<Core::AnonymousBuffer> const& info)
{
    // A renderer answers a page info request; any other page info it sends is misbehavior.
    if (!m_pending_info_request) {
        page().client().did_misbehave("did_get_internal_page_info"sv, "no page info was requested"sv);
        return;
    }

    String info_string;
    if (!info.has_value()) {
        info_string = "(no page)"_string;
    } else {
        info_string = String::from_utf8_with_replacement_character(StringView { info->bytes() });
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
        auto const& search_engine = Application::settings().search_engine_settings().engine;
        if (!search_engine.has_value())
            return;

        auto url_string = search_engine->format_search_query_for_navigation(*m_search_text);
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
    });
    m_search_selected_text_action->set_visible(false);

    m_copy_link_to_highlight_action = Action::create("Copy Link to Highlight"sv, ActionID::CopyLinkToHighlight, [this] {
        if (!m_text_fragment_url.has_value())
            return;
        Application::the().insert_clipboard_entry({ "text/plain"_string, url_text_to_copy(*m_text_fragment_url) });
    });
    m_copy_link_to_highlight_action->set_visible(false);
    m_copy_link_to_highlight_action->set_enabled(false);

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

    m_report_website_issue_action = Action::create("Report Website Issue..."sv, ActionID::ReportWebsiteIssue, [this]() {
        report_website_issue();
    });
    m_report_website_issue_action->set_enabled(false);

    // A URL from the context menu is the page's — a link's or an image's — so it's navigated to from the page, unless
    // the user selected it as text. Blink also starts a selected URL with no initiator (IDC_CONTENT_CONTEXT_GOTOURL).
    m_open_in_new_tab_action = Action::create("Open in New Tab"sv, ActionID::OpenInNewTab, [this]() {
        if (m_context_menu_navigation.has_value())
            open_navigation_in_new_tab(*m_context_menu_navigation, Web::HTML::ActivateTab::No);
        else
            open_url_in_new_tab(m_context_menu_url, Web::HTML::ActivateTab::No);
    });
    if (m_is_private == IsPrivate::No) {
        m_open_in_new_window_action = Action::create("Open in New Window"sv, ActionID::OpenInNewWindow, [this]() {
            if (m_context_menu_navigation.has_value())
                open_navigation_in_new_window(*m_context_menu_navigation, IsPrivate::No);
            else
                open_url_in_new_window(m_context_menu_url, IsPrivate::No);
        });
    }
    if (application.supports_private_browsing_windows()) {
        m_open_in_new_private_window_action = Action::create("Open in New Private Window"sv, ActionID::OpenInNewPrivateWindow, [this]() {
            if (!m_context_menu_navigation.has_value()) {
                open_url_in_new_window(m_context_menu_url, IsPrivate::Yes);
                return;
            }
            // A private window still gets the page as the navigation's initiator, but never the page's URL as its
            // referrer — as in Gecko/Blink (Gecko URILoadingHelper, Blink IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD).
            auto navigation = *m_context_menu_navigation;
            navigation.referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::NoReferrer;
            open_navigation_in_new_window(move(navigation), IsPrivate::Yes);
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
        load(*m_context_menu_navigation);
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
        load(*m_context_menu_navigation);
    });
    m_open_video_action = Action::create("Open Video"sv, ActionID::OpenVideo, [this]() {
        load(*m_context_menu_navigation);
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
    m_media_enter_picture_in_picture_action = Action::create("Picture-in-Picture"sv, ActionID::EnterPictureInPicture, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_picture_in_picture_state(); });
    });
    m_media_exit_picture_in_picture_action = Action::create("Exit Picture-in-Picture"sv, ActionID::ExitPictureInPicture, [this]() {
        send_to_media_context_menu_page([](auto& page) { page.async_toggle_media_picture_in_picture_state(); });
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

    for (size_t index = 0; index < MAX_SPELLING_GUESSES; ++index) {
        auto action = Action::create(""sv, ActionID::SpellingGuess, [this, index] {
            if (index >= m_spelling_guesses.size() || !m_spelling_context_menu_page || !m_spelling_context_menu_page->is_open())
                return;
            m_spelling_context_menu_page->async_replace_context_menu_misspelling(m_spelling_guesses[index]);
        });
        action->set_visible(false);
        m_spelling_guess_actions.append(move(action));
    }
    m_no_spelling_guesses_action = Action::create("No Guesses Found"sv, ActionID::NoSpellingGuesses, [] { });
    m_no_spelling_guesses_action->set_enabled(false);
    m_no_spelling_guesses_action->set_visible(false);
    m_ignore_spelling_action = Action::create("Ignore Spelling"sv, ActionID::IgnoreSpelling, [this] { ignore_context_menu_misspelling(); });
    m_ignore_spelling_action->set_visible(false);
    m_learn_spelling_action = Action::create("Learn Spelling"sv, ActionID::LearnSpelling, [this] { learn_context_menu_misspelling(); });
    m_learn_spelling_action->set_visible(false);

    m_page_context_menu = Menu::create("Page Context Menu"sv);
    for (auto& action : m_spelling_guess_actions)
        m_page_context_menu->add_action(action);
    m_page_context_menu->add_action(*m_no_spelling_guesses_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_ignore_spelling_action);
    m_page_context_menu->add_action(*m_learn_spelling_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_look_up_selected_text_action);
    m_page_context_menu->add_action(*m_navigate_back_action);
    m_page_context_menu->add_action(*m_navigate_forward_action);
    m_page_context_menu->add_action(application.reload_action());
    m_page_context_menu->add_separator();
    add_text_edit_actions(*m_page_context_menu);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_search_selected_text_action);
    m_page_context_menu->add_action(*m_copy_link_to_highlight_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_take_visible_screenshot_action);
    m_page_context_menu->add_action(*m_take_full_screenshot_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.view_source_action());
    m_page_context_menu->add_action(*m_report_website_issue_action);

    m_link_context_menu = Menu::create("Link Context Menu"sv);
    add_open_url_actions(*m_link_context_menu);
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(*m_download_linked_file_action);
    m_link_context_menu->add_action(*m_download_linked_file_as_action);
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(*m_copy_url_action);
    m_link_context_menu->add_action(*m_copy_link_to_highlight_action);

    m_selected_text_link_context_menu = Menu::create("Selected Text Link Context Menu"sv);
    m_selected_text_link_context_menu->add_action(*m_look_up_selected_text_action);
    add_open_url_actions(*m_selected_text_link_context_menu);
    m_selected_text_link_context_menu->add_separator();
    add_text_edit_actions(*m_selected_text_link_context_menu);
    m_selected_text_link_context_menu->add_separator();
    m_selected_text_link_context_menu->add_action(*m_search_selected_text_action);
    m_selected_text_link_context_menu->add_action(*m_copy_link_to_highlight_action);

    m_image_context_menu = Menu::create("Image Context Menu"sv);
    m_image_context_menu->add_action(*m_open_in_new_tab_action);
    m_image_context_menu->add_action(*m_open_image_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_save_image_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_copy_image_action);
    m_image_context_menu->add_action(*m_copy_url_action);

    m_media_context_menu = Menu::create("Media Context Menu"sv);
    m_media_context_menu->add_action(*m_media_play_action);
    m_media_context_menu->add_action(*m_media_pause_action);
    m_media_context_menu->add_action(*m_media_mute_action);
    m_media_context_menu->add_action(*m_media_unmute_action);
    m_media_context_menu->add_action(*m_media_show_controls_action);
    m_media_context_menu->add_action(*m_media_hide_controls_action);
    m_media_context_menu->add_action(*m_media_loop_action);
    m_media_context_menu->add_action(*m_media_enter_fullscreen_action);
    m_media_context_menu->add_action(*m_media_exit_fullscreen_action);
    m_media_context_menu->add_action(*m_media_enter_picture_in_picture_action);
    m_media_context_menu->add_action(*m_media_exit_picture_in_picture_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_open_in_new_tab_action);
    m_media_context_menu->add_action(*m_open_audio_action);
    m_media_context_menu->add_action(*m_open_video_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_copy_url_action);

    auto add_bookmark_action = Action::create("Add Bookmark..."sv, ActionID::AddBookmark, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        application.display_add_bookmark_dialog(bookmark_id->target_folder_id)
            ->when_resolved([](Application::BookmarkDialogResult result) {
                auto& bookmark = result.data.get<BookmarkItem::Bookmark>();
                Application::bookmark_store().add_bookmark(move(bookmark.url), move(bookmark.title), move(bookmark.favicon_hash), result.target_folder_id);
            });
    });
    auto add_bookmark_folder_action = Action::create("Add Folder..."sv, ActionID::AddBookmarkFolder, []() {
        auto& application = Application::the();

        auto bookmark_id = application.bookmark_item_id_for_context_menu();
        if (!bookmark_id.has_value())
            return;

        application.display_add_bookmark_folder_dialog({}, bookmark_id->target_folder_id)
            ->when_resolved([](Application::BookmarkDialogResult result) {
                auto& folder = result.data.get<BookmarkItem::Folder>();
                Application::bookmark_store().add_folder(move(folder.title), result.target_folder_id);
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

        application.display_edit_bookmark_dialog(*current_bookmark, bookmark_id->parent_folder_id)
            ->when_resolved([bookmark_id = bookmark_id.release_value()](Application::BookmarkDialogResult result) {
                auto& store = Application::bookmark_store();
                auto& bookmark = result.data.get<BookmarkItem::Bookmark>();
                store.edit_bookmark(bookmark_id.id, move(bookmark.url), move(bookmark.title));
                if (bookmark_id.parent_folder_id != result.target_folder_id)
                    store.move_item(bookmark_id.id, result.target_folder_id, NumericLimits<size_t>::max());
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

        application.display_edit_bookmark_folder_dialog(*current_folder, bookmark_id->parent_folder_id)
            ->when_resolved([bookmark_id = bookmark_id.release_value()](Application::BookmarkDialogResult result) {
                auto& store = Application::bookmark_store();
                auto& folder = result.data.get<BookmarkItem::Folder>();
                store.edit_folder(bookmark_id.id, move(folder.title));
                if (bookmark_id.parent_folder_id != result.target_folder_id)
                    store.move_item(bookmark_id.id, result.target_folder_id, NumericLimits<size_t>::max());
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

SpellChecker::DocumentTag ViewImplementation::spell_document_tag()
{
    auto* spell_checker = Application::the().spell_checker();
    VERIFY(spell_checker);
    if (!m_spell_document_tag.has_value())
        m_spell_document_tag = spell_checker->create_document_tag();
    return *m_spell_document_tag;
}

void ViewImplementation::did_request_spell_check(Badge<WebContentPage>, WebContentPage& requesting_page, u64 request_id, Utf16String text)
{
    auto* spell_checker = Application::the().spell_checker();
    if (!spell_checker) {
        requesting_page.async_did_check_spelling(request_id, {});
        return;
    }
    spell_checker->check(spell_document_tag(), text, [page = NonnullRefPtr { requesting_page }, request_id](Vector<Web::SpellingError> errors) {
        if (page->is_open())
            page->async_did_check_spelling(request_id, move(errors));
    });
}

void ViewImplementation::did_request_writing_suggestion(Badge<WebContentPage>, WebContentPage& requesting_page, u64 request_id, Utf16String text, u32 caret_offset)
{
    auto* spell_checker = Application::the().spell_checker();
    if (!spell_checker) {
        requesting_page.async_did_get_writing_suggestion(request_id, {});
        return;
    }
    spell_checker->suggest(spell_document_tag(), text, caret_offset, [page = NonnullRefPtr { requesting_page }, request_id](Optional<Web::WritingSuggestion> suggestion) {
        if (page->is_open())
            page->async_did_get_writing_suggestion(request_id, move(suggestion));
    });
}

// Forgets what the user told the spellchecker to ignore or learn in this tab.
void ViewImplementation::reset_spell_checking()
{
    if (auto* spell_checker = Application::the().spell_checker(); spell_checker && m_spell_document_tag.has_value())
        spell_checker->close_document_tag(*m_spell_document_tag);
    m_spell_document_tag.clear();
    if (m_top_level_traversable) {
        traversable().for_each_hosting_page([](WebContentPage& page) {
            if (page.is_open())
                page.async_forget_accepted_spelling_words();
        });
    }
}

void ViewImplementation::update_spelling_menu_actions(Optional<Utf16String> misspelled_word)
{
    auto* spell_checker = Application::the().spell_checker();
    m_context_menu_misspelled_word = spell_checker ? move(misspelled_word) : OptionalNone {};
    m_spelling_guesses.clear();
    if (m_context_menu_misspelled_word.has_value())
        m_spelling_guesses = spell_checker->guesses(spell_document_tag(), *m_context_menu_misspelled_word);
    if (m_spelling_guesses.size() > MAX_SPELLING_GUESSES)
        m_spelling_guesses.shrink(MAX_SPELLING_GUESSES);

    auto shows_block = m_context_menu_misspelled_word.has_value();
    for (size_t index = 0; index < m_spelling_guess_actions.size(); ++index) {
        auto shows_guess = shows_block && index < m_spelling_guesses.size();
        if (shows_guess)
            m_spelling_guess_actions[index]->set_text(MUST(m_spelling_guesses[index].utf16_view().to_utf8()));
        m_spelling_guess_actions[index]->set_visible(shows_guess);
    }
    m_no_spelling_guesses_action->set_visible(shows_block && m_spelling_guesses.is_empty());
    m_ignore_spelling_action->set_visible(shows_block);
    m_learn_spelling_action->set_visible(shows_block);
}

void ViewImplementation::ignore_context_menu_misspelling()
{
    auto* spell_checker = Application::the().spell_checker();
    if (!spell_checker || !m_context_menu_misspelled_word.has_value())
        return;
    auto word = *m_context_menu_misspelled_word;
    spell_checker->ignore(spell_document_tag(), word);
    // An ignored word holds for this tab, so every page of the tab drops its markers for it.
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        page.async_accept_spelling_word(word);
    });
}

void ViewImplementation::learn_context_menu_misspelling()
{
    auto* spell_checker = Application::the().spell_checker();
    if (!spell_checker || !m_context_menu_misspelled_word.has_value())
        return;
    auto word = *m_context_menu_misspelled_word;
    spell_checker->learn(spell_document_tag(), word);
    // A learned word goes in the user's dictionary for every app, so every page of every tab drops its markers for it.
    // A spellchecker that learns for this tab's tag alone gets the word dropped from this tab alone.
    if (!spell_checker->learns_for_every_tag()) {
        traversable().for_each_hosting_page([&](WebContentPage& page) {
            page.async_accept_spelling_word(word);
        });
        return;
    }
    for_each_view([&](ViewImplementation& view) {
        if (view.m_top_level_traversable) {
            view.traversable().for_each_hosting_page([&](WebContentPage& page) {
                page.async_accept_spelling_word(word);
            });
        }
        return IterationDecision::Continue;
    });
}

String ViewImplementation::describe_spelling_menu_for_testing() const
{
    StringBuilder builder;
    auto append = [&](Action const& action) {
        if (!action.visible())
            return;
        if (!builder.is_empty())
            builder.append(" | "sv);
        builder.append(action.text());
    };
    for (auto const& action : m_spelling_guess_actions)
        append(action);
    append(*m_no_spelling_guesses_action);
    append(*m_ignore_spelling_action);
    append(*m_learn_spelling_action);
    if (builder.is_empty())
        return "(no spelling items)"_string;
    return MUST(builder.to_string());
}

void ViewImplementation::activate_spelling_menu_action_for_testing(Badge<WebContentPage>, WebContentPage& requesting_page, Web::SpellingMenuActionForTesting action, u32 guess_index)
{
    auto menu = describe_spelling_menu_for_testing();
    switch (action) {
    case Web::SpellingMenuActionForTesting::None:
        break;
    case Web::SpellingMenuActionForTesting::Guess:
        if (guess_index < m_spelling_guess_actions.size() && m_spelling_guess_actions[guess_index]->visible())
            m_spelling_guess_actions[guess_index]->activate();
        break;
    case Web::SpellingMenuActionForTesting::IgnoreSpelling:
        if (m_ignore_spelling_action->visible())
            m_ignore_spelling_action->activate();
        break;
    case Web::SpellingMenuActionForTesting::LearnSpelling:
        if (m_learn_spelling_action->visible())
            m_learn_spelling_action->activate();
        break;
    }
    // The page gets this after whatever the action sent it, on the same connection.
    requesting_page.async_did_complete_spelling_menu_action_for_testing(menu);
}

void ViewImplementation::request_text_fragment_url(Optional<u64> generation_id)
{
    m_text_fragment_url.clear();
    m_pending_text_fragment_url_request_id.clear();
    m_copy_link_to_highlight_action->set_enabled(false);
    m_copy_link_to_highlight_action->set_visible(generation_id.has_value());
    if (!generation_id.has_value())
        return;

    auto request_id = m_next_text_fragment_url_request_id++;
    m_pending_text_fragment_url_request_id = request_id;
    page().client().async_generate_text_fragment_url(page().id(), request_id, *generation_id);
}

void ViewImplementation::did_generate_text_fragment_url(u64 request_id, Optional<URL::URL> url)
{
    if (!m_pending_text_fragment_url_request_id.has_value() || *m_pending_text_fragment_url_request_id != request_id)
        return;
    m_pending_text_fragment_url_request_id.clear();
    m_text_fragment_url = move(url);
    m_copy_link_to_highlight_action->set_enabled(m_text_fragment_url.has_value());
}

void ViewImplementation::did_request_page_context_menu(Badge<WebContentPage>, WebContentPage& requesting_page, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target, Optional<Utf16String> misspelled_word, Optional<u64> text_fragment_generation_id)
{
    request_text_fragment_url(text_fragment_generation_id);
    m_spelling_context_menu_page = requesting_page;
    update_spelling_menu_actions(move(misspelled_word));

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

            auto const& search_engine = Application::settings().search_engine_settings().engine;
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
                weak_this->set_context_menu_url_actions_enabled(true);
                weak_this->m_context_menu_url = selected_text_url.release_value();
                weak_this->m_context_menu_navigation.clear();
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

void ViewImplementation::did_request_link_context_menu(Badge<WebContentPage>, Gfx::IntPoint content_position, Web::HTML::PreparedNavigationDescriptor navigation, Optional<u64> text_fragment_generation_id)
{
    request_text_fragment_url(text_fragment_generation_id);
    ++m_context_menu_request_id;

    set_context_menu_url_actions_enabled(true);
    m_context_menu_url = navigation.url;
    m_context_menu_navigation = move(navigation);

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

    // NB: A file at a path the user chose was there when the user chose to replace it, as saving asks before replacing.
    auto may_replace_destination = prompt_for_path == PromptForPath::Yes && FileSystem::exists(download_path.value().string())
        ? FileDownloader::MayReplaceDestination::Yes
        : FileDownloader::MayReplaceDestination::No;
    Application::the().file_downloader().download_file(is_private(), m_context_menu_url, download_path.release_value(), may_replace_destination);
}

void ViewImplementation::did_request_image_context_menu(Badge<WebContentPage>, Gfx::IntPoint content_position, Web::HTML::PreparedNavigationDescriptor navigation, Optional<Gfx::ShareableBitmap> bitmap)
{
    ++m_context_menu_request_id;

    set_context_menu_url_actions_enabled(true);
    m_context_menu_url = navigation.url;
    m_context_menu_navigation = move(navigation);
    m_image_context_menu_bitmap = move(bitmap);

    m_open_in_new_tab_action->set_text("Open Image in New Tab"sv);
    m_copy_url_action->set_text("Copy Image URL"sv);

    m_copy_image_action->set_enabled(m_image_context_menu_bitmap.has_value());

    if (m_image_context_menu->on_activation)
        m_image_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::send_to_media_context_menu_page(Function<void(WebContentPage&)> const& send)
{
    NonnullRefPtr<WebContentPage> target = m_media_context_menu_page ? *m_media_context_menu_page : page();
    if (target->is_open())
        send(target);
}

// The context menus share these actions, and only a media element can leave them without a URL to act on.
void ViewImplementation::set_context_menu_url_actions_enabled(bool enabled)
{
    m_open_in_new_tab_action->set_enabled(enabled);
    m_copy_url_action->set_enabled(enabled);
    m_open_audio_action->set_enabled(enabled);
    m_open_video_action->set_enabled(enabled);
}

void ViewImplementation::did_request_media_context_menu(Badge<WebContentPage>, WebContentPage& requesting_page, Gfx::IntPoint content_position, Web::MediaContextMenu menu, Optional<Web::HTML::PreparedNavigationDescriptor> navigation)
{
    m_media_context_menu_page = requesting_page;
    ++m_context_menu_request_id;

    set_context_menu_url_actions_enabled(menu.media_url.has_value());
    m_context_menu_url = move(menu.media_url).value_or({});
    m_context_menu_navigation = move(navigation);

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
    m_media_enter_picture_in_picture_action->set_visible(menu.can_enter_picture_in_picture && !menu.is_picture_in_picture);
    m_media_exit_picture_in_picture_action->set_visible(menu.is_picture_in_picture);

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
    if (m_debugger_paused) {
        resume_debugger(DebuggerResumeMode::Continue);
        set_debugger_paused(false);
    }

    if (needs_beforeunload_check()) {
        page().async_request_close();
        return;
    }

    page().request_close();
}

void ViewImplementation::force_close()
{
    page().async_force_close();
}

void ViewImplementation::request_close_preflight(Function<void(bool)> on_complete)
{
    auto request_id = m_next_close_preflight_request_id++;
    auto page_id = page().id();
    m_pending_close_preflight_requests.set(request_id, { page_id, move(on_complete) });
    page().async_request_close_preflight(request_id);
}

void ViewImplementation::did_complete_close_preflight(Badge<WebContentPage>, u64 request_id, bool approved)
{
    if (auto request = m_pending_close_preflight_requests.take(request_id); request.has_value())
        request->on_complete(approved);
}

Function<void()> ViewImplementation::prepare_for_close_without_prompting()
{
    if (m_debugger_paused) {
        resume_debugger(DebuggerResumeMode::Continue);
        set_debugger_paused(false);
    }

    NonnullRefPtr closing_page = page();
    closing_page->set_detached_close_pending(true);
    return [closing_page = move(closing_page)] {
        closing_page->async_request_close_without_prompting();
    };
}

Function<void()> ViewImplementation::prepare_for_immediate_close()
{
    VERIFY(!needs_beforeunload_check());

    if (m_debugger_paused) {
        resume_debugger(DebuggerResumeMode::Continue);
        set_debugger_paused(false);
    }

    NonnullRefPtr closing_page = page();
    closing_page->set_detached_close_pending(true);
    return [closing_page = move(closing_page)] {
        closing_page->async_request_close();
    };
}

}
