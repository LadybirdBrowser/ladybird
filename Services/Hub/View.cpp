/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/View.h>
#include <LibWebView/PageTranslation.h>

namespace Hub {

// The backing stores of a view, as far as the view needs to know. The UI holds the images and shows them, and hears of
// each step so that it comes to the same result.
class BackingStoresInUI final : public WebView::PageBackingStores {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BackingStoresInUI(View& view)
        : m_view(view)
    {
    }

private:
    virtual void install(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override
    {
        track_installed_backing_stores(bitmap_ids);
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_install_backing_stores(m_view.view_id(), m_view.page().id().value(), move(bitmap_ids), move(backing_stores));
    }

    virtual void add(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override
    {
        track_added_backing_stores(bitmap_ids);
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_add_backing_stores(m_view.view_id(), move(bitmap_ids), move(backing_stores));
    }

    virtual void retire(ReadonlySpan<i32> bitmap_ids) override
    {
        PageBackingStores::retire(bitmap_ids);
        if (auto* ui = ConnectionFromUI::the()) {
            Vector<i32> retired_bitmap_ids;
            retired_bitmap_ids.append(bitmap_ids.data(), bitmap_ids.size());
            ui->async_view_retire_backing_stores(m_view.view_id(), move(retired_bitmap_ids));
        }
    }

    virtual PresentationResult present(i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio) override
    {
        auto result = PageBackingStores::present(bitmap_id, size, device_pixel_ratio);
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_present_backing_store(m_view.view_id(), m_view.page().id().value(), bitmap_id, size, device_pixel_ratio);
        return result;
    }

    virtual void keep_front_as_backup() override
    {
        PageBackingStores::keep_front_as_backup();
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_keep_front_backing_store_as_backup(m_view.view_id());
    }

    virtual void clear_backup() override
    {
        PageBackingStores::clear_backup();
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_clear_backup_backing_store(m_view.view_id());
    }

    virtual void clear() override
    {
        PageBackingStores::clear();
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_clear_backing_stores(m_view.view_id());
    }

    View& m_view;
};

View::View(InitialState initial_state)
    : ViewImplementation(initial_state.is_private)
    , m_system_theme(move(initial_state.system_theme))
    , m_screen_rects(move(initial_state.screen_rects))
    , m_websites_prefer_dark(initial_state.websites_prefer_dark)
{
    m_device_pixel_ratio = initial_state.device_pixel_ratio;
    m_maximum_frames_per_second = initial_state.maximum_frames_per_second;
    m_display_id = initial_state.display_id;
    m_backing_stores = make<BackingStoresInUI>(*this);
    forward_callbacks_to_ui(initial_state.is_tab);

    if (initial_state.owner_view_id.has_value()) {
        if (auto owner_view = find_view_by_id(*initial_state.owner_view_id); owner_view.has_value())
            set_owner_view(*owner_view);
    }
    set_page_background_color_to_system_canvas(m_websites_prefer_dark);
}

void View::initialize(Optional<WebView::CanonicalTraversable&> traversable)
{
    initialize_tab(Web::HTML::VisibilityState::Hidden, traversable);
}

View::InitialState View::initial_state_for_related_view() const
{
    return {
        .is_private = is_private(),
        .device_pixel_ratio = device_pixel_ratio(),
        .maximum_frames_per_second = maximum_frames_per_second(),
        .display_id = display_id(),
        .system_theme = m_system_theme,
        .screen_rects = m_screen_rects,
        .owner_view_id = {},
        .websites_prefer_dark = m_websites_prefer_dark,
        .is_tab = true,
    };
}

Gfx::IntSize View::screen_size() const
{
    if (!m_screen_rects.has_value() || m_screen_rects->main_screen_index >= m_screen_rects->available_rects.size())
        return m_viewport_size.to_type<int>();
    return m_screen_rects->available_rects[m_screen_rects->main_screen_index].size().to_type<int>();
}

void View::set_viewport(Web::DevicePixelSize size, double device_pixel_ratio)
{
    m_viewport_size = size;
    m_device_pixel_ratio = device_pixel_ratio;
    handle_resize();
}

void View::set_system_theme(Core::AnonymousBuffer theme)
{
    m_system_theme = move(theme);
    update_system_theme(m_system_theme);
}

void View::set_screen_rects(ScreenRects screen_rects)
{
    m_screen_rects = move(screen_rects);
    update_screen_rects();
}

void View::did_get_answer(u64 answer_id, bool answer)
{
    if (auto on_answer = m_pending_answers.take(answer_id); on_answer.has_value())
        (*on_answer)(answer);
}

void View::did_get_permission_answer(u64 answer_id, Web::Permissions::PromptOutcome outcome)
{
    if (auto on_answer = m_pending_permission_answers.take(answer_id); on_answer.has_value())
        (*on_answer)(outcome);
}

void View::forward_callbacks_to_ui(bool is_tab)
{
    on_favicon_change = [this](Optional<Gfx::Bitmap const&> favicon) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_favicon_change(view_id(), favicon.map([](auto const& bitmap) { return bitmap.to_shareable_bitmap(); }));
        }
    };

#if defined(AK_OS_MACOS)
    on_request_dictionary_lookup = [this](WebView::DictionaryLookup const& lookup, Gfx::IntPoint position) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_dictionary_lookup(view_id(), lookup, position);
    };
#endif

