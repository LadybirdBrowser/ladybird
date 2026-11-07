/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
#    include <Hub/Application.h>
#    include <LibWebView/PageTranslation.h>
#endif
#include <LibURL/InternalURLs.h>
#include <LibUnicode/DisplayNames.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/Translator.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/ContextMenus.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

static HashMap<u64, RemoteView*> s_views;

RemoteView::RemoteView(Optional<u64> view_id, Hub::View::InitialState initial_state)
    : BookmarkStoreObserver(Application::bookmark_store())
    , m_is_private(initial_state.is_private)
    , m_device_pixel_ratio(initial_state.device_pixel_ratio)
    , m_display_id(initial_state.display_id)
{
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    if (view_id.has_value()) {
        m_view_id = *view_id;
        m_state = HubConnection::the().view_description(*view_id);
    } else {
        auto view = HubConnection::the().create_view({
            .is_private = initial_state.is_private,
            .device_pixel_ratio = initial_state.device_pixel_ratio,
            .maximum_frames_per_second = initial_state.maximum_frames_per_second,
            .display_id = initial_state.display_id,
            .system_theme = move(initial_state.system_theme),
            .screen_rects = move(initial_state.screen_rects),
            .owner_view_id = initial_state.owner_view_id,
            .websites_prefer_dark = initial_state.websites_prefer_dark,
            .is_tab = initial_state.is_tab,
        });
        m_view_id = view.view_id();
        m_state = view.take_state();
    }
#else
    if (view_id.has_value()) {
        auto view = WebView::ViewImplementation::find_view_by_id(*view_id);
        VERIFY(view.has_value());
        m_view = static_cast<Hub::View*>(&view.value());
    } else {
        m_view = &Hub::Application::the().create_view(move(initial_state));
    }
    m_view_id = m_view->view_id();
    m_state = m_view->current_state();
#endif

    m_navigate_back_action = WebView::Action::create("Back"sv, WebView::ActionID::NavigateBack, [this] {
        traverse_the_history_by_delta(-1);
    });
    m_navigate_forward_action = WebView::Action::create("Forward"sv, WebView::ActionID::NavigateForward, [this] {
        traverse_the_history_by_delta(+1);
    });
    m_toggle_bookmark_action = WebView::Action::create("Toggle Bookmark"sv, WebView::ActionID::ToggleBookmarkViaToolbar, [] {
        Application::the().show_bookmark_popover();
    });
    m_reset_zoom_action = WebView::Action::create("100%"sv, WebView::ActionID::ResetZoomViaToolbar, [this] {
        reset_zoom();
    });
    m_reset_zoom_action->set_tooltip("Reset zoom level"sv);
    m_copy_link_to_highlight_action = WebView::Action::create("Copy Link to Highlight"sv, WebView::ActionID::CopyLinkToHighlight, [this] {
        if (m_link_to_highlight.has_value())
            Application::the().set_clipboard_text(MUST(String::from_byte_string(WebView::url_text_to_copy(*m_link_to_highlight))));
    });
    create_translation_actions();

    update_view_actions();
    update_bookmark_action();

    s_views.set(m_view_id, this);
}

RemoteView::~RemoteView()
{
    s_views.remove(m_view_id);
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_destroy_view(m_view_id);

    // NB: The Hub answers nothing for a view that is gone, so whoever still waits on this one is answered here, once
    //     this view is fully gone. A gone view stands in the way of no close.
    if (m_pending_close_preflights.is_empty() && m_pending_selected_text.is_empty() && m_pending_page_dumps.is_empty())
        return;
    Core::deferred_invoke([close_preflights = move(m_pending_close_preflights), selected_text = move(m_pending_selected_text), page_dumps = move(m_pending_page_dumps)]() mutable {
        for (auto& it : close_preflights)
            it.value(true);
        for (auto& it : selected_text)
            it.value->reject(Error::from_errno(ECANCELED));
        for (auto& it : page_dumps)
            it.value->reject(Error::from_errno(ECANCELED));
    });
}

void RemoteView::update_bookmark_action()
{
    auto is_bookmarked = Application::bookmark_store().is_bookmarked(url());
    m_toggle_bookmark_action->set_tooltip(is_bookmarked ? "Remove bookmark"sv : "Bookmark this page"sv);
    m_toggle_bookmark_action->set_engaged(is_bookmarked);
    Application::bookmarks().update_toggle_bookmark_action();
}

void RemoteView::update_screen_rects()
{
    if (auto rects = screen_rects(); rects.has_value())
        HubConnection::the().async_view_set_screen_rects(view_id(), rects.release_value());
}

