/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/View.h>
#include <LibCore/Promise.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/MicrophoneAuthorizer.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PictureInPictureWindow.h>

namespace Hub {

// What the application needs from the UI. With no UI, as in headless mode, it does without.
class ApplicationHost {
public:
    virtual ~ApplicationHost() = default;

    virtual void will_create_event_loop(Main::Arguments&) = 0;
    virtual void did_create_event_loop(Core::EventLoop&) = 0;

    virtual void create_platform_actions() = 0;
    virtual Optional<String> ui_font_family() const = 0;
    virtual Web::CSS::PreferredColorScheme interface_color_scheme() const = 0;
    virtual Optional<String> system_font_family() const = 0;
    virtual bool supports_picture_in_picture() const = 0;
    virtual bool supports_clipboard_type(WebView::Application::ClipboardType type) const = 0;
    virtual bool supports_system_menu_bar() const = 0;
    virtual bool supports_vertical_tabs() const = 0;
    virtual bool supports_private_browsing_windows() const = 0;
    virtual bool supports_client_side_window_decorations() const = 0;
    virtual bool platform_reports_scroll_momentum() const = 0;
    virtual bool platform_scrolls_elastically() const = 0;
    virtual bool platform_scrollbar_track_press_jumps() const = 0;
    virtual Vector<String> platform_preferred_languages() const = 0;
    virtual Optional<Gfx::GpuDeviceIdentity> platform_presentation_gpu_device() const = 0;
    virtual Vector<u64> platform_presentation_drm_format_modifiers() const = 0;
};

// The browser itself: its state, and the helper processes that do its work.
class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    // The host is going away, and takes no more calls.
    void detach_from_host() { m_host = nullptr; }

    View& create_view(View::InitialState, Optional<WebView::CanonicalTraversable&> = {});
    void destroy_view(u64 view_id);
    void destroy_all_views() { m_views.clear(); }

    void set_active_view(Optional<u64> view_id);

    // The UI has made its first windows and asked for what they start out showing.
    void ui_did_start();
    virtual void notify_webdriver_window_created(String const& handle) override;
    void set_ui_info(Hub::UIInfo);
    Hub::UIInfo const* ui_info() const;
    StringView ui_mach_server_name() const { return m_ui_mach_server_name; }
    bool ui_has_location_source() const;
    bool ui_has_translator() const;
    void did_resolve_external_url_handler(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird);
    void launch_external_url(u64 handler_id, URL::URL const&, Function<void(bool)>) const;
    void did_launch_external_url(u64 request_id, bool launched);
    void did_read_clipboard_text(u64 request_id, Utf16String);
    void did_read_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem);
    void did_get_download_path(u64 request_id, Optional<ByteString>);

private:
    explicit Application(ApplicationHost*);

    // ^WebView::Application
    virtual Core::EventLoop& create_platform_event_loop() override;
    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab, WebView::ViewImplementation const* related_view = nullptr) const override;
    virtual void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab, WebView::ViewImplementation const* source_view = nullptr) const override;
    virtual void activate_or_open_tab_with_url(URL::URL const&) const override;
    virtual void open_urls_in_new_tabs(ReadonlySpan<URL::URL>) const override;
    virtual void open_url_in_new_window(URL::URL const&, WebView::IsPrivate) override;
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor, Web::HTML::ActivateTab, WebView::ViewImplementation const* source_view = nullptr) const override;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, WebView::IsPrivate) override;
    virtual NonnullRefPtr<Core::Promise<ByteString>> ask_user_for_download_path(ByteString const& file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;
    virtual void set_clipboard_text(String, ClipboardType) override;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem) override;
    virtual void read_clipboard_text(ClipboardType, Function<void(Utf16String)>) const override;
    virtual void read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)>) const override;
    virtual void update_tabs_display() const override;
    virtual void did_clear_history_since(UnixDateTime) const override;
    virtual void quit_for_signal() override;
    virtual NonnullOwnPtr<WebView::MicrophoneAuthorizer> create_microphone_authorizer() override;
    virtual NonnullOwnPtr<WebView::NotificationBackend> create_notification_backend() override;
    virtual bool captures_browser_crashes() const override { return true; }
    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;
    virtual void create_platform_actions() override;
    virtual Optional<String> ui_font_family() const override;
    virtual Web::CSS::PreferredColorScheme interface_color_scheme() const override;
    virtual Optional<String> system_font_family() const override;
    virtual void focus_page_from_notification(WebView::WebContentPage& page) override;
    virtual void open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId originating_page, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private) override;
    virtual bool supports_picture_in_picture() const override;
    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& canonical_traversable, Gfx::IntSize video_size) override;
    virtual void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const override;
    virtual bool supports_clipboard_type(WebView::Application::ClipboardType type) const override;
    virtual bool supports_system_menu_bar() const override;
    virtual bool supports_vertical_tabs() const override;
    virtual bool supports_private_browsing_windows() const override;
    virtual bool supports_client_side_window_decorations() const override;
    virtual bool platform_reports_scroll_momentum() const override;
    virtual bool platform_scrolls_elastically() const override;
    virtual bool platform_scrollbar_track_press_jumps() const override;
    virtual Vector<String> platform_preferred_languages() const override;
    virtual Optional<Gfx::GpuDeviceIdentity> platform_presentation_gpu_device() const override;
    virtual Vector<u64> platform_presentation_drm_format_modifiers() const override;

    ApplicationHost* m_host { nullptr };
    StringView m_encoded_ui_info;
    StringView m_ui_mach_server_name;
    mutable Optional<Hub::UIInfo> m_ui_info;
    HashMap<u64, NonnullOwnPtr<View>> m_views;
    Optional<u64> m_active_view_id;
    bool m_ui_did_start { false };
    Vector<String> m_webdriver_windows_awaiting_ui;

    // What the Hub waits to hear from the UI.
    mutable HashMap<u64, Function<void(Utf16String)>> m_pending_clipboard_text_reads;
    mutable HashMap<u64, Function<void(Web::Clipboard::SystemClipboardItem)>> m_pending_clipboard_item_reads;
    mutable HashMap<u64, NonnullRefPtr<Core::Promise<ByteString>>> m_pending_download_paths;
    mutable HashMap<u64, WebView::ExternalURLHandlerCallback> m_pending_external_url_handlers;
    mutable HashMap<u64, Function<void(bool)>> m_pending_external_url_launches;
    struct NewTabSource {
        Optional<u64> view_id;
        WebView::IsPrivate is_private { WebView::IsPrivate::No };
    };
    NewTabSource new_tab_source(WebView::ViewImplementation const* source_view) const;

    mutable u64 m_next_ui_request_id { 1 };
};

}
