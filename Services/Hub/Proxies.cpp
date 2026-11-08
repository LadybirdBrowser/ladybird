/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibWebView/Application.h>

namespace Hub {

static HashMap<u64, PictureInPictureWindowInUI*> s_picture_in_picture_windows;

PictureInPictureWindowInUI::PictureInPictureWindowInUI(View& view, u64 owner_view_id, Gfx::IntSize video_size, Gfx::IntSize initial_size)
    : m_view(view)
    , m_view_id(view.view_id())
    , m_size(initial_size)
{
    s_picture_in_picture_windows.set(m_view_id, this);
    report_page_close_of(view);
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_open(m_view_id, owner_view_id, video_size);
}

PictureInPictureWindowInUI::~PictureInPictureWindowInUI()
{
    s_picture_in_picture_windows.remove(m_view_id);
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_close(m_view_id);
}

PictureInPictureWindowInUI* PictureInPictureWindowInUI::find(u64 view_id)
{
    return s_picture_in_picture_windows.get(view_id).value_or(nullptr);
}

void PictureInPictureWindowInUI::did_resize(Gfx::IntSize size)
{
    m_size = size;
    if (on_resize)
        on_resize(size);
}

void PictureInPictureWindowInUI::did_close(bool video_keeps_playing)
{
    if (on_close)
        on_close(video_keeps_playing ? VideoKeepsPlaying::Yes : VideoKeepsPlaying::No);
}

void PictureInPictureWindowInUI::hide()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_hide(m_view_id);
}

void PictureInPictureWindowInUI::set_video_size(Gfx::IntSize video_size)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_pip_set_video_size(m_view_id, video_size);
}

static NotificationBackendInUI* s_notification_backend;

NotificationBackendInUI::NotificationBackendInUI()
{
    s_notification_backend = this;
}

NotificationBackendInUI::~NotificationBackendInUI()
{
    s_notification_backend = nullptr;
}

NotificationBackendInUI* NotificationBackendInUI::the()
{
    return s_notification_backend;
}

void NotificationBackendInUI::show(WebView::PlatformNotification const& notification)
{
    Optional<Gfx::ShareableBitmap> icon;
    if (notification.icon)
        icon = notification.icon->to_shareable_bitmap();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_notification_show(notification.id, notification.page_id.value(), notification.replaces, notification.data, notification.attribution, move(icon));
}

void NotificationBackendInUI::close(u64 id)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_notification_close(id);
}

static GeolocationProviderInUI* s_geolocation_provider = nullptr;

GeolocationProviderInUI::GeolocationProviderInUI()
{
    s_geolocation_provider = this;
    if (auto const* info = Application::the().ui_info(); info && info->location_authorization.has_value())
        m_authorization = *info->location_authorization;
    // NB: The UI's provider tells the Hub whenever the authorization changes, but only once it exists.
    if (auto* ui = ConnectionFromUI::the())
        ui->async_geolocation_start();
}

GeolocationProviderInUI::~GeolocationProviderInUI()
{
    s_geolocation_provider = nullptr;
}

GeolocationProviderInUI* GeolocationProviderInUI::the()
{
    return s_geolocation_provider;
}

ErrorOr<Core::GeolocationProvider::RequestId, Core::GeolocationError> GeolocationProviderInUI::request_current_position(SuccessCallback on_success, ErrorCallback on_error)
{
    auto id = m_next_id++;
    m_clients.set(id, make_ref_counted<Client>(Kind::CurrentPosition, move(on_success), move(on_error)));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_geolocation_request_position(id);
    return id;
}

void GeolocationProviderInUI::cancel_current_position_request(RequestId id)
{
    if (!m_clients.remove(id))
        return;
    if (auto* ui = ConnectionFromUI::the())
        ui->async_geolocation_cancel_position_request(id);
}

ErrorOr<Core::GeolocationProvider::WatchId, Core::GeolocationError> GeolocationProviderInUI::start_watching_position(SuccessCallback on_success, ErrorCallback on_error)
{
    auto id = m_next_id++;
    m_clients.set(id, make_ref_counted<Client>(Kind::Watch, move(on_success), move(on_error)));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_geolocation_start_watching(id);
    return id;
}

void GeolocationProviderInUI::stop_watching_position(WatchId id)
{
    if (!m_clients.remove(id))
        return;
    if (auto* ui = ConnectionFromUI::the())
        ui->async_geolocation_stop_watching(id);
}