    add_navigation_listener({
        .on_load_start = [this](URL::URL const& url) {
            if (auto* ui = ConnectionFromUI::the()) {
                push_state_to_ui();
                ui->async_view_navigation_load_start(view_id(), url);
            } },
        .on_load_finish = [this](URL::URL const& url) {
            if (auto* ui = ConnectionFromUI::the()) {
                push_state_to_ui();
                ui->async_view_navigation_load_finish(view_id(), url);
            } },
    });

    // NB: What the view does differs on whether it has these, so a view that the UI will not answer for must not.
    if (is_tab) {
        on_request_before_unload = [this](String const& source, Function<void(bool)> on_complete) {
            auto* ui = ConnectionFromUI::the();
            if (!ui) {
                on_complete(true);
                return;
            }
            auto answer_id = m_next_answer_id++;
            m_pending_answers.set(answer_id, move(on_complete));
            push_state_to_ui();
            ui->async_view_request_before_unload(view_id(), answer_id, source);
        };

        on_request_external_url_confirmation = [this](URL::URL const& url, URL::Origin const& initiator_origin, WebView::ExternalURLHandler const& handler, Function<void(bool)> on_complete) {
            auto* ui = ConnectionFromUI::the();
            if (!ui) {
                on_complete(false);
                return;
            }
            auto answer_id = m_next_answer_id++;
            m_pending_answers.set(answer_id, move(on_complete));
            push_state_to_ui();
            ui->async_view_request_external_url_confirmation(view_id(), answer_id, url, initiator_origin, handler.application_name());
        };

        on_request_permission = [this](WebView::PermissionPromptRequest const& request, WebView::PermissionPromptCallback on_outcome) {
            auto* ui = ConnectionFromUI::the();
            if (!ui) {
                on_outcome(Web::Permissions::PromptOutcome::Dismissed);
                return;
            }
            auto answer_id = m_next_answer_id++;
            m_pending_permission_answers.set(answer_id, move(on_outcome));
            push_state_to_ui();
            ui->async_view_request_permission(view_id(), answer_id, request);
        };

        on_request_date_picker = [this](auto const&... arguments) {
            if (auto* ui = ConnectionFromUI::the()) {
                push_state_to_ui();
                ui->async_view_request_date_picker(view_id(), arguments...);
            }
        };

        on_new_web_view = [this](Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, WebView::CanonicalTraversable& traversable) {
            auto& view = Application::the().create_view(initial_state_for_related_view(), traversable);
            if (auto* ui = ConnectionFromUI::the()) {
                push_state_to_ui();
                ui->async_view_open_new_view(view_id(), view.view_id(), view.current_state(), activate_tab, hints);
            }
            return view.handle();
        };
    }

