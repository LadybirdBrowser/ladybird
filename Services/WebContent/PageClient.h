/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Forward.h>
#include <WebContent/BackingStoreManager.h>
#include <WebContent/Forward.h>

namespace WebContent {

class PageClient final : public Web::PageClient {
    GC_CELL(PageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(PageClient);

public:
    static GC::Ref<PageClient> create(JS::VM& vm, PageHost& page_host, u64 id);

    virtual ~PageClient() override;

    enum class UseSkiaPainter {
        CPUBackend,
        GPUBackendIfAvailable,
    };
    static void set_use_skia_painter(UseSkiaPainter);

    virtual bool is_headless() const override;
    static void set_is_headless(bool);

    virtual bool is_ready_to_paint() const override;

    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }

    ErrorOr<void> connect_to_webdriver(ByteString const& webdriver_ipc_path);
    ErrorOr<void> connect_to_web_ui(IPC::File);

    virtual void paint_next_frame() override;
    virtual void process_screenshot_requests() override;
    virtual void paint(Web::DevicePixelRect const& content_rect, Web::Painting::BackingStore&, Web::PaintOptions = {}) override;

    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override;
    virtual void report_finished_handling_input_event(u64 page_id, Web::EventResult event_was_handled) override;

    void set_palette_impl(Gfx::PaletteImpl&);
    void set_viewport_size(Web::DevicePixelSize const&);
    void set_screen_rects(Vector<Web::DevicePixelRect, 4> const& rects, size_t main_screen_index) { m_screen_rect = rects[main_screen_index]; }
    void set_device_pixels_per_css_pixel(float device_pixels_per_css_pixel) { m_device_pixels_per_css_pixel = device_pixels_per_css_pixel; }
    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme);
    void set_preferred_contrast(Web::CSS::PreferredContrast);
    void set_preferred_motion(Web::CSS::PreferredMotion);
    void set_should_show_line_box_borders(bool b) { m_should_show_line_box_borders = b; }
    void set_has_focus(bool);
    void set_is_scripting_enabled(bool);
    void set_window_position(Web::DevicePixelPoint);
    void set_window_size(Web::DevicePixelSize);

    Web::DevicePixelSize content_size() const { return m_content_size; }

    Web::WebIDL::ExceptionOr<void> toggle_media_play_state();
    void toggle_media_mute_state();
    Web::WebIDL::ExceptionOr<void> toggle_media_loop_state();
    Web::WebIDL::ExceptionOr<void> toggle_media_controls_state();

    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(Optional<String> response);
    void color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    void set_user_style(String source);

    void ready_to_paint();

    void initialize_js_console(Web::DOM::Document& document);
    void js_console_input(StringView js_source);
    void did_execute_js_console_input(JsonValue const&);
    void run_javascript(StringView js_source);
    void js_console_request_messages(i32 start_index);
    void did_output_js_console_message(i32 message_index);
    void console_peer_did_misbehave(char const* reason);
    void did_get_js_console_messages(i32 start_index, ReadonlySpan<WebView::ConsoleOutput> console_output);

    Vector<Web::CSS::StyleSheetIdentifier> list_style_sheets() const;

    virtual double device_pixels_per_css_pixel() const override { return m_device_pixels_per_css_pixel; }

    virtual Web::DisplayListPlayerType display_list_player_type() const override;

    void queue_screenshot_task(Optional<Web::UniqueNodeID> node_id);

