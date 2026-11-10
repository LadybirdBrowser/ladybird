/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/MicrophoneAuthorizer.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/ProcessSnapshot.h>
#include <LibWebView/ReportSubmission.h>
#include <LibWebView/Settings.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/WebCompatReport.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>

namespace Hub {

static ConnectionFromUI* s_the;

ConnectionFromUI::ConnectionFromUI(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<HubClientEndpoint, HubServerEndpoint>(*this, move(transport), 1)
{
    s_the = this;

    // NB: The UI reads the gamepads.
    WebView::GamepadManager::the().set_source(make<GamepadSourceInUI>());
    Application::settings().on_persist = [this](JsonValue const& settings) {
        async_settings_changed(settings.serialized().to_byte_string(), m_last_applied_settings_change_id);
    };
    WebView::ApplicationWebUI::set_callbacks({
        .on_open = [this](u64 id, String const& host, WebView::IsPrivate is_private) { async_web_ui_opened(id, host, is_private); },
        .on_message = [this](u64 id, String name, JsonValue data) { async_web_ui_message(id, move(name), move(data)); },
        .on_close = [this](u64 id) { async_web_ui_closed(id); },
    });
    (void)WebView::TabPerformanceMonitor::the();

    auto& browser_process = Application::the().browser_process();
    browser_process.on_new_tab = [this](auto const& urls) { async_app_open_new_tab(urls); };
    browser_process.on_new_window = [this](auto const& urls) { async_app_open_new_window(urls); };
}

ConnectionFromUI::~ConnectionFromUI()
{
    WebView::ApplicationWebUI::set_callbacks({});
    Application::settings().on_persist = nullptr;
    auto& browser_process = Application::the().browser_process();
    browser_process.on_new_tab = nullptr;
    browser_process.on_new_window = nullptr;
    s_the = nullptr;
}

ConnectionFromUI* ConnectionFromUI::the()
{
    return s_the;
}

void ConnectionFromUI::die()
{
    // The UI is gone, which ends the browser, however that came to be.
    Core::EventLoop::current().quit(0);
}

// What the UI knows an input event by, while the Hub has it. The event carries it through the view's queue, in place of
// the data about the event that only the UI can hold. Once the event is gone, the UI can let go of that data.
struct InputFromUI final : public Web::BrowserInputData {
    InputFromUI(u64 view_id, u64 input_id)
        : view_id(view_id)
        , input_id(input_id)
    {
    }

    virtual ~InputFromUI() override
    {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_release_input(view_id, input_id);
    }

    u64 view_id { 0 };
    u64 input_id { 0 };
};

u64 ConnectionFromUI::input_id_of(Web::BrowserInputData const& browser_data)
{
    return as<InputFromUI>(browser_data).input_id;
}

template<typename EventType>
static void enqueue_input_from_ui(View& view, u64 input_id, EventType event)
{
    event.browser_data = make<InputFromUI>(view.view_id(), input_id);
    view.enqueue_input_event(move(event));
    view.push_state_to_ui();
}

void ConnectionFromUI::view_enqueue_key_event(u64 view_id, u64 input_id, Web::KeyEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_drag_event(u64 view_id, u64 input_id, Web::DragEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_pinch_event(u64 view_id, Web::PinchEvent event)
{
    if (auto* view = find_view(view_id)) {
        view->enqueue_input_event(move(event));
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::release_backing_store(u64 page_id, i32 bitmap_id)
{
    // NB: A page can outlive its view, and a view can stop showing a page, so this goes by the page.
    Web::PageId id { page_id };
    auto client = WebView::WebContentClient::client_for_compositor_context_id(Web::compositor_context_id_for_page(id));
    if (!client.has_value())
        return;
    if (auto* page = client->page(id))
        page->release_presented_bitmap(bitmap_id);
}

void ConnectionFromUI::view_answer(u64 view_id, u64 answer_id, bool answer)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_answer(answer_id, answer);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_answer_permission_request(u64 view_id, u64 answer_id, Web::Permissions::PromptOutcome outcome)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_permission_answer(answer_id, outcome);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::active_view_changed(Optional<u64> view_id)
{
    Application::the().set_active_view(view_id);
}

void ConnectionFromUI::answer_clipboard_text(u64 request_id, Utf16String text)
{
    Application::the().did_read_clipboard_text(request_id, move(text));
}

void ConnectionFromUI::answer_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item)
{
    Application::the().did_read_clipboard_item(request_id, move(item));
}

void ConnectionFromUI::answer_download_path(u64 request_id, Optional<ByteString> path)
{
    Application::the().did_get_download_path(request_id, move(path));
}

static DownloadState state_of_download(WebView::FileDownloader::Download const& download)
{
    DownloadState state;
    state.id = download.id;
    state.is_private = download.is_private;
    state.url = download.url;
    state.destination = download.destination.string();
    state.status = download.status;
    state.downloaded_size = download.downloaded_size;
    state.total_size = download.total_size;
    state.error = download.error;
    state.can_resume = download.can_resume;
    state.connection_count = download.connection_count;
    state.bytes_per_second = download.bytes_per_second;
    state.is_waiting_to_retry = download.is_waiting_to_retry;
    state.created_time = download.created_time;
    state.segments = Application::the().file_downloader().segment_progress(download.id);
    return state;
}

Vector<DownloadState> ConnectionFromUI::downloads()
{
    Vector<DownloadState> downloads;
    for (auto const& download : Application::the().file_downloader().downloads())
        downloads.append(state_of_download(download));
    return downloads;
}

void ConnectionFromUI::download_added(WebView::FileDownloader::Download const& download)
{
    async_download_added(state_of_download(download));
}

void ConnectionFromUI::download_updated(WebView::FileDownloader::Download const& download)
{
    async_download_updated(state_of_download(download));
}

void ConnectionFromUI::download_removed(u64 id, WebView::IsPrivate is_private)
{
    async_download_removed(id, is_private);
}

void ConnectionFromUI::cancel_download(u64 id)
{
    Application::the().file_downloader().cancel_download(id);
}

void ConnectionFromUI::pause_download(u64 id)
{
    Application::the().file_downloader().pause_download(id);
}

void ConnectionFromUI::resume_download(u64 id)
{
    Application::the().file_downloader().resume_download(id);
}

void ConnectionFromUI::prune_inactive_downloads(WebView::IsPrivate is_private)
{
    (void)Application::the().file_downloader().prune_inactive_downloads(is_private);
}

void ConnectionFromUI::pause_active_downloads()
{
    Application::the().file_downloader().pause_active_downloads();
}

void ConnectionFromUI::cancel_unresumable_downloads()
{
    Application::the().file_downloader().cancel_unresumable_downloads();
}

void ConnectionFromUI::pip_did_resize(u64 view_id, Gfx::IntSize size)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_resize(size);
}

void ConnectionFromUI::pip_did_close(u64 view_id, bool video_keeps_playing)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_close(video_keeps_playing);
}

void ConnectionFromUI::geolocation_position(u64 id, Core::GeolocationCoordinates coordinates)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_receive_position(id, move(coordinates));
}

void ConnectionFromUI::geolocation_error(u64 id, Core::GeolocationError error)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_receive_error(id, move(error));
}

void ConnectionFromUI::geolocation_authorization_changed(Core::GeolocationAuthorization authorization)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_change_authorization(authorization);
}