void RemoteView::set_device_pixel_ratio_and_viewport(double device_pixel_ratio, Web::DevicePixelSize size)
{
    m_device_pixel_ratio = device_pixel_ratio;
    HubConnection::the().async_view_set_viewport(view_id(), size, device_pixel_ratio);
}

u64 RemoteView::add_navigation_listener(NavigationListener listener)
{
    auto id = m_next_navigation_listener_id++;
    m_navigation_listeners.set(id, move(listener));
    return id;
}

void RemoteView::did_start_navigation_load(URL::URL const& url)
{
    for (auto const& [id, listener] : m_navigation_listeners) {
        if (listener.on_load_start)
            listener.on_load_start(url);
    }
}

void RemoteView::did_finish_navigation_load(URL::URL const& url)
{
    for (auto const& [id, listener] : m_navigation_listeners) {
        if (listener.on_load_finish)
            listener.on_load_finish(url);
    }
}

void RemoteView::did_install_backing_stores(u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    m_backing_stores_page_id = page_id;
    m_backing_stores.install(move(bitmap_ids), move(backing_stores));
}

void RemoteView::did_present_backing_store(u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio)
{
    // NB: The Compositor may paint into the backing store this frame replaced as soon as it gets it back, so that must
    //     wait until nothing here reads from it anymore.
    auto result = m_backing_stores.present(bitmap_id, size, device_pixel_ratio);
    release_backing_store(page_id, result.bitmap_id_to_release);
}

void RemoteView::did_accept_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect)
{
    did_accept_presented_backing_store(bitmap_id, damage_rect);
    if (on_ready_to_paint)
        on_ready_to_paint();
}

void RemoteView::release_backing_store_to_page(u64 page_id, i32 bitmap_id)
{
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_release_backing_store(page_id, bitmap_id);
}

void RemoteView::enqueue_input_event(Web::InputEvent event)
{
    auto input_id = m_next_input_id++;
    event.visit(
        [&](Web::KeyEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            HubConnection::the().async_view_enqueue_key_event(view_id(), input_id, event.clone_without_browser_data());
        },
        [&](Web::MouseEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            HubConnection::the().async_view_enqueue_mouse_event(view_id(), input_id, event.clone_without_browser_data());
        },
        [&](Web::DragEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            auto event_to_send = event.clone_without_browser_data();
            event_to_send.files = move(event.files);
            HubConnection::the().async_view_enqueue_drag_event(view_id(), input_id, move(event_to_send));
        },
        [&](Web::PinchEvent& event) {
            HubConnection::the().async_view_enqueue_pinch_event(view_id(), event);
        });
}

void RemoteView::did_finish_handling_key_event(u64 input_id, Web::KeyEvent event)
{
    if (!on_finish_handling_key_event)
        return;
    if (auto browser_data = m_input_in_hub.take(input_id); browser_data.has_value())
        event.browser_data = browser_data.release_value();
    on_finish_handling_key_event(event);
}

void RemoteView::did_finish_handling_mouse_event(u64 input_id, Web::MouseEvent event, Web::EventResult result)
{
    if (!on_finish_handling_mouse_event)
        return;
    if (auto browser_data = m_input_in_hub.take(input_id); browser_data.has_value())
        event.browser_data = browser_data.release_value();
    on_finish_handling_mouse_event(event, result);
}

void RemoteView::did_finish_handling_drag_event(u64 input_id, Web::DragEvent event)
{
    if (!on_finish_handling_drag_event)
        return;
    if (auto browser_data = m_input_in_hub.take(input_id); browser_data.has_value())
        event.browser_data = browser_data.release_value();
    on_finish_handling_drag_event(event);
}

NonnullRefPtr<Core::Promise<ByteString>> RemoteView::selected_text()
{
    auto promise = Core::Promise<ByteString>::construct();
    auto request_id = m_next_request_id++;
    m_pending_selected_text.set(request_id, promise);
    HubConnection::the().async_view_request_selected_text(view_id(), request_id);
    return promise;
}

void RemoteView::did_get_selected_text(u64 request_id, Optional<ByteString> text)
{
    auto promise = m_pending_selected_text.take(request_id);
    if (!promise.has_value())
        return;
    if (text.has_value())
        (*promise)->resolve(text.release_value());
    else
        (*promise)->reject(Error::from_errno(ECANCELED));
}

void RemoteView::request_close_preflight(Function<void(bool)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_close_preflights.set(request_id, move(on_complete));
    HubConnection::the().async_view_request_close_preflight(view_id(), request_id);
}

