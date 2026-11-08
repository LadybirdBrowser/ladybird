/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <Hub/View.h>
#include <LibCore/GeolocationProvider.h>
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/GamepadSource.h>
#include <LibWebView/MicrophoneAuthorizer.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/SpellChecker.h>
#include <LibWebView/Translator.h>

namespace Hub {

// These stand in for what only the UI can do, as it has the windows and the desktop.

class PictureInPictureWindowInUI final : public WebView::PictureInPictureWindow {
public:
    PictureInPictureWindowInUI(View&, u64 owner_view_id, Gfx::IntSize video_size, Gfx::IntSize initial_size);
    virtual ~PictureInPictureWindowInUI() override;

    static PictureInPictureWindowInUI* find(u64 view_id);

    void did_resize(Gfx::IntSize);
    void did_close(bool video_keeps_playing);

private:
    virtual Gfx::IntSize size() const override { return m_size; }
    virtual String handle() const override { return m_view.handle(); }
    virtual void hide() override;
    virtual void set_video_size(Gfx::IntSize) override;

    View& m_view;
    u64 m_view_id { 0 };
    Gfx::IntSize m_size;
};

class NotificationBackendInUI final : public WebView::NotificationBackend {
public:
    NotificationBackendInUI();
    virtual ~NotificationBackendInUI() override;

    static NotificationBackendInUI* the();

private:
    virtual void show(WebView::PlatformNotification const&) override;
    virtual void close(u64 id) override;
};

class GeolocationProviderInUI final : public Core::GeolocationProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    GeolocationProviderInUI();
    virtual ~GeolocationProviderInUI() override;

    static GeolocationProviderInUI* the();

    void did_receive_position(u64 id, Core::GeolocationCoordinates);
    void did_receive_error(u64 id, Core::GeolocationError);
    void did_change_authorization(Core::GeolocationAuthorization);

private:
    enum class Kind : u8 {
        CurrentPosition,
        Watch,
    };
    // NB: A callback may stop or start a watch, so a client must stay alive while its callbacks run.
    struct Client : public RefCounted<Client> {
        Client(Kind kind, SuccessCallback on_success, ErrorCallback on_error)
            : kind(kind)
            , on_success(move(on_success))
            , on_error(move(on_error))
        {
        }

        Kind kind;
        SuccessCallback on_success;
        ErrorCallback on_error;
    };

    virtual ErrorOr<RequestId, Core::GeolocationError> request_current_position(SuccessCallback, ErrorCallback) override;
    virtual void cancel_current_position_request(RequestId) override;
    virtual ErrorOr<WatchId, Core::GeolocationError> start_watching_position(SuccessCallback, ErrorCallback) override;
    virtual void stop_watching_position(WatchId) override;
    virtual Core::GeolocationAuthorization authorization() const override { return m_authorization; }

    HashMap<u64, NonnullRefPtr<Client>> m_clients;
    u64 m_next_id { 1 };
    Core::GeolocationAuthorization m_authorization { Core::GeolocationAuthorization::Unknown };
};

// NB: The Hub never waits for the UI, so it keeps the last authorization the UI told it about.
class MicrophoneAuthorizerInUI final : public WebView::MicrophoneAuthorizer {
public:
    explicit MicrophoneAuthorizerInUI(WebView::MicrophoneAuthorization);
    virtual ~MicrophoneAuthorizerInUI() override;

    static MicrophoneAuthorizerInUI* the();

    void did_change_authorization(WebView::MicrophoneAuthorization);

private:
    virtual WebView::MicrophoneAuthorization authorization() const override { return m_authorization; }
    virtual void request_authorization(Function<void(WebView::MicrophoneAuthorization)> on_complete) override;
    // NB: The UI opens the system settings itself.
    virtual void open_system_settings() override { }

    WebView::MicrophoneAuthorization m_authorization;
    Vector<Function<void(WebView::MicrophoneAuthorization)>> m_pending_requests;
};