void ConnectionFromUI::microphone_authorization_changed(WebView::MicrophoneAuthorization authorization)
{
    if (auto* authorizer = MicrophoneAuthorizerInUI::the())
        authorizer->did_change_authorization(authorization);
}

void ConnectionFromUI::gamepad_connected(Web::Gamepad::GamepadDescription description, Web::Gamepad::GamepadState state)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_connected)
        source->on_gamepad_connected(move(description), move(state));
}

void ConnectionFromUI::gamepad_disconnected(u32 handle)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_disconnected)
        source->on_gamepad_disconnected(handle);
}

void ConnectionFromUI::gamepad_states_changed(Vector<Web::Gamepad::GamepadState> states)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_states_changed)
        source->on_gamepad_states_changed(move(states));
}

void ConnectionFromUI::translator_language_detected(u64 request_id, Optional<String> language)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_detect_language(request_id, move(language));
}

void ConnectionFromUI::translator_target_languages_ready(u64 request_id, Vector<WebView::Translator::TargetLanguage> target_languages)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_get_target_languages(request_id, move(target_languages));
}

void ConnectionFromUI::translator_translation_finished(u64 request_id, Optional<Vector<Web::TranslationRun>> runs)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_translate(request_id, move(runs));
}

void ConnectionFromUI::translator_languages_changed()
{
    if (auto* translator = TranslatorInUI::the(); translator && translator->on_languages_changed)
        translator->on_languages_changed();
}

void ConnectionFromUI::spell_checker_checked(u64 request_id, Vector<Web::SpellingError> errors)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_check(request_id, move(errors));
}