void GeolocationProviderInUI::did_receive_position(u64 id, Core::GeolocationCoordinates coordinates)
{
    RefPtr<Client> client = m_clients.get(id).value_or(nullptr);
    if (!client)
        return;
    if (client->kind == Kind::CurrentPosition)
        m_clients.remove(id);
    client->on_success(move(coordinates));
}

void GeolocationProviderInUI::did_receive_error(u64 id, Core::GeolocationError error)
{
    RefPtr<Client> client = m_clients.get(id).value_or(nullptr);
    if (!client)
        return;
    if (client->kind == Kind::CurrentPosition)
        m_clients.remove(id);
    client->on_error(move(error));
}

void GeolocationProviderInUI::did_change_authorization(Core::GeolocationAuthorization authorization)
{
    m_authorization = authorization;
    if (on_authorization_change)
        on_authorization_change();
}

static MicrophoneAuthorizerInUI* s_microphone_authorizer = nullptr;

MicrophoneAuthorizerInUI::MicrophoneAuthorizerInUI(WebView::MicrophoneAuthorization authorization)
    : m_authorization(authorization)
{
    s_microphone_authorizer = this;
}

MicrophoneAuthorizerInUI::~MicrophoneAuthorizerInUI()
{
    s_microphone_authorizer = nullptr;
}

MicrophoneAuthorizerInUI* MicrophoneAuthorizerInUI::the()
{
    return s_microphone_authorizer;
}

void MicrophoneAuthorizerInUI::request_authorization(Function<void(WebView::MicrophoneAuthorization)> on_complete)
{
    // NB: With no UI to ask, nothing would ever answer, and every later request would wait behind this one.
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        on_complete(m_authorization);
        return;
    }

    // Requests made while the system is asking share its answer.
    m_pending_requests.append(move(on_complete));
    if (m_pending_requests.size() > 1)
        return;
    ui->async_microphone_request_authorization();
}

void MicrophoneAuthorizerInUI::did_change_authorization(WebView::MicrophoneAuthorization authorization)
{
    m_authorization = authorization;
    if (authorization == WebView::MicrophoneAuthorization::NotDetermined)
        return;
    auto pending_requests = move(m_pending_requests);
    for (auto& on_complete : pending_requests)
        on_complete(authorization);
}

static GamepadSourceInUI* s_gamepad_source = nullptr;

GamepadSourceInUI::GamepadSourceInUI()
{
    s_gamepad_source = this;
}

GamepadSourceInUI::~GamepadSourceInUI()
{
    s_gamepad_source = nullptr;
}

GamepadSourceInUI* GamepadSourceInUI::the()
{
    return s_gamepad_source;
}

bool GamepadSourceInUI::start()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_gamepad_start(WebView::Application::web_content_options().is_test_mode == WebView::IsTestMode::Yes);
    return true;
}

void GamepadSourceInUI::set_polling(bool polling)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_gamepad_set_polling(polling);
}

void GamepadSourceInUI::rumble(Web::Gamepad::GamepadHandle handle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_gamepad_rumble(handle, low_frequency_magnitude, high_frequency_magnitude, duration_ms);
}

void GamepadSourceInUI::rumble_triggers(Web::Gamepad::GamepadHandle handle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_gamepad_rumble_triggers(handle, left_magnitude, right_magnitude, duration_ms);
}

static TranslatorInUI* s_translator = nullptr;

TranslatorInUI::TranslatorInUI()
{
    s_translator = this;
}

TranslatorInUI::~TranslatorInUI()
{
    s_translator = nullptr;
}

TranslatorInUI* TranslatorInUI::the()
{
    return s_translator;
}

void TranslatorInUI::detect_language(Web::TranslationLanguageSample const& sample, Function<void(Optional<String>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_detections.set(request_id, move(on_complete));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_detect_language(request_id, sample);
}

void TranslatorInUI::did_detect_language(u64 request_id, Optional<String> language)
{
    if (auto on_complete = m_pending_detections.take(request_id); on_complete.has_value())
        (*on_complete)(move(language));
}

void TranslatorInUI::target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_target_languages.set(request_id, move(on_complete));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_target_languages(request_id, source_language, candidates);
}

void TranslatorInUI::did_get_target_languages(u64 request_id, Vector<TargetLanguage> target_languages)
{
    if (auto on_complete = m_pending_target_languages.take(request_id); on_complete.has_value())
        (*on_complete)(move(target_languages));
}