class GamepadSourceInUI final : public WebView::GamepadSource {
public:
    GamepadSourceInUI();
    virtual ~GamepadSourceInUI() override;

    static GamepadSourceInUI* the();

private:
    virtual bool start() override;
    virtual void set_polling(bool) override;
    // NB: The UI reports what changes as it sees it.
    virtual void poll() override { }
    virtual void rumble(Web::Gamepad::GamepadHandle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms) override;
    virtual void rumble_triggers(Web::Gamepad::GamepadHandle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms) override;
};

class TranslatorInUI final : public WebView::Translator {
public:
    TranslatorInUI();
    virtual ~TranslatorInUI() override;

    static TranslatorInUI* the();

    void did_detect_language(u64 request_id, Optional<String> language);
    void did_get_target_languages(u64 request_id, Vector<TargetLanguage>);
    void did_translate(u64 request_id, Optional<Vector<Web::TranslationRun>>);

private:
    struct PendingTranslation {
        SessionId session_id { 0 };
        Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete;
    };

    virtual void detect_language(Web::TranslationLanguageSample const&, Function<void(Optional<String>)> on_complete) override;
    virtual void target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete) override;
    virtual SessionId create_session(String const& source_language, String const& target_language, bool prefer_higher_quality) override;
    virtual void choose_session_model_again(SessionId, bool prefer_higher_quality) override;
    virtual void close_session(SessionId) override;
    virtual void translate(SessionId, Web::TranslationUnit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) override;
    virtual void open_language_download_settings() override;

    HashMap<u64, Function<void(Optional<String>)>> m_pending_detections;
    HashMap<u64, Function<void(Vector<TargetLanguage>)>> m_pending_target_languages;
    HashMap<u64, PendingTranslation> m_pending_translations;
    u64 m_next_request_id { 1 };
    SessionId m_next_session_id { 1 };
};

class SpellCheckerInUI final : public WebView::SpellChecker {
public:
    SpellCheckerInUI(bool learns_for_every_tag, bool can_suggest);
    virtual ~SpellCheckerInUI() override;

    static SpellCheckerInUI* the();

    void did_check(u64 request_id, Vector<Web::SpellingError>);
    void did_guess(u64 request_id, Vector<Utf16String>);
    void did_suggest(u64 request_id, Optional<Web::WritingSuggestion>);

private:
    virtual DocumentTag create_document_tag() override;
    virtual void close_document_tag(DocumentTag) override;
    virtual void check(DocumentTag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete) override;
    virtual void guesses(DocumentTag, Utf16String const& word, Function<void(Vector<Utf16String>)> on_complete) override;
    virtual void ignore(DocumentTag, Utf16String const& word) override;
    virtual void learn(DocumentTag, Utf16String const& word) override;
    virtual bool learns_for_every_tag() const override { return m_learns_for_every_tag; }
    virtual bool can_suggest() const override { return m_can_suggest; }
    virtual void suggest(DocumentTag, Utf16String const& text, u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete) override;

    HashMap<u64, Function<void(Vector<Web::SpellingError>)>> m_pending_checks;
    HashMap<u64, Function<void(Vector<Utf16String>)>> m_pending_guesses;
    HashMap<u64, Function<void(Optional<Web::WritingSuggestion>)>> m_pending_suggestions;
    u64 m_next_request_id { 1 };
    DocumentTag m_next_document_tag { 1 };
    bool m_learns_for_every_tag { true };
    bool m_can_suggest { false };
};

class ExternalURLHandlerInUI final : public WebView::ExternalURLHandler {
public:
    ExternalURLHandlerInUI(u64 handler_id, String application_name, bool is_ladybird);
    virtual ~ExternalURLHandlerInUI() override;

    virtual void launch(URL::URL const&, LaunchCallback) const override;

private:
    u64 m_handler_id { 0 };
};

}