void ConnectionFromUI::spell_checker_guessed(u64 request_id, Vector<Utf16String> guesses)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_guess(request_id, move(guesses));
}

void ConnectionFromUI::spell_checker_suggested(u64 request_id, Optional<Web::WritingSuggestion> suggestion)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_suggest(request_id, move(suggestion));
}

void ConnectionFromUI::system_time_zone_changed()
{
    WebView::WebContentClient::for_each_client([](WebView::WebContentClient& client) {
        client.async_system_time_zone_changed();
        return IterationDecision::Continue;
    });
}

void ConnectionFromUI::notification_shown(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_shown)
        backend->on_shown(id);
}

void ConnectionFromUI::notification_failed(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_failed)
        backend->on_failed(id);
}

void ConnectionFromUI::notification_clicked(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_clicked)
        backend->on_clicked(id);
}

void ConnectionFromUI::notification_closed(u64 id, bool by_user)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_closed)
        backend->on_closed(id, by_user ? WebView::NotificationBackend::ByUser::Yes : WebView::NotificationBackend::ByUser::No);
}

void ConnectionFromUI::external_url_handler_resolved(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird)
{
    Application::the().did_resolve_external_url_handler(request_id, handler_id, move(application_name), is_ladybird);
}

void ConnectionFromUI::external_url_launched(u64 request_id, bool launched)
{
    Application::the().did_launch_external_url(request_id, launched);
}

void ConnectionFromUI::view_set_site_permission(u64 view_id, URL::Origin origin, Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision)
{
    if (auto* view = find_view(view_id)) {
        view->set_site_permission(origin, name, decision);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_did_show_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request)
{
    if (auto* view = find_view(view_id))
        view->did_show_permission_prompt(request);
}

void ConnectionFromUI::view_load_navigation(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation)
{
    if (auto* view = find_view(view_id))
        view->load(move(navigation));
}

void ConnectionFromUI::view_open_navigation_in_new_tab(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab)
{
    if (auto* view = find_view(view_id))
        view->open_navigation_in_new_tab(move(navigation), activate_tab);
}

void ConnectionFromUI::view_open_navigation_in_new_window(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    if (auto* view = find_view(view_id))
        view->open_navigation_in_new_window(move(navigation), is_private);
}

void ConnectionFromUI::view_traverse_the_history_to_step(u64 view_id, i32 step)
{
    if (auto* view = find_view(view_id))
        view->traverse_the_history_to_step(step);
}

void ConnectionFromUI::view_set_reports_session_history(u64 view_id, bool reports_session_history)
{
    if (auto* view = find_view(view_id))
        view->set_reports_session_history(reports_session_history);
}

void ConnectionFromUI::view_file_picker_closed(u64 view_id, Vector<Web::HTML::SelectedFile> selected_files)
{
    if (auto* view = find_view(view_id))
        view->file_picker_closed(move(selected_files));
}

void ConnectionFromUI::view_save_screenshot(u64 view_id, Gfx::ShareableBitmap bitmap)
{
    auto* view = find_view(view_id);
    if (view && bitmap.is_valid())
        view->save_screenshot(*bitmap.bitmap());
}

void ConnectionFromUI::view_request_selected_text(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_selected_text(view_id, request_id, {});
        return;
    }
    auto promise = view->selected_text();
    promise->when_resolved([this, view_id, request_id](ByteString& text) {
        async_view_selected_text(view_id, request_id, text);
    });
    promise->when_rejected([this, view_id, request_id](Error const&) {
        async_view_selected_text(view_id, request_id, {});
    });
}

void ConnectionFromUI::view_request_close_preflight(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_close_preflight_finished(view_id, request_id, true);
        return;
    }
    view->request_close_preflight([this, view_id, request_id](bool may_close) {
        async_view_close_preflight_finished(view_id, request_id, may_close);
    });
}

void ConnectionFromUI::view_request_page_dumps(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_page_dumps(view_id, request_id, {});
        return;
    }
    auto promise = WebView::WebCompatReport::request_page_dumps(*view);
    promise->when_resolved([this, view_id, request_id](String& dumps) {
        async_view_page_dumps(view_id, request_id, dumps);
    });
    promise->when_rejected([this, view_id, request_id](Error const&) {
        async_view_page_dumps(view_id, request_id, {});
    });
}

