/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibCore/Process.h>
#include <LibWebView/CrashReportOutbox.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteActions.h>
#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/RemoteSuggestionProvider.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

static HubConnection* s_the;

HubConnection::HubConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>(*this, move(transport))
{
    s_the = this;
}

HubConnection::~HubConnection()
{
    s_the = nullptr;
}

HubConnection& HubConnection::the()
{
    return *s_the;
}

HubConnection* HubConnection::the_if_connected()
{
    return s_the;
}

void HubConnection::did_initialize()
{
    m_did_initialize = true;
    set_peer_owns_this_process(true);
}

void HubConnection::die()
{
    // NB: A Hub that ends before answering initialize() has just found that there is nothing for us to do.
    if (!m_did_initialize)
        return;

    // The UI has nothing left to show without the Hub.
    Core::Process::terminate_immediately(1);
}

void HubConnection::crash_reports_changed()
{
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    // NB: The Hub has an outbox of its own, and it is that one that saw the change. In one process there is only the one,
    //     which has told its observers already, and telling them again here would have the Hub send this once more.
    WebView::CrashReportOutbox::the().reports_changed();
#endif
}

u64 HubConnection::post_report(URL::URL const& url, Vector<HTTP::Header> headers, ByteString body, Function<void(WebView::ReportSubmission::PostResponse)> on_response)
{
    auto post_id = m_next_report_post_id++;
    m_report_posts.set(post_id, move(on_response));
    async_post_report(post_id, url, move(headers), move(body));
    return post_id;
}

void HubConnection::cancel_report_post(u64 post_id)
{
    m_report_posts.remove(post_id);
    async_cancel_report_post(post_id);
}

void HubConnection::report_post_finished(u64 post_id, bool had_network_error, Optional<u32> response_code, Vector<HTTP::Header> response_headers, ByteString body)
{
    if (auto on_response = m_report_posts.take(post_id); on_response.has_value())
        (*on_response)({ .had_network_error = had_network_error, .response_code = response_code, .headers = move(response_headers), .body = move(body) });
}

void HubConnection::view_install_backing_stores(u64 view_id, u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_install_backing_stores(page_id, move(bitmap_ids), move(backing_stores));
    else if (!bitmap_ids.is_empty())
        RemoteView::release_backing_store_to_page(page_id, bitmap_ids[0]);
}

void HubConnection::view_add_backing_stores(u64 view_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_add_backing_stores(move(bitmap_ids), move(backing_stores));
}

void HubConnection::view_retire_backing_stores(u64 view_id, Vector<i32> bitmap_ids)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_retire_backing_stores(bitmap_ids);
}

void HubConnection::view_present_backing_store(u64 view_id, u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_present_backing_store(page_id, bitmap_id, size, device_pixel_ratio);
    else
        RemoteView::release_backing_store_to_page(page_id, bitmap_id);
}

void HubConnection::view_accept_presented_backing_store(u64 view_id, i32 bitmap_id, Gfx::IntRect damage_rect)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_accept_backing_store(bitmap_id, damage_rect);
}

void HubConnection::view_keep_front_backing_store_as_backup(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_keep_front_backing_store_as_backup();
}

void HubConnection::view_clear_backup_backing_store(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_clear_backup_backing_store();
}

void HubConnection::view_clear_backing_stores(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_clear_backing_stores();
}

void HubConnection::view_finish_handling_key_event(u64 view_id, u64 input_id, Web::KeyEvent event)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_key_event(input_id, move(event));
}

void HubConnection::view_finish_handling_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event, Web::EventResult result)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_mouse_event(input_id, move(event), result);
}

void HubConnection::view_finish_handling_drag_event(u64 view_id, u64 input_id, Web::DragEvent event)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_drag_event(input_id, move(event));
}

void HubConnection::view_release_input(u64 view_id, u64 input_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->release_input(input_id);
}

void HubConnection::view_open_new_view(u64 view_id, u64 new_view_id, Hub::ViewState, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_open_new_view)
        view->on_open_new_view(activate_tab, hints, new_view_id);
    else
        async_destroy_view(new_view_id);
}