    on_finish_handling_key_event = [this](Web::KeyEvent const& event) {
        // NB: Input that the Hub came up with itself, such as WebDriver's, is nothing the UI knows about.
        if (!event.browser_data)
            return;
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_finish_handling_key_event(view_id(), ConnectionFromUI::input_id_of(*event.browser_data), event.clone_without_browser_data());
        }
    };

    on_finish_handling_mouse_event = [this](Web::MouseEvent const& event, Web::EventResult result) {
        if (!event.browser_data)
            return;
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_finish_handling_mouse_event(view_id(), ConnectionFromUI::input_id_of(*event.browser_data), event.clone_without_browser_data(), result);
        }
    };

    on_finish_handling_drag_event = [this](Web::DragEvent const& event) {
        // NB: Input that the Hub came up with itself, such as WebDriver's, is nothing the UI knows about.
        if (!event.browser_data)
            return;
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_finish_handling_drag_event(view_id(), ConnectionFromUI::input_id_of(*event.browser_data), event.clone_without_browser_data());
        }
    };

    on_load_start = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_load_start(view_id());
        }
    };

    on_load_finish = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_load_finish(view_id(), arguments...);
        }
    };

    on_activate_tab = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_activate_tab(view_id());
        }
    };

    on_close = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_close(view_id());
        }
    };

    on_link_hover = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_link_hover(view_id(), arguments...);
        }
    };

    on_link_unhover = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_link_unhover(view_id());
        }
    };

    on_title_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_title_change(view_id(), arguments...);
        }
    };

    on_url_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_url_change(view_id(), arguments...);
        }
    };

    on_loading_state_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_loading_state_change(view_id(), arguments...);
        }
    };

    on_top_level_navigation_commit = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_top_level_navigation_commit(view_id());
        }
    };

    on_browser_history_traversal_complete = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_browser_history_traversal_complete(view_id());
        }
    };

    on_cursor_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_cursor_change(view_id(), arguments...);
        }
    };

    on_input_method_state_change = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_input_method_state_change(view_id());
        }
    };

    on_enter_tooltip_area = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_enter_tooltip_area(view_id(), arguments...);
        }
    };

    on_leave_tooltip_area = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_leave_tooltip_area(view_id());
        }
    };

    on_content_blocking_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_content_blocking_change(view_id(), arguments...);
        }
    };

    on_request_alert = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_alert(view_id(), arguments...);
        }
    };

    on_request_confirm = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_confirm(view_id(), arguments...);
        }
    };

    on_request_prompt = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_prompt(view_id(), arguments...);
        }
    };

    on_request_set_prompt_text = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_set_prompt_text(view_id(), arguments...);
        }
    };

    on_before_browser_initiated_navigation = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_before_browser_initiated_navigation(view_id());
        }
    };

    on_request_accept_dialog = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_accept_dialog(view_id());
        }
    };

    on_request_dismiss_dialog = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_dismiss_dialog(view_id());
        }
    };

    on_request_website_issue_report = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_website_issue_report(view_id());
        }
    };

    on_request_screenshot = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_screenshot(view_id());
        }
    };

    on_accessibility_tree_received = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_accessibility_tree_received(view_id(), arguments...);
        }
    };

    on_accessibility_tree_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_accessibility_tree_changed(view_id(), arguments...);
        }
    };

    on_accessibility_focus_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_accessibility_focus_changed(view_id(), arguments...);
        }
    };

    on_restore_window = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_restore_window(view_id());
        }
    };

    on_reposition_window = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_reposition_window(view_id(), arguments...);
        }
    };

    on_resize_window = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_resize_window(view_id(), arguments...);
        }
    };

    on_maximize_window = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_maximize_window(view_id());
        }
    };

    on_minimize_window = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_minimize_window(view_id());
        }
    };

    on_fullscreen_window = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_fullscreen_window(view_id());
        }
    };

    on_exit_fullscreen_window = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_exit_fullscreen_window(view_id());
        }
    };

    on_request_color_picker = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_color_picker(view_id(), arguments...);
        }
    };

    on_cancel_permission_prompt = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_cancel_permission_prompt(view_id(), arguments...);
        }
    };

    on_tab_indicator_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_tab_indicator_change(view_id(), arguments...);
        }
    };

    on_request_file_picker = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_file_picker(view_id(), arguments...);
        }
    };

    on_request_select_dropdown = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_request_select_dropdown(view_id(), arguments...);
        }
    };

    on_picture_in_picture_control_activated = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_picture_in_picture_control_activated(view_id(), arguments...);
        }
    };

    on_find_in_page = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_find_in_page(view_id(), arguments...);
        }
    };

    on_page_background_color_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_page_background_color_change(view_id(), arguments...);
        }
    };

    on_screen_wake_lock_state_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_screen_wake_lock_state_changed(view_id(), arguments...);
        }
    };

    on_web_content_crashed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_web_content_crashed(view_id(), arguments...);
        }
    };

    on_crash_overlay_state_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_crash_overlay_state_change(view_id(), arguments...);
        }
    };

    on_crash_report_saved = [this] {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_crash_report_saved(view_id());
        }
    };

    on_performance_stats = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the()) {
            push_state_to_ui();
            ui->async_view_performance_stats(view_id(), arguments...);
        }
    };
}