void ConnectionFromUI::view_prepare_for_immediate_close(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return;
    // NB: The UI looked before it asked, but a page can start to need a beforeunload check at any moment, and the user
    //     still wants the tab gone. A page must not be able to take the Hub down by racing that, and the UI has
    //     already closed the tab, so the page closes without prompting.
    if (view->needs_beforeunload_check()) {
        view->prepare_for_close_without_prompting()();
        return;
    }
    view->prepare_for_immediate_close();
}

void ConnectionFromUI::view_prepare_for_close_without_prompting(u64 view_id, u64 close_id)
{
    if (auto* view = find_view(view_id))
        m_prepared_closes.set(close_id, view->prepare_for_close_without_prompting());
}

void ConnectionFromUI::finish_prepared_close(u64 close_id)
{
    if (auto close = m_prepared_closes.take(close_id); close.has_value())
        (*close)();
}

void ConnectionFromUI::add_web_ui_page(String host, String title)
{
    WebView::WebUI::add_application_page(move(host), move(title));
}

void ConnectionFromUI::send_web_ui_message(u64 web_ui_id, String name, JsonValue data)
{
    if (auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id))
        web_ui->send_to_page(name, data);
}

Messages::HubServer::WebUiConsumeUserActivationResponse ConnectionFromUI::web_ui_consume_user_activation(u64 web_ui_id)
{
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    return web_ui && web_ui->consume_user_activation();
}

Messages::HubServer::InitializeResponse ConnectionFromUI::initialize()
{
    auto const& options = Application::browser_options();

    InitialState state;
    state.settings_json = Application::settings().serialize_json().serialized().to_byte_string();
    state.user_agent = Application::the().user_agent_string();
    state.navigator_compatibility_mode = Application::the().navigator_compatibility_mode();
    state.scripting_enabled = Application::the().scripting_enabled();
    state.blocks_pop_ups = Application::the().blocks_pop_ups();
    state.devtools_enabled = Application::the().devtools_enabled();
    state.sandbox_is_disabled = options.disable_sandbox == WebView::DisableSandbox::Yes;
    state.downloads = downloads();
    state.urls = options.urls;
    state.raw_urls = options.raw_urls;
    state.crash_report_directory = WebView::CrashReportStore::the().directory();
    state.devtools_port = options.devtools_port;
    auto const& profile = Application::profile();
    state.config_directory = profile.paths().config;
    state.data_directory = profile.paths().data;
    state.persists_sessions = Application::the().persists_sessions();
    state.uses_sql_database = options.disable_sql_database == WebView::DisableSQLDatabase::No;
    state.profile_identifier = profile.identifier();
    state.profile_identity = profile.paths().identity;
    state.profile_is_temporary = profile.is_temporary();
    return state;
}

Messages::HubServer::CreateViewResponse ConnectionFromUI::create_view(Hub::NewViewRequest request)
{
    auto& view = Application::the().create_view({
        .is_private = request.is_private,
        .device_pixel_ratio = request.device_pixel_ratio,
        .maximum_frames_per_second = request.maximum_frames_per_second,
        .display_id = request.display_id,
        .system_theme = move(request.system_theme),
        .screen_rects = move(request.screen_rects),
        .owner_view_id = request.owner_view_id,
        .websites_prefer_dark = request.websites_prefer_dark,
        .is_tab = request.is_tab,
    });
    return { view.view_id(), view.current_state() };
}

Messages::HubServer::ViewDescriptionResponse ConnectionFromUI::view_description(u64 view_id)
{
    // NB: The UI can ask about a view that the Hub has let go of already, and must not take the Hub down by that.
    auto* view = find_view(view_id);
    if (!view)
        return Hub::ViewState {};
    return { view->current_state() };
}

void ConnectionFromUI::crash_reports_changed()
{
    async_crash_reports_changed();
}

void ConnectionFromUI::ui_did_start()
{
    Application::the().ui_did_start();
}

// NB: The UI makes no requests of its own choosing. It names a search engine and a query, and the Hub makes the request
//     for that engine's suggestions, as the UI's own settings say.
void ConnectionFromUI::fetch_search_suggestions(u64 request_id, String engine_name, String query, WebView::IsPrivate is_private)
{
    Optional<URL::URL> url;
    if (auto engine = Application::settings().find_search_engine_by_name(engine_name); engine.has_value())
        url = engine->suggestions_url_for_query(query);
    if (!url.has_value()) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }
    static constexpr Array response_header_names { "Content-Type"sv };
    start_request(request_id, "GET"sv, *url, *HTTP::HeaderList::create(), {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, is_private, response_header_names);
}