void HubConnection::view_favicon_change(u64 view_id, Optional<Gfx::ShareableBitmap> favicon)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_favicon_change)
        return;
    if (favicon.has_value() && favicon->is_valid())
        view->on_favicon_change(*favicon->bitmap());
    else
        view->on_favicon_change({});
}

void HubConnection::view_request_before_unload(u64 view_id, u64 answer_id, String source)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_before_unload) {
        async_view_answer(view_id, answer_id, true);
        return;
    }
    view->on_request_before_unload(source, [view_id, answer_id](bool answer) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer(view_id, answer_id, answer);
    });
}

void HubConnection::view_request_external_url_confirmation(u64 view_id, u64 answer_id, URL::URL url, URL::Origin initiator_origin, String application_name)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_external_url_confirmation) {
        async_view_answer(view_id, answer_id, false);
        return;
    }
    view->on_request_external_url_confirmation(url, initiator_origin, application_name, [view_id, answer_id](bool answer) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer(view_id, answer_id, answer);
    });
}

void HubConnection::view_request_permission(u64 view_id, u64 answer_id, WebView::PermissionPromptRequest request)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_permission) {
        async_view_answer_permission_request(view_id, answer_id, Web::Permissions::PromptOutcome::Dismissed);
        return;
    }
    view->on_request_permission(request, [view_id, answer_id](Web::Permissions::PromptOutcome outcome) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer_permission_request(view_id, answer_id, outcome);
    });
}

void HubConnection::view_request_date_picker(u64 view_id, Utf16String input_type, Utf16String value, Utf16String minimum, Utf16String maximum)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_date_picker)
        view->on_request_date_picker(input_type, value, minimum, maximum);
}

void HubConnection::view_request_dictionary_lookup(u64 view_id, WebView::DictionaryLookup lookup, Gfx::IntPoint position)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_dictionary_lookup)
        view->on_request_dictionary_lookup(lookup, position);
}

void HubConnection::app_open_url_in_new_tab(URL::URL url, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private)
{
    Application::the().open_url_in_new_tab(url, activate_tab, source_view_id, is_private);
}

void HubConnection::app_open_url_in_new_window(URL::URL url, WebView::IsPrivate is_private)
{
    Application::the().open_url_in_new_window(move(url), move(is_private));
}

void HubConnection::app_open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private)
{
    Application::the().open_navigation_in_new_tab(move(navigation), activate_tab, source_view_id, is_private);
}

void HubConnection::app_open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    Application::the().open_navigation_in_new_window(move(navigation), move(is_private));
}

void HubConnection::app_set_clipboard_text(String text, WebView::Application::ClipboardType type)
{
    Application::the().set_clipboard_text(move(text), move(type));
}

void HubConnection::app_insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    Application::the().insert_clipboard_item(move(item));
}

void HubConnection::app_update_tabs_display()
{
    Application::the().update_tabs_display();
}

void HubConnection::app_on_recently_closed_entries_changed()
{
    Application::the().on_recently_closed_entries_changed();
}

void HubConnection::app_did_enable_devtools()
{
    Application::the().did_enable_devtools();
}

void HubConnection::app_did_disable_devtools()
{
    Application::the().did_disable_devtools();
}

void HubConnection::app_open_urls_in_new_tabs(Vector<URL::URL> urls)
{
    Application::the().open_urls_in_new_tabs(urls);
}

void HubConnection::app_activate_or_open_tab_with_url(URL::URL url)
{
    Application::the().activate_or_open_tab_with_url(url);
}

void HubConnection::app_open_view_in_new_tab(u64 view_id, Optional<u64> source_view_id, WebView::IsPrivate is_private, Web::HTML::ActivateTab activate_tab)
{
    Application::the().open_view_in_new_tab(view_id, source_view_id, is_private, activate_tab);
}

void HubConnection::app_display_error_dialog(String message)
{
    Application::the().display_error_dialog(message);
}

void HubConnection::app_display_download_confirmation_dialog(String download_name, ByteString path)
{
    Application::the().display_download_confirmation_dialog(download_name, path);
}

void HubConnection::app_open_file(String path)
{
    Application::the().open_file(path);
}

void HubConnection::app_read_clipboard_text(u64 request_id, WebView::Application::ClipboardType type)
{
    async_answer_clipboard_text(request_id, Application::the().clipboard_text(type));
}