void RemoteView::did_finish_close_preflight(u64 request_id, bool may_close)
{
    if (auto on_complete = m_pending_close_preflights.take(request_id); on_complete.has_value())
        (*on_complete)(may_close);
}

NonnullRefPtr<Core::Promise<String>> RemoteView::request_page_dumps()
{
    auto promise = Core::Promise<String>::construct();
    auto request_id = m_next_request_id++;
    m_pending_page_dumps.set(request_id, promise);
    HubConnection::the().async_view_request_page_dumps(view_id(), request_id);
    return promise;
}

void RemoteView::did_get_page_dumps(u64 request_id, Optional<String> dumps)
{
    auto promise = m_pending_page_dumps.take(request_id);
    if (!promise.has_value())
        return;
    if (dumps.has_value())
        (*promise)->resolve(dumps.release_value());
    else
        (*promise)->reject(Error::from_errno(ECANCELED));
}

static u64 s_next_close_id { 1 };

static Function<void()> finish_prepared_close(u64 close_id)
{
    return [close_id] {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_finish_prepared_close(close_id);
    };
}

Function<void()> RemoteView::prepare_for_close_without_prompting()
{
    auto close_id = s_next_close_id++;
    HubConnection::the().async_view_prepare_for_close_without_prompting(view_id(), close_id);
    return finish_prepared_close(close_id);
}

void RemoteView::prepare_for_immediate_close()
{
    HubConnection::the().async_view_prepare_for_immediate_close(view_id());
}

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
Optional<i32> RemoteView::history_step_at_delta(int delta) const
{
    return HubConnection::the().view_history_step_at_delta(view_id(), delta);
}

WebView::WebCompatReport::Page RemoteView::describe_page() const
{
    auto page = HubConnection::the().view_page_description(view_id());
    return { .url = page.take_url(), .user_agent = page.take_user_agent(), .viewport_size = page.viewport_size(), .device_pixel_ratio = page.device_pixel_ratio(), .zoom_level = page.zoom_level() };
}

Vector<WebView::HistoryEntry> RemoteView::recent_history_entries(size_t limit) const
{
    return HubConnection::the().view_recent_history_entries(view_id(), limit);
}

bool RemoteView::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    return HubConnection::the().view_look_up_selected_text_at(view_id(), widget_position);
}

bool RemoteView::needs_beforeunload_check() const
{
    return HubConnection::the().view_needs_beforeunload_check(view_id());
}

Vector<RemoteView::SessionHistoryTraversalMenuItem> RemoteView::session_history_traversal_menu_items(int direction) const
{
    return HubConnection::the().view_session_history_traversal_menu_items(view_id(), direction);
}

WebView::SessionHistorySnapshot RemoteView::session_history_snapshot() const
{
    return HubConnection::the().view_session_history_snapshot(view_id()).value_or({});
}

ErrorOr<void> RemoteView::restore_session_history_from_snapshot(WebView::SessionHistorySnapshot snapshot)
{
    if (!HubConnection::the().view_restore_session_history_from_snapshot(view_id(), move(snapshot)))
        return Error::from_string_literal("Unable to restore the session history");
    return {};
}
#else
Optional<i32> RemoteView::history_step_at_delta(int delta) const
{
    auto const& traversable = m_view->traversable();
    auto used_steps = traversable.session_history().used_steps();
    auto step_index = traversable.effective_current_session_history_step_index() + delta;
    if (step_index >= used_steps.size())
        return {};
    return used_steps[step_index];
}

WebView::WebCompatReport::Page RemoteView::describe_page() const
{
    return WebView::WebCompatReport::describe_page(*m_view);
}

Vector<WebView::HistoryEntry> RemoteView::recent_history_entries(size_t limit) const
{
    return m_view->recent_history_entries(limit);
}

bool RemoteView::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    return m_view->look_up_selected_text_at(widget_position);
}

bool RemoteView::needs_beforeunload_check() const
{
    return m_view->needs_beforeunload_check();
}

Vector<RemoteView::SessionHistoryTraversalMenuItem> RemoteView::session_history_traversal_menu_items(int direction) const
{
    return m_view->session_history_traversal_menu_items(direction);
}

WebView::SessionHistorySnapshot RemoteView::session_history_snapshot() const
{
    return m_view->session_history_snapshot();
}

ErrorOr<void> RemoteView::restore_session_history_from_snapshot(WebView::SessionHistorySnapshot snapshot)
{
    return m_view->restore_session_history_from_snapshot(move(snapshot));
}
#endif

RemoteView* RemoteView::find(u64 view_id)
{
    return s_views.get(view_id).value_or(nullptr);
}

void RemoteView::for_each(Function<void(RemoteView&)> const& callback)
{
    for (auto& [view_id, view] : s_views)
        callback(*view);
}