// NB: A report goes only to the report server, with no credentials, and with only the headers that reports carry.
void ConnectionFromUI::post_report(u64 request_id, URL::URL url, Vector<HTTP::Header> headers, ByteString body)
{
    if (!WebView::ReportSubmission::is_report_server_url(url)) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }
    static constexpr Array report_header_names { "Content-Type"sv, "X-Ladybird-Challenge"sv, "X-Ladybird-Nonce"sv };
    auto request_headers = HTTP::HeaderList::create();
    for (auto& header : headers) {
        if (any_of(report_header_names, [&](auto name) { return header.name.equals_ignoring_ascii_case(name); }))
            request_headers->append(move(header));
    }
    static constexpr Array response_header_names { "Retry-After"sv };
    start_request(request_id, "POST"sv, url, *request_headers, body, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, WebView::IsPrivate::No, response_header_names);
}

void ConnectionFromUI::start_request(u64 request_id, ByteString const& method, URL::URL const& url, HTTP::HeaderList const& headers, ByteString const& body, HTTP::CacheMode cache_mode, HTTP::Cookie::IncludeCredentials include_credentials, WebView::IsPrivate is_private, ReadonlySpan<StringView> response_header_names)
{
    RefPtr<Requests::Request> request;
    if (auto client = WebView::Application::request_server_client(is_private); !client.is_error())
        request = client.value()->start_request(method, url, HTTP::Cookie::SameSiteContext::same_site(), headers, body.bytes(), cache_mode, include_credentials);
    if (!request) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }

    request->set_buffered_request_finished_callback(
        [this, request_id, response_header_names](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error,
            HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const&,
            Requests::CacheState, Core::ImmutableBytes payload) {
            // NB: The UI gets only the headers that the response is read for.
            Vector<HTTP::Header> headers_for_ui;
            for (auto name : response_header_names) {
                if (auto value = response_headers.get(name); value.has_value())
                    headers_for_ui.append({ name, value.release_value() });
            }
            async_request_finished(request_id, network_error.has_value(), response_code, move(headers_for_ui), ByteString::copy(payload.bytes()));

            // A request cannot be destroyed from inside its own callback.
            Core::deferred_invoke([this, request_id] {
                m_requests.remove(request_id);
            });
        });
    m_requests.set(request_id, request.release_nonnull());
}

void ConnectionFromUI::stop_request(u64 request_id)
{
    if (auto request = m_requests.take(request_id); request.has_value())
        (*request)->stop();
}

void ConnectionFromUI::toggle_devtools_enabled()
{
    if (auto result = Application::the().toggle_devtools_enabled(); result.is_error())
        static_cast<WebView::Application&>(Application::the()).display_error_dialog(MUST(String::formatted("Unable to start DevTools: {}", result.error())));
}

void ConnectionFromUI::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    Application::the().set_preferred_contrast(contrast);
}

void ConnectionFromUI::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    Application::the().set_preferred_motion(motion);
}

void ConnectionFromUI::set_user_agent_string(String user_agent)
{
    Application::the().set_user_agent_string(move(user_agent));
}

void ConnectionFromUI::set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    Application::the().set_navigator_compatibility_mode(mode);
}

void ConnectionFromUI::set_scripting_enabled(bool enabled)
{
    Application::the().set_scripting_enabled(enabled);
}

void ConnectionFromUI::set_blocks_pop_ups(bool blocks)
{
    Application::the().set_blocks_pop_ups(blocks);
}

void ConnectionFromUI::set_shows_line_box_borders(bool shows)
{
    Application::the().set_shows_line_box_borders(shows);
}

void ConnectionFromUI::set_shows_caret_hit_test_debug_overlay(bool shows)
{
    Application::the().set_shows_caret_hit_test_debug_overlay(shows);
}

void ConnectionFromUI::dump_cookies()
{
    Application::the().dump_cookies();
}

void ConnectionFromUI::crash_compositor_process()
{
    Application::the().crash_compositor_process();
}

void ConnectionFromUI::launch_devtools_client()
{
    if (auto result = Application::the().launch_devtools_client(); result.is_error())
        dbgln("Unable to launch DevTools client: {}", result.error());
}

void ConnectionFromUI::reset_private_browsing_session()
{
    Application::the().reset_private_browsing_session();
}

void ConnectionFromUI::notify_compositor_gpu_presentation_unavailable()
{
    Application::the().notify_compositor_gpu_presentation_unavailable();
}

void ConnectionFromUI::ui_info_changed(Hub::UIInfo info)
{
    Application::the().set_ui_info(move(info));
}