void HubConnection::app_read_clipboard_item(u64 request_id)
{
    async_answer_clipboard_item(request_id, Application::the().clipboard_item());
}

void HubConnection::app_ask_user_for_download_path(u64 request_id, ByteString file)
{
    Application::the().ask_user_for_download_path(request_id, file);
}

void HubConnection::download_added(Hub::DownloadState download)
{
    RemoteDownloads::the().did_add(download);
}

void HubConnection::download_updated(Hub::DownloadState download)
{
    RemoteDownloads::the().did_update(download);
}

void HubConnection::download_removed(u64 id, WebView::IsPrivate is_private)
{
    RemoteDownloads::the().did_remove(id, is_private);
}

void HubConnection::bookmarks_changed(ByteString bookmarks_json)
{
    auto bookmarks = JsonValue::from_string(bookmarks_json);
    if (bookmarks.is_error() || !bookmarks.value().is_object())
        return;
    Application::bookmark_store().replace_with_json(bookmarks.value(), WebView::BookmarkStore::PersistChange::No);
}

void HubConnection::app_read_bookmarks_for_all_tabs(u64 request_id, u64 view_id)
{
    async_answer_bookmarks_for_all_tabs(request_id, Application::the().bookmarks_for_all_tabs_in_window_of_view(view_id));
}

void HubConnection::app_show_bookmark_popover()
{
    Application::the().show_bookmark_popover();
}

void HubConnection::app_show_bookmark_context_menu(Gfx::IntPoint position, Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id)
{
    Optional<WebView::BookmarkItem const&> item;
    if (item_id.has_value())
        item = Application::bookmark_store().find_item_by_id(*item_id);
    Application::the().show_bookmark_context_menu(position, item, target_folder_id, parent_folder_id);
}

void HubConnection::app_display_bookmark_dialog(u64 request_id, Hub::BookmarkDialogKind kind, Optional<String> item_id, Optional<String> default_title, Optional<String> folder_id)
{
    auto& application = Application::the();
    Optional<WebView::BookmarkItem const&> item;
    if (item_id.has_value())
        item = Application::bookmark_store().find_item_by_id(*item_id);

    auto answer = [request_id](Optional<Hub::BookmarkDialogResult> result) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_answer_bookmark_dialog(request_id, move(result));
    };

    RefPtr<Application::BookmarkPromise> promise;
    switch (kind) {
    case Hub::BookmarkDialogKind::AddBookmark:
        promise = application.display_add_bookmark_dialog(folder_id);
        break;
    case Hub::BookmarkDialogKind::EditBookmark:
        if (item.has_value())
            promise = application.display_edit_bookmark_dialog(*item, folder_id);
        break;
    case Hub::BookmarkDialogKind::AddFolder:
        promise = application.display_add_bookmark_folder_dialog(default_title, folder_id);
        break;
    case Hub::BookmarkDialogKind::EditFolder:
        if (item.has_value())
            promise = application.display_edit_bookmark_folder_dialog(*item, folder_id);
        break;
    }
    if (!promise) {
        answer({});
        return;
    }
    promise->when_resolved([answer](WebView::Application::BookmarkDialogResult& result) {
        Hub::BookmarkDialogResult answer_to_send;
        answer_to_send.target_folder_id = result.target_folder_id;
        result.data.visit(
            [&](WebView::BookmarkItem::Bookmark const& bookmark) {
                answer_to_send.url = bookmark.url;
                answer_to_send.title = bookmark.title;
                answer_to_send.favicon_hash = bookmark.favicon_hash;
            },
            [&](WebView::BookmarkItem::Folder const& folder) {
                answer_to_send.is_folder = true;
                answer_to_send.title = folder.title;
            });
        answer(move(answer_to_send));
    });
    promise->when_rejected([answer](Error const&) {
        answer({});
    });
}

void HubConnection::omnibox_suggestions(u64 client_id, u64 query_id, Vector<WebView::AutocompleteSuggestion> suggestions)
{
    if (auto* provider = RemoteSuggestionProvider::find(client_id); provider && provider->on_suggestions)
        provider->on_suggestions(query_id, move(suggestions));
}

void HubConnection::pip_open(u64 view_id, u64 owner_view_id, Gfx::IntSize video_size)
{
    Application::the().open_picture_in_picture_window(view_id, owner_view_id, video_size);
}