    friend class BackingStoreManager;

private:
    PageClient(PageHost&, u64 id);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // ^PageClient
    virtual bool is_connection_open() const override;
    virtual bool is_url_suitable_for_same_process_navigation(URL::URL const& current_url, URL::URL const& target_url) const override;
    virtual void request_new_process_for_navigation(URL::URL const&) override;
    virtual Gfx::Palette palette() const override;
    virtual Web::DevicePixelRect screen_rect() const override { return m_screen_rect; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return m_preferred_color_scheme; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return m_preferred_contrast; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return m_preferred_motion; }
    virtual void page_did_request_cursor_change(Gfx::Cursor const&) override;
    virtual void page_did_layout() override;
    virtual void page_did_change_title(ByteString const&) override;
    virtual void page_did_change_url(URL::URL const&) override;
    virtual void page_did_request_refresh() override;
    virtual void page_did_request_resize_window(Gfx::IntSize) override;
    virtual void page_did_request_reposition_window(Gfx::IntPoint) override;
    virtual void page_did_request_restore_window() override;
    virtual void page_did_request_maximize_window() override;
    virtual void page_did_request_minimize_window() override;
    virtual void page_did_request_fullscreen_window() override;
    virtual void page_did_request_tooltip_override(Web::CSSPixelPoint, ByteString const&) override;
    virtual void page_did_stop_tooltip_override() override;
    virtual void page_did_enter_tooltip_area(ByteString const&) override;
    virtual void page_did_leave_tooltip_area() override;
    virtual void page_did_hover_link(URL::URL const&) override;
    virtual void page_did_unhover_link() override;
    virtual void page_did_click_link(URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_middle_click_link(URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_request_context_menu(Web::CSSPixelPoint) override;
    virtual void page_did_request_link_context_menu(Web::CSSPixelPoint, URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_request_image_context_menu(Web::CSSPixelPoint, URL::URL const&, ByteString const& target, unsigned modifiers, Optional<Gfx::Bitmap const*>) override;
    virtual void page_did_request_media_context_menu(Web::CSSPixelPoint, ByteString const& target, unsigned modifiers, Web::Page::MediaContextMenu const&) override;
    virtual void page_did_start_loading(URL::URL const&, bool) override;
    virtual void page_did_create_new_document(Web::DOM::Document&) override;
    virtual void page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document&) override;
    virtual void page_did_finish_loading(URL::URL const&) override;
    virtual void page_did_request_alert(String const&) override;
    virtual void page_did_request_confirm(String const&) override;
    virtual void page_did_request_prompt(String const&, String const&) override;
    virtual void page_did_request_set_prompt_text(String const&) override;
    virtual void page_did_request_accept_dialog() override;
    virtual void page_did_request_dismiss_dialog() override;
    virtual void page_did_change_favicon(Gfx::Bitmap const&) override;
    virtual Vector<Web::Cookie::Cookie> page_did_request_all_cookies(URL::URL const&) override;
    virtual Optional<Web::Cookie::Cookie> page_did_request_named_cookie(URL::URL const&, String const&) override;
    virtual String page_did_request_cookie(URL::URL const&, Web::Cookie::Source) override;
    virtual void page_did_set_cookie(URL::URL const&, Web::Cookie::ParsedCookie const&, Web::Cookie::Source) override;
    virtual void page_did_update_cookie(Web::Cookie::Cookie const&) override;
    virtual void page_did_expire_cookies_with_time_offset(AK::Duration) override;
    virtual void page_did_update_resource_count(i32) override;
    virtual NewWebViewResult page_did_request_new_web_view(Web::HTML::ActivateTab, Web::HTML::WebViewHints, Web::HTML::TokenizedFeature::NoOpener) override;
    virtual void page_did_request_activate_tab() override;
    virtual void page_did_close_top_level_traversable() override;
    virtual void page_did_update_navigation_buttons_state(bool back_enabled, bool forward_enabled) override;
    virtual void request_file(Web::FileRequest) override;
    virtual void page_did_request_color_picker(Color current_color) override;
    virtual void page_did_request_file_picker(Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles) override;
    virtual void page_did_request_select_dropdown(Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void page_did_finish_test(String const& text) override;
    virtual void page_did_set_test_timeout(double milliseconds) override;
    virtual void page_did_set_browser_zoom(double factor) override;
    virtual void page_did_change_theme_color(Gfx::Color color) override;
    virtual void page_did_insert_clipboard_entry(StringView data, StringView presentation_style, StringView mime_type) override;
    virtual void page_did_change_audio_play_state(Web::HTML::AudioPlayState) override;
    virtual void page_did_allocate_backing_stores(i32 front_bitmap_id, Gfx::ShareableBitmap front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap back_bitmap) override;
    virtual IPC::File request_worker_agent() override;
    virtual void page_did_mutate_dom(FlyString const& type, Web::DOM::Node const& target, Web::DOM::NodeList& added_nodes, Web::DOM::NodeList& removed_nodes, GC::Ptr<Web::DOM::Node> previous_sibling, GC::Ptr<Web::DOM::Node> next_sibling, Optional<String> const& attribute_name) override;
    virtual void received_message_from_web_ui(String const& name, JS::Value data) override;

    Web::Layout::Viewport* layout_root();
    void setup_palette();
    ConnectionFromClient& client() const;

    PageHost& m_owner;
    GC::Ref<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Web::DevicePixelRect m_screen_rect;
    Web::DevicePixelSize m_content_size;
    float m_device_pixels_per_css_pixel { 1.0f };
    u64 m_id { 0 };
    bool m_should_show_line_box_borders { false };
    bool m_has_focus { false };

    enum class PaintState {
        Ready,
        WaitingForClient,
    };

    PaintState m_paint_state { PaintState::Ready };

    struct ScreenshotTask {
        Optional<Web::UniqueNodeID> node_id;
    };
    Queue<ScreenshotTask> m_screenshot_tasks;

    Web::CSS::PreferredColorScheme m_preferred_color_scheme { Web::CSS::PreferredColorScheme::Auto };
    Web::CSS::PreferredContrast m_preferred_contrast { Web::CSS::PreferredContrast::NoPreference };
    Web::CSS::PreferredMotion m_preferred_motion { Web::CSS::PreferredMotion::NoPreference };

    RefPtr<WebDriverConnection> m_webdriver;
    RefPtr<WebUIConnection> m_web_ui;

    BackingStoreManager m_backing_store_manager;

    WeakPtr<WebContentConsoleClient> m_top_level_document_console_client;

    GC::Root<JS::GlobalObject> m_console_global_object;

    RefPtr<Core::Timer> m_paint_refresh_timer;

    bool m_pending_set_browser_zoom_request = false;
};

}