Messages::HubServer::ProcessSnapshotsResponse ConnectionFromUI::process_snapshots()
{
    auto snapshots = WebView::process_snapshots(Application::process_manager());
    for (auto& snapshot : snapshots) {
        if (snapshot.type == WebView::ProcessType::Browser)
            snapshot.type = WebView::ProcessType::Hub;
    }
    return snapshots;
}

Messages::HubServer::TerminateProcessResponse ConnectionFromUI::terminate_process(u64 child_id)
{
    return WebView::end_process(Application::process_manager(), child_id);
}

Messages::HubServer::ViewNeedsBeforeunloadCheckResponse ConnectionFromUI::view_needs_beforeunload_check(u64 view_id)
{
    auto* view = find_view(view_id);
    return view && view->needs_beforeunload_check();
}

Messages::HubServer::ViewSessionHistoryTraversalItemsResponse ConnectionFromUI::view_session_history_traversal_items(u64 view_id, i32 direction)
{
    if (auto* view = find_view(view_id))
        return view->session_history_traversal_items(direction);
    return Vector<WebView::ViewImplementation::SessionHistoryTraversalItem> {};
}

Messages::HubServer::ViewSessionHistorySnapshotResponse ConnectionFromUI::view_session_history_snapshot(u64 view_id)
{
    if (auto* view = find_view(view_id))
        return Optional<WebView::SessionHistorySnapshot> { view->session_history_snapshot() };
    return Optional<WebView::SessionHistorySnapshot> {};
}

Messages::HubServer::ViewRestoreSessionHistoryFromSnapshotResponse ConnectionFromUI::view_restore_session_history_from_snapshot(u64 view_id, WebView::SessionHistorySnapshot snapshot)
{
    auto* view = find_view(view_id);
    return view && !view->restore_session_history_from_snapshot(move(snapshot)).is_error();
}

Messages::HubServer::ViewHistoryStepAtDeltaResponse ConnectionFromUI::view_history_step_at_delta(u64 view_id, i32 delta)
{
    auto* view = find_view(view_id);
    if (!view)
        return Optional<i32> {};
    auto const& traversable = view->traversable();
    auto used_steps = traversable.session_history().used_steps();
    auto step_index = traversable.effective_current_session_history_step_index() + delta;
    if (step_index >= used_steps.size())
        return Optional<i32> {};
    return Optional<i32> { used_steps[step_index] };
}

Messages::HubServer::ViewLookUpSelectedTextAtResponse ConnectionFromUI::view_look_up_selected_text_at(u64 view_id, Gfx::IntPoint position)
{
    auto* view = find_view(view_id);
    return view && view->look_up_selected_text_at(position);
}

Messages::HubServer::ViewPageDescriptionResponse ConnectionFromUI::view_page_description(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return { URL::URL {}, String {}, Gfx::IntSize {}, 1.0, 1.0 };
    auto page = WebView::WebCompatReport::describe_page(*view);
    return { move(page.url), move(page.user_agent), page.viewport_size, page.device_pixel_ratio, page.zoom_level };
}

void ConnectionFromUI::update_settings(u64 change_id, ByteString changes_json)
{
    auto changes = JsonValue::from_string(changes_json);
    if (changes.is_error() || !changes.value().is_object())
        return;

    // NB: This process owns the settings, so what the UI did not change stays as it is here, however old its copy is.
    auto settings = Application::settings().serialize_json();
    WebView::Settings::apply_changes(settings.as_object(), changes.value().as_object());
    m_last_applied_settings_change_id = change_id;
    Application::settings().replace_with_json(settings, WebView::Settings::PersistChange::Yes);
}

void ConnectionFromUI::destroy_view(u64 view_id)
{
    Application::the().destroy_view(view_id);
}

void ConnectionFromUI::view_set_viewport(u64 view_id, Web::DevicePixelSize size, double device_pixel_ratio)
{
    if (auto* view = find_view(view_id))
        view->set_viewport(size, device_pixel_ratio);
}

void ConnectionFromUI::view_set_system_theme(u64 view_id, Core::AnonymousBuffer theme)
{
    if (auto* view = find_view(view_id))
        view->set_system_theme(move(theme));
}

void ConnectionFromUI::view_set_screen_rects(u64 view_id, WebView::ViewImplementation::ScreenRects screen_rects)
{
    if (auto* view = find_view(view_id))
        view->set_screen_rects(move(screen_rects));
}