void HubConnection::pip_hide(u64 view_id)
{
    Application::the().hide_picture_in_picture_window(view_id);
}

void HubConnection::pip_set_video_size(u64 view_id, Gfx::IntSize video_size)
{
    Application::the().set_picture_in_picture_video_size(view_id, video_size);
}

void HubConnection::pip_close(u64 view_id)
{
    Application::the().close_picture_in_picture_window(view_id);
}

void HubConnection::notification_show(u64 id, u64 page_id, Optional<u64> replaces, Web::NotificationsAPI::NotificationData data, String attribution, Optional<Gfx::ShareableBitmap> icon)
{
    RefPtr<Gfx::Bitmap const> icon_bitmap;
    if (icon.has_value() && icon->is_valid())
        icon_bitmap = icon->bitmap();
    Application::the().notification_backend().show({
        .id = id,
        .page_id = Web::PageId { page_id },
        .replaces = replaces,
        .data = move(data),
        .attribution = move(attribution),
        .icon = move(icon_bitmap),
    });
}

void HubConnection::notification_close(u64 id)
{
    Application::the().notification_backend().close(id);
}

Core::GeolocationProvider* HubConnection::ensure_geolocation_provider()
{
    if (m_geolocation_provider)
        return m_geolocation_provider.ptr();

    auto provider = Core::GeolocationProvider::create();
    if (provider.is_error()) {
        dbgln("Unable to create the geolocation provider: {}", provider.error());
        return nullptr;
    }
    m_geolocation_provider = provider.release_value();
    m_geolocation_provider->on_authorization_change = [this] {
        async_geolocation_authorization_changed(m_geolocation_provider->authorization());
    };
    async_geolocation_authorization_changed(m_geolocation_provider->authorization());
    return m_geolocation_provider.ptr();
}

static Core::GeolocationError geolocation_provider_unavailable_error()
{
    return { Core::GeolocationError::Type::PositionUnavailable, "The location provider is not available"_string };
}

// NB: Our provider tells the Hub of changes to the system's authorization from when it exists, so the Hub has it made
//     as soon as it needs the location rather than with the first position request.
void HubConnection::geolocation_start()
{
    (void)ensure_geolocation_provider();
}

void HubConnection::geolocation_request_position(u64 id)
{
    auto* provider = ensure_geolocation_provider();
    if (!provider) {
        async_geolocation_error(id, geolocation_provider_unavailable_error());
        return;
    }
    auto request = provider->request_current_position(
        [this, id](Core::GeolocationCoordinates coordinates) {
            m_geolocation_requests.remove(id);
            async_geolocation_position(id, coordinates);
        },
        [this, id](Core::GeolocationError error) {
            m_geolocation_requests.remove(id);
            async_geolocation_error(id, error);
        });
    if (request.is_error()) {
        async_geolocation_error(id, request.release_error());
        return;
    }
    m_geolocation_requests.set(id, request.value());
}

void HubConnection::geolocation_cancel_position_request(u64 id)
{
    if (auto request = m_geolocation_requests.take(id); request.has_value())
        m_geolocation_provider->cancel_current_position_request(*request);
}

void HubConnection::geolocation_start_watching(u64 id)
{
    auto* provider = ensure_geolocation_provider();
    if (!provider) {
        async_geolocation_error(id, geolocation_provider_unavailable_error());
        return;
    }
    auto watch = provider->start_watching_position(
        [this, id](Core::GeolocationCoordinates coordinates) { async_geolocation_position(id, coordinates); },
        [this, id](Core::GeolocationError error) { async_geolocation_error(id, error); });
    if (watch.is_error()) {
        async_geolocation_error(id, watch.release_error());
        return;
    }
    m_geolocation_watches.set(id, watch.value());
}

void HubConnection::geolocation_stop_watching(u64 id)
{
    if (auto watch = m_geolocation_watches.take(id); watch.has_value())
        m_geolocation_provider->stop_watching_position(*watch);
}

void HubConnection::translator_detect_language(u64 request_id, Web::TranslationLanguageSample sample)
{
    auto* translator = Application::the().translator();
    if (!translator) {
        async_translator_language_detected(request_id, {});
        return;
    }
    translator->detect_language(sample, [this, request_id](Optional<String> language) {
        async_translator_language_detected(request_id, language);
    });
}

