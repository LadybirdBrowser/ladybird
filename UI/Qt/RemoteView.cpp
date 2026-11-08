/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/HashMap.h>
#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
#    include <Hub/Application.h>
#    include <LibWebView/PageTranslation.h>
#endif
#include <LibCore/EventLoop.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/InternalURLs.h>
#include <LibUnicode/DisplayNames.h>
#include <LibWebView/ErrorHTML.h>
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HistoryStore.h>
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

    m_navigate_back_action = Action::create("Back"sv, ActionID::NavigateBack, [this] {
        traverse_the_history_by_delta(-1);
    });
    m_navigate_forward_action = Action::create("Forward"sv, ActionID::NavigateForward, [this] {
        traverse_the_history_by_delta(+1);
    });
    m_toggle_bookmark_action = Action::create("Toggle Bookmark"sv, ActionID::ToggleBookmarkViaToolbar, [] {
        Application::the().show_bookmark_popover();
    });
    m_reset_zoom_action = Action::create("100%"sv, ActionID::ResetZoomViaToolbar, [this] {
        reset_zoom();
    });
    m_reset_zoom_action->set_tooltip("Reset zoom level"sv);
    m_copy_link_to_highlight_action = Action::create("Copy Link to Highlight"sv, ActionID::CopyLinkToHighlight, [this] {
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

bool RemoteView::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    return HubConnection::the().view_look_up_selected_text_at(view_id(), widget_position);
}

bool RemoteView::needs_beforeunload_check() const
{
    return HubConnection::the().view_needs_beforeunload_check(view_id());
}

Vector<WebView::ViewImplementation::SessionHistoryTraversalItem> RemoteView::session_history_traversal_items(int direction) const
{
    return HubConnection::the().view_session_history_traversal_items(view_id(), direction);
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

bool RemoteView::look_up_selected_text_at(Gfx::IntPoint widget_position)
{
    return m_view->look_up_selected_text_at(widget_position);
}

bool RemoteView::needs_beforeunload_check() const
{
    return m_view->needs_beforeunload_check();
}

Vector<WebView::ViewImplementation::SessionHistoryTraversalItem> RemoteView::session_history_traversal_items(int direction) const
{
    return m_view->session_history_traversal_items(direction);
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

Vector<WebView::HistoryEntry> RemoteView::recent_history_entries(size_t limit) const
{
    if (is_private() == WebView::IsPrivate::Yes)
        return {};
    return Application::history_store().list_entries({}, 0, limit);
}

Vector<RemoteView::SessionHistoryTraversalMenuItem> RemoteView::session_history_traversal_menu_items(int direction) const
{
    Vector<SessionHistoryTraversalMenuItem> items;
    for (auto& item : session_history_traversal_items(direction)) {
        Optional<WebView::HistoryEntry> history_entry;
        if (is_private() == WebView::IsPrivate::No)
            history_entry = Application::history_store().entry_for_url(item.url);
        auto url = item.url.serialize();
        auto title = history_entry.has_value() && history_entry->title.has_value() && !history_entry->title->is_empty()
            ? move(*history_entry->title)
            : url;
        items.append({
            .step = item.step,
            .title = move(title),
            .url = move(url),
            .favicon_png = history_entry.has_value() ? move(history_entry->favicon_png) : OptionalNone {},
        });
    }
    return items;
}

void RemoteView::did_change_favicon(Optional<Gfx::Bitmap const&> favicon)
{
    m_favicon_hash.clear();
    if (!favicon.has_value())
        return;

    if (auto favicon_png = Gfx::PNGWriter::encode(*favicon); !favicon_png.is_error())
        m_favicon_hash = Application::favicon_store(is_private()).add_favicon(favicon_png.release_value());
    if (!m_favicon_hash.has_value() || is_private() == WebView::IsPrivate::Yes)
        return;

    Application::history_store().update_favicon(url(), *m_favicon_hash);
    Application::bookmark_store().update_favicon(url(), *m_favicon_hash);
}

void RemoteView::did_visit_page(URL::URL const& url, Optional<String> const& title, WebView::HistoryVisitTransition transition)
{
    if (is_private() == WebView::IsPrivate::No)
        Application::history_store().record_visit(url, title, UnixDateTime::now(), transition);
}

void RemoteView::did_change_visited_page_title(URL::URL const& url, String const& title)
{
    if (is_private() == WebView::IsPrivate::No)
        Application::history_store().update_title(url, title);
}

void RemoteView::did_finish_loading(URL::URL const& url)
{
    if (is_private() == WebView::IsPrivate::No && m_favicon_hash.has_value())
        Application::history_store().update_favicon(url, *m_favicon_hash);
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
    m_translate_action = Action::create("Translate"sv, ActionID::TranslatePage, [] { });

    m_translation_menu = Menu::create("Translation"sv);
    for (size_t index = 0; index < WebView::PageTranslation::MAXIMUM_OFFERED_LANGUAGES; ++index) {
        auto translate_to_action = Action::create_checkable("Translate"sv, ActionID::TranslateTo, [this, index] {
            if (index < m_state.translation_targets.size() && !m_state.translation_targets[index].needs_download)
                HubConnection::the().async_view_translate_to(view_id(), m_state.translation_targets[index].language);
            // NB: The check mark shows the language the page is in, which activating an action doesn't change by itself.
            update_view_actions();
        });
        translate_to_action->set_visible(false);
        m_translation_menu->add_action(translate_to_action);
        m_translate_to_actions.append(move(translate_to_action));

        auto download_languages_action = Action::create("Download"sv, ActionID::DownloadTranslationLanguages, [this, index] {
            if (index < m_state.translation_targets.size())
                HubConnection::the().async_view_download_translation_languages(view_id(), m_state.translation_targets[index].language);
        });
        download_languages_action->set_visible(false);
        m_translation_menu->add_action(download_languages_action);
        m_download_languages_actions.append(move(download_languages_action));
    }
    m_show_original_action = Action::create("Show Original"sv, ActionID::ShowOriginal, [this] {
        HubConnection::the().async_view_show_original(view_id());
    });
    m_translation_menu->add_action(*m_show_original_action);
    m_translation_menu->add_separator();
    m_prefer_higher_quality_action = Action::create_checkable("Use Higher-Quality Models"sv, ActionID::PreferHigherQualityTranslation, [this] {
        HubConnection::the().async_view_prefer_higher_quality_translation(view_id(), !m_state.translation_prefers_higher_quality);
        // NB: The check mark shows the saved choice, which the Hub sends back with the view's state.
        update_view_actions();
    });
    m_prefer_higher_quality_action->set_visible(false);
    m_translation_menu->add_action(*m_prefer_higher_quality_action);
    m_translation_menu->add_action(Action::create("Preferred Languages…"sv, ActionID::EditPreferredLanguages, [] {
        auto url = URL::about_settings();
        url.set_fragment("editLanguages"sv);
        Application::the().activate_or_open_tab_with_url(url);
    }));
}

void RemoteView::update_translation_actions()
{
    auto const& targets = m_state.translation_targets;
    // The platform's settings run a download and don't say how far along it is — so the button pulses until it ends.
    auto downloading = any_of(targets, [](auto const& target) { return target.downloading; });
    m_translate_action->set_visible(m_state.translation_offered);
    m_translate_action->set_engaged(m_state.is_translated);
    m_translate_action->set_busy(downloading);
    if (m_state.is_translated)
        m_translate_action->set_tooltip("Showing translation"sv);
    else if (downloading)
        m_translate_action->set_tooltip("Downloading a language to translate this page"sv);
    else
        m_translate_action->set_tooltip("Translate this page"sv);

    auto const& target_language = m_state.translation_target_language;
    for (size_t index = 0; index < m_translate_to_actions.size(); ++index) {
        auto& translate_to_action = m_translate_to_actions[index];
        auto& download_languages_action = m_download_languages_actions[index];
        if (index >= targets.size()) {
            translate_to_action->set_visible(false);
            download_languages_action->set_visible(false);
            continue;
        }

        auto const& target = targets[index];
        auto name = language_display_name(target.language);
        // The item names the languages to download, and the target when it isn't one of them: "Download Dutch to
        // Translate to English…" for a Dutch page, but "Download Italian to Translate…" for an English one.
        String download_names;
        String download_target;
        if (target.languages_to_download.is_empty()) {
            download_names = "Languages"_string;
            download_target = MUST(String::formatted(" to {}", name));
        } else {
            Vector<String> names;
            for (auto const& language : target.languages_to_download)
                names.append(language_display_name(language));
            download_names = MUST(String::join(" and "sv, names));
            if (!any_of(target.languages_to_download, [&](auto const& language) { return WebView::is_same_language(language, target.language); }))
                download_target = MUST(String::formatted(" to {}", name));
        }

        translate_to_action->set_text(MUST(String::formatted("Translate to {}", name)));
        translate_to_action->set_checked(target_language.has_value() && WebView::is_same_language(*target_language, target.language));
        translate_to_action->set_visible(!target.needs_download);

        if (target.needs_download && target.downloading)
            download_languages_action->set_text(MUST(String::formatted("Downloading {} to Translate{}…", download_names, download_target)));
        else if (target.needs_download)
            download_languages_action->set_text(MUST(String::formatted("Download {} to Translate{}…", download_names, download_target)));
        else if (target.downloading)
            download_languages_action->set_text(MUST(String::formatted("Downloading {} for Faster Translation{}…", download_names, download_target)));
        else
            download_languages_action->set_text(MUST(String::formatted("Download {} for Faster Translation{}…", download_names, download_target)));
        download_languages_action->set_enabled(!target.downloading);
        download_languages_action->set_visible(target.needs_download || target.faster_after_download);
    }

    m_show_original_action->set_enabled(m_state.is_translated);

    auto* translator = Application::the().translator();
    auto higher_quality_models_name = translator ? translator->higher_quality_models_name() : Optional<StringView> {};
    if (higher_quality_models_name.has_value())
        m_prefer_higher_quality_action->set_text(MUST(String::formatted("Use {} (Slower)", *higher_quality_models_name)));
    m_prefer_higher_quality_action->set_checked(m_state.translation_prefers_higher_quality);
    m_prefer_higher_quality_action->set_visible(higher_quality_models_name.has_value() && any_of(targets, [](auto const& target) { return target.higher_quality_available; }));
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

void RemoteView::load_from_user_input(URL::URL const& url)
{
    if (on_load_from_user_input)
        on_load_from_user_input();

    if (!WebView::is_url_handled_internally(url)) {
        open_external_url_from_user_input(url);
        return;
    }
    load(url);
}

void RemoteView::load_from_user_input(StringView input)
{
    load_from_user_input(input, WebView::sanitize_url(input, Application::settings().search_engine_settings().engine));
}

void RemoteView::load_from_user_input(StringView input, Optional<URL::URL> fallback_url)
{
    if (on_load_from_user_input)
        on_load_from_user_input();

    auto classified_input = WebView::classify_user_input(input);

    if (classified_input.classification != WebView::UserInputClassification::ExternalURL) {
        if (fallback_url.has_value())
            load(*fallback_url);
        else
            load_navigation_error_page(input);
        return;
    }

    VERIFY(classified_input.url.has_value());
    auto external_url = classified_input.url.release_value();

    if (fallback_url.has_value() && *fallback_url == external_url) {
        open_external_url_from_user_input(external_url);
        return;
    }

    open_external_url_from_user_input(external_url, [view_id = view_id(), input = MUST(String::from_utf8(input)), fallback_url = move(fallback_url)] {
        auto* view = RemoteView::find(view_id);
        if (!view)
            return;
        if (fallback_url.has_value())
            view->load(*fallback_url);
        else
            view->load_navigation_error_page(input);
    });
}

void RemoteView::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
{
    if (!WebView::is_url_handled_internally(url)) {
        open_external_url_from_user_input(url);
        return;
    }
    HubConnection::the().async_view_open_url_in_new_tab(view_id(), url, activate_tab);
}

void RemoteView::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    if (!WebView::is_url_handled_internally(url)) {
        open_external_url_from_user_input(url);
        return;
    }
    HubConnection::the().async_view_open_url_in_new_window(view_id(), url, is_private);
}

void RemoteView::load_navigation_error_page(StringView input)
{
    auto message = MUST(String::formatted("Failed to load \"{}\"", input));

    StringBuilder builder;
    builder.appendff(WebView::ERROR_HTML_HEADER, ""sv, WebView::ERROR_SVG, message);
    builder.append("<p>If you were trying to enter a search query, please enable search in <a href=\"about:settings#search\">settings</a>.</p>"sv);
    builder.append(WebView::ERROR_HTML_FOOTER);
    load_html(builder.string_view());
}

void RemoteView::open_external_url_from_user_input(URL::URL const& url, Function<void()> on_handler_unavailable)
{
    m_pending_external_urls.enqueue({ url, this->url().origin(), move(on_handler_unavailable) });
    open_next_external_url();
}

void RemoteView::open_next_external_url()
{
    while (!m_external_url_pending && !m_pending_external_urls.is_empty()) {
        auto request = m_pending_external_urls.dequeue();
        if (WebView::ExternalURLRequestPolicy::is_denied_scheme(request.url.scheme()))
            continue;

        m_external_url_pending = true;
        auto url = request.url;
        Application::the().resolve_external_url_handler(url, [view_id = view_id(), request = move(request)](RefPtr<WebView::ExternalURLHandler> handler) mutable {
            if (auto* view = RemoteView::find(view_id))
                view->open_external_url_with_handler(move(request), move(handler));
        });
    }
}

void RemoteView::open_external_url_with_handler(PendingExternalURL request, RefPtr<WebView::ExternalURLHandler> handler)
{
    auto launch = [view_id = view_id()](WebView::ExternalURLHandler const& handler, URL::URL const& url) {
        handler.launch(url, [view_id](bool success) {
            if (!success && RemoteView::find(view_id))
                Application::the().display_error_dialog("Unable to open external URL"sv);
        });
    };

    if (!handler || handler->is_ladybird()) {
        finish_external_url();
        if (request.on_handler_unavailable)
            request.on_handler_unavailable();
        else if (!handler)
            Application::the().display_error_dialog(MUST(String::formatted("No application is registered to open {}", request.url)));
        return;
    }

    // Don't prompt when the user asked for a mailto: URL. This matches other browsers.
    if (request.url.scheme() == "mailto"sv) {
        launch(*handler, request.url);
        finish_external_url();
        return;
    }

    if (!on_request_external_url_confirmation) {
        finish_external_url();
        return;
    }

    on_request_external_url_confirmation(request.url, request.initiator_origin, handler->application_name(), [view_id = view_id(), url = request.url, handler, launch](bool accepted) {
        if (accepted)
            launch(*handler, url);
        if (auto* view = RemoteView::find(view_id))
            view->finish_external_url();
    });
}

void RemoteView::finish_external_url()
{
    VERIFY(m_external_url_pending);
    m_external_url_pending = false;
    if (m_pending_external_urls.is_empty())
        return;
    Core::deferred_invoke([view_id = view_id()] {
        if (auto* view = RemoteView::find(view_id))
            view->open_next_external_url();
    });
}

}
