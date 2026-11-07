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
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PictureInPictureWindow.h>
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
    virtual SessionId create_session(String const& source_language, String const& target_language) override;
    virtual void close_session(SessionId) override;
    virtual void translate(SessionId, Web::TranslationUnit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) override;
    virtual void open_language_download_settings() override;

    HashMap<u64, Function<void(Optional<String>)>> m_pending_detections;
    HashMap<u64, Function<void(Vector<TargetLanguage>)>> m_pending_target_languages;
    HashMap<u64, PendingTranslation> m_pending_translations;
    u64 m_next_request_id { 1 };
    SessionId m_next_session_id { 1 };
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