void RemoteView::did_change_session_history(WebView::SessionHistorySnapshot history, URL::URL url)
{
    // NB: A report the Hub sent before we stopped asking for them may still arrive.
    if (!m_session_tab_id.has_value())
        return;
    Application::session_store(m_is_private).update_tab_state({ .tab_id = *m_session_tab_id, .history = move(history), .url = move(url) });
}

// The browser's own strings are English, so languages are named in English.
static String language_display_name(StringView language)
{
    auto primary_language = WebView::primary_language_subtag(language);
    if (auto name = Unicode::language_display_name("en"sv, primary_language, Unicode::LanguageDisplay::Standard); name.has_value())
        return name->to_utf8();
    return MUST(String::from_utf8(primary_language));
}

void RemoteView::create_translation_actions()
{
    m_translate_action = WebView::Action::create("Translate"sv, WebView::ActionID::TranslatePage, [] { });

    m_translation_menu = WebView::Menu::create("Translation"sv);
    for (size_t index = 0; index < WebView::PageTranslation::MAXIMUM_OFFERED_LANGUAGES; ++index) {
        auto action = WebView::Action::create_checkable("Translate"sv, WebView::ActionID::TranslateTo, [this, index] {
            if (index < m_state.translation_languages.size())
                HubConnection::the().async_view_translate_to(view_id(), m_state.translation_languages[index]);
            // NB: The check mark shows the language the page is in, which activating an action doesn't change by itself.
            update_view_actions();
        });
        m_translation_menu->add_action(action);
        m_translate_to_actions.append(move(action));
    }
    m_show_original_action = WebView::Action::create("Show Original"sv, WebView::ActionID::ShowOriginal, [this] {
        HubConnection::the().async_view_show_original(view_id());
    });
    m_translation_menu->add_action(*m_show_original_action);
    m_translation_menu->add_separator();
    m_download_languages_action = WebView::Action::create("Download Languages…"sv, WebView::ActionID::DownloadTranslationLanguages, [] {
        if (auto* translator = Application::the().translator())
            translator->open_language_download_settings();
    });
    m_translation_menu->add_action(*m_download_languages_action);
    m_translation_menu->add_action(WebView::Action::create("Preferred Languages…"sv, WebView::ActionID::EditPreferredLanguages, [] {
        auto url = URL::about_settings();
        url.set_fragment("editLanguages"sv);
        Application::the().activate_or_open_tab_with_url(url);
    }));
}

void RemoteView::update_translation_actions()
{
    m_translate_action->set_visible(m_state.translation_offered);
    m_translate_action->set_engaged(m_state.is_translated);
    m_translate_action->set_tooltip(m_state.is_translated ? "Showing translation"sv : "Translate this page"sv);

    auto const& target_language = m_state.translation_target_language;
    for (size_t slot = 0; slot < m_translate_to_actions.size(); ++slot) {
        auto& action = m_translate_to_actions[slot];
        if (slot >= m_state.translation_languages.size()) {
            action->set_visible(false);
            continue;
        }
        auto const& language = m_state.translation_languages[slot];
        action->set_text(MUST(String::formatted("Translate to {}", language_display_name(language))));
        action->set_checked(target_language.has_value() && WebView::is_same_language(*target_language, language));
        action->set_visible(true);
    }

    m_show_original_action->set_enabled(m_state.is_translated);
    m_download_languages_action->set_visible(m_state.translation_needs_language_download);
}

void RemoteView::update_view_actions()
{
    // NB: The state can come before the view has made its actions.
    if (!m_navigate_back_action)
        return;

    m_navigate_back_action->set_enabled(m_state.can_navigate_back);
    m_navigate_forward_action->set_enabled(m_state.can_navigate_forward);
    if (m_state.zoom_level != 1.0) {
        m_reset_zoom_action->set_text(MUST(String::formatted("{}%", round_to<int>(m_state.zoom_level * 100))));
        m_reset_zoom_action->set_visible(true);
    } else {
        m_reset_zoom_action->set_visible(false);
    }
    update_translation_actions();
}

void RemoteView::did_request_context_menu(WebView::ContextMenuRequest const& request)
{
    // NB: The menu has to outlive this call, since the view may show it later.
    m_context_menu = create_context_menu(*this, request);
    show_context_menu(*m_context_menu, request.widget_position);
}

void RemoteView::did_make_link_to_highlight(Optional<URL::URL> url)
{
    m_link_to_highlight = move(url);
    m_copy_link_to_highlight_action->set_enabled(m_link_to_highlight.has_value());
}

}