void ConnectionFromUI::view_set_page_background_color_to_system_canvas(u64 view_id, bool dark)
{
    if (auto* view = find_view(view_id)) {
        view->set_page_background_color_to_system_canvas(dark);
        view->push_state_to_ui();
    }
}

View* ConnectionFromUI::find_view(u64 view_id)
{
    auto view = WebView::ViewImplementation::find_view_by_id(view_id);
    if (!view.has_value())
        return nullptr;
    return static_cast<View*>(&view.value());
}

void ConnectionFromUI::view_load(u64 view_id, URL::URL url, Web::Bindings::NavigationHistoryBehavior history_behavior)
{
    if (auto* view = find_view(view_id)) {
        view->load(url, history_behavior);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_html(u64 view_id, ByteString html)
{
    if (auto* view = find_view(view_id)) {
        view->load_html(html);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page(u64 view_id, Utf16String query, AK::CaseSensitivity case_sensitivity, bool highlight_all_matches)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page(query, case_sensitivity, highlight_all_matches);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_position(u64 view_id, Gfx::IntPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_position(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_size(u64 view_id, Gfx::IntSize size)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_size(size);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_system_visibility_state(u64 view_id, Web::HTML::VisibilityState visibility_state)
{
    if (auto* view = find_view(view_id)) {
        view->set_system_visibility_state(visibility_state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_presents_from_cpu_memory(u64 view_id, bool presents_from_cpu_memory)
{
    if (auto* view = find_view(view_id)) {
        view->set_presents_from_cpu_memory(presents_from_cpu_memory);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_has_system_focus(u64 view_id, bool has_system_focus)
{
    if (auto* view = find_view(view_id)) {
        view->set_has_system_focus(has_system_focus);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_tab(u64 view_id, URL::URL url, Web::HTML::ActivateTab activate_tab)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_tab(url, activate_tab);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_window(u64 view_id, URL::URL url, WebView::IsPrivate is_private)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_window(url, is_private);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_next_history_visit_transition(u64 view_id, WebView::HistoryVisitTransition transition)
{
    if (auto* view = find_view(view_id)) {
        view->set_next_history_visit_transition(transition);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reload(u64 view_id, WebView::BypassCache bypass_cache)
{
    if (auto* view = find_view(view_id)) {
        view->reload(bypass_cache);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_undo(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->undo();
}

void ConnectionFromUI::view_redo(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->redo();
}

void ConnectionFromUI::view_cut_selection(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->cut_selected_text()->when_resolved([](auto& text) {
            if (!text.is_empty())
                Application::the().insert_clipboard_entry({ "text/plain"_string, text });
        });
    }
}

void ConnectionFromUI::view_paste(u64 view_id, Web::Clipboard::PasteMode paste_mode)
{
    if (auto* view = find_view(view_id))
        view->paste_from_clipboard(paste_mode);
}

void ConnectionFromUI::view_select_all(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->select_all();
}

void ConnectionFromUI::view_get_source(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->get_source();
}

void ConnectionFromUI::view_traverse_the_history_by_delta(u64 view_id, i32 delta)
{
    if (auto* view = find_view(view_id))
        view->traverse_the_history_by_delta(delta);
}

void ConnectionFromUI::view_translate_to(u64 view_id, String target_language)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().translate_to(target_language);
}

void ConnectionFromUI::view_show_original(u64 view_id)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().show_original();
}

void ConnectionFromUI::view_download_translation_languages(u64 view_id, String target_language)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().download_languages_for(target_language);
}

void ConnectionFromUI::view_prefer_higher_quality_translation(u64 view_id, bool prefers_higher_quality)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().prefer_higher_quality_models(prefers_higher_quality);
}

void ConnectionFromUI::view_download_url(u64 view_id, URL::URL url, bool prompt_for_path, Optional<String> image_mime_type)
{
    if (auto* view = find_view(view_id))
        view->download_url(url, prompt_for_path ? WebView::ViewImplementation::PromptForPath::Yes : WebView::ViewImplementation::PromptForPath::No, image_mime_type);
}

void ConnectionFromUI::view_inspect_context_menu_element(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->inspect_context_menu_element();
}

void ConnectionFromUI::view_replace_context_menu_misspelling(u64 view_id, Utf16String replacement)
{
    if (auto* view = find_view(view_id))
        view->replace_context_menu_misspelling(replacement);
}

void ConnectionFromUI::view_ignore_misspelling(u64 view_id, Utf16String word)
{
    if (auto* view = find_view(view_id))
        view->ignore_misspelling(word);
}

void ConnectionFromUI::view_learn_misspelling(u64 view_id, Utf16String word)
{
    if (auto* view = find_view(view_id))
        view->learn_misspelling(word);
}

void ConnectionFromUI::view_do_context_menu_media_action(u64 view_id, WebView::ContextMenuMediaAction action)
{
    if (auto* view = find_view(view_id))
        view->do_context_menu_media_action(action);
}

void ConnectionFromUI::view_debug_request(u64 view_id, ByteString request, ByteString argument)
{
    if (auto* view = find_view(view_id))
        view->debug_request(request, argument);
}

void ConnectionFromUI::view_dump_gc_graph(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return;

    auto gc_graph_path = view->dump_gc_graph();
    if (gc_graph_path.is_error()) {
        warnln("\033[31;1mFailed to dump GC graph: {}\033[0m", gc_graph_path.error());
        return;
    }
    warnln("\033[33;1mDumped GC graph into {}\033[0m", gc_graph_path.value());
    if (auto source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv); source_dir.has_value())
        warnln("\033[33;1mGC graph explorer: file://{}/Meta/gc-heap-explorer.html?script=file://{}\033[0m", *source_dir, gc_graph_path.value());
}

void ConnectionFromUI::view_stop_loading(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->stop_loading();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_in(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_in();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_out(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_out();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_zoom(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->reset_zoom();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_display_metadata(u64 view_id, Optional<u64> display_id, double maximum_frames_per_second)
{
    if (auto* view = find_view(view_id)) {
        view->set_display_metadata(display_id, maximum_frames_per_second);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_scales_frames_to_fit(u64 view_id, bool scales_frames_to_fit)
{
    if (auto* view = find_view(view_id)) {
        view->set_scales_frames_to_fit(scales_frames_to_fit);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_enable_picture_in_picture_controls(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->enable_picture_in_picture_controls();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_update_interface_color_scheme(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->update_interface_color_scheme();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_next_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_next_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_previous_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_previous_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_end(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_end();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_report_website_issue(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->report_website_issue();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_accessibility_tree(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_accessibility_tree();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_perform_accessibility_action(u64 view_id, i64 node_id, String action)
{
    if (auto* view = find_view(view_id)) {
        view->perform_accessibility_action(node_id, action);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_clear_node_picker(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->clear_node_picker();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_hover(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_hover(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_pick(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_pick(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_preview(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_preview(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_cancel(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_cancel();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_alert_closed(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->alert_closed();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_confirm_closed(u64 view_id, bool accepted)
{
    if (auto* view = find_view(view_id)) {
        view->confirm_closed(accepted);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_prompt_closed(u64 view_id, Optional<Utf16String> response)
{
    if (auto* view = find_view(view_id)) {
        view->prompt_closed(response);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_color_picker_update(u64 view_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    if (auto* view = find_view(view_id)) {
        view->color_picker_update(picked_color, state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_date_picker_closed(u64 view_id, Optional<Utf16String> picked_value)
{
    if (auto* view = find_view(view_id)) {
        view->date_picker_closed(picked_value);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_select_dropdown_closed(u64 view_id, Optional<u32> selected_item_id)
{
    if (auto* view = find_view(view_id)) {
        view->select_dropdown_closed(selected_item_id);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_marked_text_from_input_method(u64 view_id, Utf16String text)
{
    if (auto* view = find_view(view_id)) {
        view->set_marked_text_from_input_method(text);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_commit_text_from_input_method(u64 view_id, Utf16String text, i32 replacement_start, i32 replacement_length)
{
    if (auto* view = find_view(view_id)) {
        view->commit_text_from_input_method(text, replacement_start, replacement_length);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_toggle_page_mute_state(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->toggle_page_mute_state();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_take_and_save_screenshot(u64 view_id, WebView::ViewImplementation::ScreenshotType screenshot_type)
{
    if (auto* view = find_view(view_id)) {
        view->take_and_save_screenshot(screenshot_type);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_close(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_close();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_deny_fullscreen_window(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->deny_fullscreen_window();
}

void ConnectionFromUI::view_exit_fullscreen(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->exit_fullscreen();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_is_fullscreen(u64 view_id, Web::ViewportIsFullscreen is_fullscreen)
{
    if (auto* view = find_view(view_id)) {
        view->set_is_fullscreen(is_fullscreen);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_site_permissions(u64 view_id, URL::Origin origin)
{
    if (auto* view = find_view(view_id)) {
        view->reset_site_permissions(origin);
        view->push_state_to_ui();
    }
}

}