void HubConnection::translator_target_languages(u64 request_id, String source_language, Vector<String> candidates)
{
    auto* translator = Application::the().translator();
    if (!translator) {
        async_translator_target_languages_ready(request_id, {});
        return;
    }
    translator->target_languages(source_language, candidates, [this, request_id](Vector<WebView::Translator::TargetLanguage> target_languages) {
        async_translator_target_languages_ready(request_id, target_languages);
    });
}

void HubConnection::translator_create_session(u64 session_id, String source_language, String target_language)
{
    if (auto* translator = Application::the().translator())
        m_translator_sessions.set(session_id, translator->create_session(source_language, target_language));
}

void HubConnection::translator_close_session(u64 session_id)
{
    if (auto session = m_translator_sessions.take(session_id); session.has_value())
        Application::the().translator()->close_session(*session);
}

void HubConnection::translator_translate(u64 request_id, u64 session_id, Web::TranslationUnit unit)
{
    auto session = m_translator_sessions.get(session_id);
    if (!session.has_value()) {
        async_translator_translation_finished(request_id, {});
        return;
    }
    Application::the().translator()->translate(*session, move(unit), [this, request_id](Optional<Vector<Web::TranslationRun>> runs) {
        async_translator_translation_finished(request_id, runs);
    });
}

void HubConnection::translator_open_language_download_settings()
{
    if (auto* translator = Application::the().translator())
        translator->open_language_download_settings();
}

void HubConnection::app_focus_view_from_notification(u64 view_id)
{
    Application::the().focus_view_from_notification(view_id);
}

void HubConnection::app_open_url_in_new_tab_from_notification(URL::URL url, Optional<u64> view_id, WebView::IsPrivate is_private)
{
    Application::the().open_url_in_new_tab_from_notification(url, view_id, is_private);
}

void HubConnection::external_url_resolve_handler(u64 request_id, URL::URL url)
{
    Application::the().resolve_external_url_handler_for_hub(request_id, url);
}

void HubConnection::external_url_launch(u64 handler_id, u64 request_id, URL::URL url)
{
    Application::the().launch_external_url(handler_id, request_id, url);
}

void HubConnection::external_url_release_handler(u64 handler_id)
{
    Application::the().release_external_url_handler(handler_id);
}

void HubConnection::view_selected_text(u64 view_id, u64 request_id, Optional<ByteString> text)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_get_selected_text(request_id, move(text));
}

void HubConnection::view_close_preflight_finished(u64 view_id, u64 request_id, bool may_close)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_close_preflight(request_id, may_close);
}

void HubConnection::view_page_dumps(u64 view_id, u64 request_id, Optional<String> dumps)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_get_page_dumps(request_id, move(dumps));
}

void HubConnection::app_open_urls(Vector<URL::URL> urls)
{
    if (auto& application = Application::the(); application.on_open_urls)
        application.on_open_urls(urls);
}

void HubConnection::app_open_new_tab(Vector<URL::URL> urls)
{
    if (auto& application = Application::the(); application.on_new_tab)
        application.on_new_tab(urls);
}

void HubConnection::app_open_new_window(Vector<URL::URL> urls)
{
    if (auto& application = Application::the(); application.on_new_window)
        application.on_new_window(urls);
}

void HubConnection::set_settings_from_hub(JsonValue const& settings)
{
    m_last_known_settings = settings.as_object();
}

void HubConnection::send_settings_changes(JsonValue const& settings)
{
    auto changes = WebView::Settings::changes_between(m_last_known_settings, settings.as_object());
    if (changes.is_empty())
        return;

    m_last_known_settings = settings.as_object();
    auto change_id = m_next_settings_change_id++;
    async_update_settings(change_id, JsonValue { changes }.serialized().to_byte_string());
    m_unconfirmed_settings_changes.append({ change_id, move(changes) });
}