ViewState View::current_state() const
{
    ViewState state;
    state.url = url();
    state.displayed_url = displayed_url();
    state.title = title();
    state.handle = handle();
    state.is_loading = is_loading();
    state.zoom_level = zoom_level();
    state.can_navigate_back = can_navigate_back();
    state.can_navigate_forward = can_navigate_forward();
    state.is_node_picker_active = is_node_picker_active();
    if (has_traversable()) {
        auto& translation = const_cast<View&>(*this).page_translation();
        state.is_translated = translation.is_translated();
        state.translation_offered = translation.is_offered();
        for (auto const& target : translation.target_languages()) {
            state.translation_targets.append({
                .language = target.language,
                .needs_download = target.needs_download,
                .faster_after_download = target.faster_after_download,
                .higher_quality_available = target.higher_quality_available,
                .downloading = target.downloading,
                .languages_to_download = target.languages_to_download,
            });
        }
        state.translation_target_language = translation.target_language();
        state.translation_prefers_higher_quality = Application::settings().translation_prefers_higher_quality();
    }
    state.input_method_state = input_method_state();
    state.page_mute_state = page_mute_state();
    state.plays_audio_indicator_visible = tab_indicator_visible(WebView::TabIndicator::PlayingAudio);
    state.uses_location_indicator_visible = tab_indicator_visible(WebView::TabIndicator::UsingLocation);
    state.uses_microphone_indicator_visible = tab_indicator_visible(WebView::TabIndicator::UsingMicrophone);
    state.page_background_color = page_background_color();
    state.crash_overlay_active = crash_overlay_active();
    state.crash_report_name = crash_report_name();
    state.crash_report_website = crash_report_website();
    state.crash_overlay_failed_url = crash_overlay_failed_url();
    state.host_for_settings = current_host_for_settings();
    state.can_report_website_issue = can_report_website_issue();
    state.can_undo = can_undo();
    state.can_redo = can_redo();
    state.permission_origin = permission_origin();
    if (has_traversable()) {
        state.content_blocking_snapshot = content_blocking_snapshot();
        state.site_permission_rows = site_permission_rows();
        state.can_view_source = can_view_source();
        if (auto page = traversable().display_page(); page && page->is_open())
            state.web_content_pid = web_content_pid();
    }
    return state;
}

void View::push_state_to_ui()
{
    auto* ui = ConnectionFromUI::the();
    if (!ui || !has_traversable())
        return;

    auto state = current_state();

    // Most of what a view reports changes nothing about the state, so only say something if it changed.
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(state));
    auto encoded_state = MUST(ByteBuffer::copy(buffer.data().span()));
    if (encoded_state == m_last_state_pushed_to_ui)
        return;
    m_last_state_pushed_to_ui = move(encoded_state);

    ui->async_view_state_changed(view_id(), move(state));
}

void View::did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_accept_presented_backing_store(view_id(), bitmap_id, damage_rect);
}

void View::prepare_page_for_navigable(WebView::WebContentPage& page)
{
    ViewImplementation::prepare_page_for_navigable(page);

    update_compositor_display_metadata(page);
}

void View::update_zoom()
{
    ViewImplementation::update_zoom();
    push_state_to_ui();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_update_zoom(view_id());
}

void View::read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)> on_read) const
{
    WebView::Application::the().read_clipboard_item(move(on_read));
}

void View::did_request_context_menu(WebView::ContextMenuRequest request)
{
    if (auto* ui = ConnectionFromUI::the()) {
        push_state_to_ui();
        ui->async_view_request_context_menu(view_id(), move(request));
    }
}

void View::did_make_link_to_highlight(Optional<URL::URL> url)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_did_make_link_to_highlight(view_id(), move(url));
}

void View::did_change_session_history(WebView::SessionHistorySnapshot const& history, URL::URL const& url)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_session_history_changed(view_id(), history, url);
}

void View::did_visit_page(URL::URL const& url, Optional<String> const& title, WebView::HistoryVisitTransition transition)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_visited_page(view_id(), url, title, transition);
}

void View::did_change_visited_page_title(URL::URL const& url, String const& title)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_view_visited_page_title_change(view_id(), url, title);
}

}