// NB: The Hub never waits for the UI, so it picks the session's ID itself.
WebView::Translator::SessionId TranslatorInUI::create_session(String const& source_language, String const& target_language, bool prefer_higher_quality)
{
    auto session_id = m_next_session_id++;
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_create_session(session_id, source_language, target_language, prefer_higher_quality);
    return session_id;
}

void TranslatorInUI::choose_session_model_again(SessionId session_id, bool prefer_higher_quality)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_choose_session_model_again(session_id, prefer_higher_quality);
}

void TranslatorInUI::close_session(SessionId session_id)
{
    m_pending_translations.remove_all_matching([&](auto, auto const& translation) {
        return translation.session_id == session_id;
    });
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_close_session(session_id);
}

void TranslatorInUI::translate(SessionId session_id, Web::TranslationUnit unit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_translations.set(request_id, { session_id, move(on_complete) });
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_translate(request_id, session_id, unit);
}

void TranslatorInUI::did_translate(u64 request_id, Optional<Vector<Web::TranslationRun>> runs)
{
    if (auto translation = m_pending_translations.take(request_id); translation.has_value())
        translation->on_complete(move(runs));
}

void TranslatorInUI::open_language_download_settings()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_translator_open_language_download_settings();
}

static SpellCheckerInUI* s_spell_checker = nullptr;

SpellCheckerInUI::SpellCheckerInUI(bool learns_for_every_tag, bool can_suggest)
    : m_learns_for_every_tag(learns_for_every_tag)
    , m_can_suggest(can_suggest)
{
    s_spell_checker = this;
}

SpellCheckerInUI::~SpellCheckerInUI()
{
    s_spell_checker = nullptr;
}

SpellCheckerInUI* SpellCheckerInUI::the()
{
    return s_spell_checker;
}

// NB: The Hub never waits for the UI, so it picks the document's tag itself. The UI makes its own tag on first use.
WebView::SpellChecker::DocumentTag SpellCheckerInUI::create_document_tag()
{
    return m_next_document_tag++;
}

void SpellCheckerInUI::close_document_tag(DocumentTag tag)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_close_document(tag);
}

void SpellCheckerInUI::check(DocumentTag tag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_checks.set(request_id, move(on_complete));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_check(request_id, tag, text);
}

void SpellCheckerInUI::did_check(u64 request_id, Vector<Web::SpellingError> errors)
{
    if (auto on_complete = m_pending_checks.take(request_id); on_complete.has_value())
        (*on_complete)(move(errors));
}

void SpellCheckerInUI::guesses(DocumentTag tag, Utf16String const& word, Function<void(Vector<Utf16String>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_guesses.set(request_id, move(on_complete));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_guesses(request_id, tag, word);
}

void SpellCheckerInUI::did_guess(u64 request_id, Vector<Utf16String> guesses)
{
    if (auto on_complete = m_pending_guesses.take(request_id); on_complete.has_value())
        (*on_complete)(move(guesses));
}

void SpellCheckerInUI::ignore(DocumentTag tag, Utf16String const& word)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_ignore(tag, word);
}

void SpellCheckerInUI::learn(DocumentTag tag, Utf16String const& word)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_learn(tag, word);
}

void SpellCheckerInUI::suggest(DocumentTag tag, Utf16String const& text, u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete)
{
    auto request_id = m_next_request_id++;
    m_pending_suggestions.set(request_id, move(on_complete));
    if (auto* ui = ConnectionFromUI::the())
        ui->async_spell_checker_suggest(request_id, tag, text, caret_offset);
}

void SpellCheckerInUI::did_suggest(u64 request_id, Optional<Web::WritingSuggestion> suggestion)
{
    if (auto on_complete = m_pending_suggestions.take(request_id); on_complete.has_value())
        (*on_complete)(move(suggestion));
}

ExternalURLHandlerInUI::ExternalURLHandlerInUI(u64 handler_id, String application_name, bool is_ladybird)
    : ExternalURLHandler(move(application_name), is_ladybird)
    , m_handler_id(handler_id)
{
}

ExternalURLHandlerInUI::~ExternalURLHandlerInUI()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_external_url_release_handler(m_handler_id);
}

void ExternalURLHandlerInUI::launch(URL::URL const& url, LaunchCallback callback) const
{
    Application::the().launch_external_url(m_handler_id, url, move(callback));
}

}