void HubConnection::settings_changed(ByteString settings_json, u64 last_applied_change_id)
{
    auto settings = JsonValue::from_string(settings_json);
    if (settings.is_error() || !settings.value().is_object())
        return;

    // NB: What the Hub sent can be older than a change made here, which must not go away until the Hub has it.
    m_unconfirmed_settings_changes.remove_all_matching([&](auto const& change) { return change.id <= last_applied_change_id; });
    auto& new_settings = settings.value().as_object();
    for (auto const& change : m_unconfirmed_settings_changes)
        WebView::Settings::apply_changes(new_settings, change.changes);
    m_last_known_settings = new_settings;

    auto current_settings = Application::settings().serialize_json();
    if (WebView::Settings::changes_between(current_settings.as_object(), new_settings).is_empty())
        return;
    Application::settings().replace_with_json(settings.value(), WebView::Settings::PersistChange::No);
}

void HubConnection::action_retired(u64 handle)
{
    RemoteActions::the().retire(handle);
}

void HubConnection::action_changed(Hub::ActionState state)
{
    RemoteActions::the().update(state);
}

void HubConnection::bookmarks_menu_changed(Vector<Hub::MenuEntry> entries)
{
    RemoteActions::the().set_bookmarks_menu(entries);
    Application::the().rebuild_bookmarks_menu();
}

void HubConnection::view_show_context_menu(u64 view_id, Vector<Hub::MenuEntry> entries, Gfx::IntPoint widget_position)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_request_context_menu(entries, widget_position);
}

void HubConnection::view_update_zoom(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_update_zoom();
}

void HubConnection::view_navigation_load_start(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_start_navigation_load(url);
}

void HubConnection::view_navigation_load_finish(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_navigation_load(url);
}

void HubConnection::view_state_changed(u64 view_id, Hub::ViewState state)
{
    if (auto* view = RemoteView::find(view_id))
        view->set_state(move(state));
}

void HubConnection::view_load_start(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_load_start)
        view->on_load_start();
}

void HubConnection::view_load_finish(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_load_finish)
        view->on_load_finish(move(url));
}

void HubConnection::view_activate_tab(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_activate_tab)
        view->on_activate_tab();
}

void HubConnection::view_close(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_close)
        view->on_close();
}

void HubConnection::view_link_hover(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_link_hover)
        view->on_link_hover(move(url));
}

void HubConnection::view_link_unhover(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_link_unhover)
        view->on_link_unhover();
}

void HubConnection::view_title_change(u64 view_id, Utf16String title)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_title_change)
        view->on_title_change(move(title));
}

void HubConnection::view_url_change(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_url_change)
        view->on_url_change(move(url));
}

void HubConnection::view_loading_state_change(u64 view_id, bool is_loading)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_loading_state_change)
        view->on_loading_state_change(move(is_loading));
}

void HubConnection::view_top_level_navigation_commit(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_top_level_navigation_commit)
        view->on_top_level_navigation_commit();
}

void HubConnection::view_browser_history_traversal_complete(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_browser_history_traversal_complete)
        view->on_browser_history_traversal_complete();
}

void HubConnection::view_cursor_change(u64 view_id, Gfx::Cursor cursor)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_cursor_change)
        view->on_cursor_change(move(cursor));
}

void HubConnection::view_input_method_state_change(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_input_method_state_change)
        view->on_input_method_state_change();
}

void HubConnection::view_enter_tooltip_area(u64 view_id, ByteString tooltip)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_enter_tooltip_area)
        view->on_enter_tooltip_area(move(tooltip));
}

void HubConnection::view_leave_tooltip_area(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_leave_tooltip_area)
        view->on_leave_tooltip_area();
}

void HubConnection::view_content_blocking_change(u64 view_id, WebView::ContentBlockingSnapshot snapshot)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_content_blocking_change)
        view->on_content_blocking_change(move(snapshot));
}

void HubConnection::view_request_alert(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_alert)
        view->on_request_alert(move(message));
}

void HubConnection::view_request_confirm(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_confirm)
        view->on_request_confirm(move(message));
}

void HubConnection::view_request_prompt(u64 view_id, Utf16String message, Utf16String default_value)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_prompt)
        view->on_request_prompt(move(message), move(default_value));
}

void HubConnection::view_request_set_prompt_text(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_set_prompt_text)
        view->on_request_set_prompt_text(move(message));
}

void HubConnection::view_before_browser_initiated_navigation(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_before_browser_initiated_navigation)
        view->on_before_browser_initiated_navigation();
}

void HubConnection::view_request_accept_dialog(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_accept_dialog)
        view->on_request_accept_dialog();
}

void HubConnection::view_request_dismiss_dialog(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_dismiss_dialog)
        view->on_request_dismiss_dialog();
}

void HubConnection::view_request_website_issue_report(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_website_issue_report)
        view->on_request_website_issue_report();
}

void HubConnection::view_request_screenshot(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_screenshot)
        view->on_request_screenshot();
}

void HubConnection::view_accessibility_tree_received(u64 view_id, u64 sequence, Vector<WebView::AccessibilityNodeData> nodes)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_tree_received)
        view->on_accessibility_tree_received(move(sequence), move(nodes));
}

void HubConnection::view_accessibility_tree_changed(u64 view_id, u64 base_sequence, u64 sequence, WebView::AccessibilityTreeDelta delta)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_tree_changed)
        view->on_accessibility_tree_changed(move(base_sequence), move(sequence), move(delta));
}

void HubConnection::view_accessibility_focus_changed(u64 view_id, i64 node_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_focus_changed)
        view->on_accessibility_focus_changed(move(node_id));
}

void HubConnection::view_restore_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_restore_window)
        view->on_restore_window();
}

void HubConnection::view_reposition_window(u64 view_id, Gfx::IntPoint position)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_reposition_window)
        view->on_reposition_window(move(position));
}

void HubConnection::view_resize_window(u64 view_id, Gfx::IntSize size)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_resize_window)
        view->on_resize_window(move(size));
}

void HubConnection::view_maximize_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_maximize_window)
        view->on_maximize_window();
}

void HubConnection::view_minimize_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_minimize_window)
        view->on_minimize_window();
}

void HubConnection::view_fullscreen_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_fullscreen_window)
        view->on_fullscreen_window();
}

void HubConnection::view_exit_fullscreen_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_exit_fullscreen_window)
        view->on_exit_fullscreen_window();
}

void HubConnection::view_request_color_picker(u64 view_id, Color current_color)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_color_picker)
        view->on_request_color_picker(move(current_color));
}

void HubConnection::view_cancel_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_cancel_permission_prompt)
        view->on_cancel_permission_prompt(move(request));
}

void HubConnection::view_tab_indicator_change(u64 view_id, WebView::TabIndicator indicator, bool visible)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_tab_indicator_change)
        view->on_tab_indicator_change(move(indicator), move(visible));
}

void HubConnection::view_request_file_picker(u64 view_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_file_picker)
        view->on_request_file_picker(move(accepted_file_types), move(allow_multiple_files));
}

void HubConnection::view_request_select_dropdown(u64 view_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_select_dropdown)
        view->on_request_select_dropdown(move(content_position), move(minimum_width), move(items));
}

void HubConnection::view_picture_in_picture_control_activated(u64 view_id, Compositing::PictureInPictureControl control)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_picture_in_picture_control_activated)
        view->on_picture_in_picture_control_activated(move(control));
}

void HubConnection::view_find_in_page(u64 view_id, size_t current_match_index, Optional<size_t> total_match_count)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_find_in_page)
        view->on_find_in_page(move(current_match_index), move(total_match_count));
}

void HubConnection::view_page_background_color_change(u64 view_id, Gfx::Color color)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_page_background_color_change)
        view->on_page_background_color_change(move(color));
}

void HubConnection::view_screen_wake_lock_state_changed(u64 view_id, Web::ScreenWakeLockState state)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_screen_wake_lock_state_changed)
        view->on_screen_wake_lock_state_changed(move(state));
}

void HubConnection::view_web_content_crashed(u64 view_id, WebView::ViewImplementation::WebContentCrashReason reason)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_web_content_crashed)
        view->on_web_content_crashed(move(reason));
}

void HubConnection::view_crash_overlay_state_change(u64 view_id, bool active)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_crash_overlay_state_change)
        view->on_crash_overlay_state_change(move(active));
}

void HubConnection::view_crash_report_saved(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_crash_report_saved)
        view->on_crash_report_saved();
}

void HubConnection::view_performance_stats(u64 view_id, WebView::TabPerformanceStats stats)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_performance_stats)
        view->on_performance_stats(move(stats));
}

}
