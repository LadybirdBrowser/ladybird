/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibGC/Root.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace Web {

class PageClient;

class Page final : public JS::Cell {
    GC_CELL(Page, JS::Cell);
    GC_DECLARE_ALLOCATOR(Page);

public:
    static GC::Ref<Page> create(JS::VM&, GC::Ref<PageClient>);

    ~Page();

    PageClient& client() { return m_client; }
    PageClient const& client() const { return m_client; }

    void set_top_level_traversable(GC::Ref<HTML::TraversableNavigable>);

    // FIXME: This is a hack.
    bool top_level_traversable_is_initialized() const;

    HTML::BrowsingContext& top_level_browsing_context();
    HTML::BrowsingContext const& top_level_browsing_context() const;

    GC::Ref<HTML::TraversableNavigable> top_level_traversable() const;

    HTML::Navigable& focused_navigable();
    HTML::Navigable const& focused_navigable() const { return const_cast<Page*>(this)->focused_navigable(); }

    void set_focused_navigable(Badge<EventHandler>, HTML::Navigable&);
    void navigable_document_destroyed(Badge<DOM::Document>, HTML::Navigable&);

    void load(URL::URL const&);

    void load_html(StringView);

    void reload();

    void traverse_the_history_by_delta(int delta);

    CSSPixelPoint device_to_css_point(DevicePixelPoint) const;
    DevicePixelPoint css_to_device_point(CSSPixelPoint) const;
    DevicePixelRect css_to_device_rect(CSSPixelRect) const;
    CSSPixelRect device_to_css_rect(DevicePixelRect) const;
    CSSPixelSize device_to_css_size(DevicePixelSize) const;
    DevicePixelRect enclosing_device_rect(CSSPixelRect) const;
    DevicePixelRect rounded_device_rect(CSSPixelRect) const;

    EventResult handle_mouseup(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousedown(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousemove(DevicePixelPoint, DevicePixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult handle_mousewheel(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, DevicePixels wheel_delta_x, DevicePixels wheel_delta_y);
    EventResult handle_doubleclick(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    EventResult handle_drag_and_drop_event(DragEvent::Type, DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    Gfx::Palette palette() const;
    CSSPixelRect web_exposed_screen_area() const;
    CSS::PreferredColorScheme preferred_color_scheme() const;
    CSS::PreferredContrast preferred_contrast() const;
    CSS::PreferredMotion preferred_motion() const;

    bool is_same_origin_policy_enabled() const { return m_same_origin_policy_enabled; }
    void set_same_origin_policy_enabled(bool b) { m_same_origin_policy_enabled = b; }

    bool is_scripting_enabled() const { return m_is_scripting_enabled; }
    void set_is_scripting_enabled(bool b) { m_is_scripting_enabled = b; }

    bool should_block_pop_ups() const { return m_should_block_pop_ups; }
    void set_should_block_pop_ups(bool b) { m_should_block_pop_ups = b; }

    bool is_webdriver_active() const { return m_is_webdriver_active; }
    void set_is_webdriver_active(bool b) { m_is_webdriver_active = b; }

    bool is_hovering_link() const { return m_is_hovering_link; }
    void set_is_hovering_link(bool b) { m_is_hovering_link = b; }

    bool is_in_tooltip_area() const { return m_is_in_tooltip_area; }
    void set_is_in_tooltip_area(bool b) { m_is_in_tooltip_area = b; }

    Gfx::Cursor current_cursor() const { return m_current_cursor; }
    void set_current_cursor(Gfx::Cursor cursor) { m_current_cursor = move(cursor); }

    DevicePixelPoint window_position() const { return m_window_position; }
    void set_window_position(DevicePixelPoint position) { m_window_position = position; }

    DevicePixelSize window_size() const { return m_window_size; }
    void set_window_size(DevicePixelSize size) { m_window_size = size; }

    void did_update_window_rect();
    void set_window_rect_observer(GC::Ptr<GC::Function<void(DevicePixelRect)>> window_rect_observer) { m_window_rect_observer = window_rect_observer; }

    void did_request_alert(String const& message);
    void alert_closed();

    bool did_request_confirm(String const& message);
    void confirm_closed(bool accepted);

    Optional<String> did_request_prompt(String const& message, String const& default_);
    void prompt_closed(Optional<String> response);

    enum class PendingDialog {
        None,
        Alert,
        Confirm,
        Prompt,
    };
    bool has_pending_dialog() const { return m_pending_dialog != PendingDialog::None; }
    PendingDialog pending_dialog() const { return m_pending_dialog; }
    Optional<String> const& pending_dialog_text() const { return m_pending_dialog_text; }
    void dismiss_dialog(GC::Ref<GC::Function<void()>> on_dialog_closed);
    void accept_dialog(GC::Ref<GC::Function<void()>> on_dialog_closed);

    void did_request_color_picker(WeakPtr<HTML::HTMLInputElement> target, Color current_color);
    void color_picker_update(Optional<Color> picked_color, HTML::ColorPickerUpdateState state);

    void did_request_file_picker(WeakPtr<HTML::HTMLInputElement> target, HTML::FileFilter const& accepted_file_types, HTML::AllowMultipleFiles);
    void file_picker_closed(Span<HTML::SelectedFile> selected_files);

    void did_request_select_dropdown(WeakPtr<HTML::HTMLSelectElement> target, Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    enum class PendingNonBlockingDialog {
        None,
        ColorPicker,
        FilePicker,
        Select,
    };

    void register_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id);
    void unregister_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id);

    struct MediaContextMenu {
        URL::URL media_url;
        bool is_video { false };
        bool is_playing { false };
        bool is_muted { false };
        bool has_user_agent_controls { false };
        bool is_looping { false };
    };
    void did_request_media_context_menu(UniqueNodeID media_id, CSSPixelPoint, ByteString const& target, unsigned modifiers, MediaContextMenu const&);
    WebIDL::ExceptionOr<void> toggle_media_play_state();
    void toggle_media_mute_state();
    WebIDL::ExceptionOr<void> toggle_media_loop_state();
    WebIDL::ExceptionOr<void> toggle_media_controls_state();

    HTML::MuteState page_mute_state() const { return m_mute_state; }
    void toggle_page_mute_state();

    Optional<String> const& user_style() const { return m_user_style_sheet_source; }
    void set_user_style(String source);

    bool pdf_viewer_supported() const { return m_pdf_viewer_supported; }

    void clear_selection();

    enum class WrapAround {
        Yes,
        No,
    };
    struct FindInPageQuery {
        String string {};
        CaseSensitivity case_sensitivity { CaseSensitivity::CaseInsensitive };
        WrapAround wrap_around { WrapAround::Yes };
    };
    struct FindInPageResult {
        size_t current_match_index { 0 };
        Optional<size_t> total_match_count {};
    };
    FindInPageResult find_in_page(FindInPageQuery const&);
    FindInPageResult find_in_page_next_match();
    FindInPageResult find_in_page_previous_match();
    Optional<FindInPageQuery> last_find_in_page_query() const { return m_last_find_in_page_query; }

    bool listen_for_dom_mutations() const { return m_listen_for_dom_mutations; }
    void set_listen_for_dom_mutations(bool listen_for_dom_mutations) { m_listen_for_dom_mutations = listen_for_dom_mutations; }

private:
    explicit Page(GC::Ref<PageClient>);
    virtual void visit_edges(Visitor&) override;

    GC::Ptr<HTML::HTMLMediaElement> media_context_menu_element();

    Vector<GC::Root<DOM::Document>> documents_in_active_window() const;

    enum class SearchDirection {
        Forward,
        Backward,
    };
    FindInPageResult perform_find_in_page_query(FindInPageQuery const&, Optional<SearchDirection> = {});
    void update_find_in_page_selection(Vector<GC::Root<DOM::Range>> matches);

    void on_pending_dialog_closed();

    GC::Ref<PageClient> m_client;

    WeakPtr<HTML::Navigable> m_focused_navigable;

    GC::Ptr<HTML::TraversableNavigable> m_top_level_traversable;

    // FIXME: Enable this by default once CORS preflight checks are supported.
    bool m_same_origin_policy_enabled { false };

    bool m_is_scripting_enabled { true };

    bool m_should_block_pop_ups { true };

    // https://w3c.github.io/webdriver/#dfn-webdriver-active-flag
    // The webdriver-active flag is set to true when the user agent is under remote control. It is initially false.
    bool m_is_webdriver_active { false };

    bool m_is_hovering_link { false };
    bool m_is_in_tooltip_area { false };

    Gfx::Cursor m_current_cursor { Gfx::StandardCursor::Arrow };

    DevicePixelPoint m_window_position {};
    DevicePixelSize m_window_size {};
    GC::Ptr<GC::Function<void(DevicePixelRect)>> m_window_rect_observer;

    PendingDialog m_pending_dialog { PendingDialog::None };
    Optional<String> m_pending_dialog_text;
    Optional<Empty> m_pending_alert_response;
    Optional<bool> m_pending_confirm_response;
    Optional<Optional<String>> m_pending_prompt_response;
    GC::Ptr<GC::Function<void()>> m_on_pending_dialog_closed;

    PendingNonBlockingDialog m_pending_non_blocking_dialog { PendingNonBlockingDialog::None };
    WeakPtr<HTML::HTMLElement> m_pending_non_blocking_dialog_target;

    Vector<UniqueNodeID> m_media_elements;
    Optional<UniqueNodeID> m_media_context_menu_element_id;

    Web::HTML::MuteState m_mute_state { Web::HTML::MuteState::Unmuted };

    Optional<String> m_user_style_sheet_source;

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-supported
    // Each user agent has a PDF viewer supported boolean, whose value is implementation-defined (and might vary according to user preferences).
    // Spec Note: This value also impacts the navigation processing model.
    // FIXME: Actually support pdf viewing
    bool m_pdf_viewer_supported { false };

    size_t m_find_in_page_match_index { 0 };
    Optional<FindInPageQuery> m_last_find_in_page_query;
    URL::URL m_last_find_in_page_url;

    bool m_listen_for_dom_mutations { false };
};

struct PaintOptions {
    enum class PaintOverlay {
        No,
        Yes,
    };

    PaintOverlay paint_overlay { PaintOverlay::Yes };
    bool should_show_line_box_borders { false };
    bool has_focus { false };
};

enum class DisplayListPlayerType {
    SkiaGPUIfAvailable,
    SkiaCPU,
};

class PageClient : public JS::Cell {
    GC_CELL(PageClient, JS::Cell);

public:
    virtual Page& page() = 0;
    virtual Page const& page() const = 0;
    virtual bool is_connection_open() const = 0;
    virtual bool is_url_suitable_for_same_process_navigation([[maybe_unused]] URL::URL const& current_url, [[maybe_unused]] URL::URL const& target_url) const { return true; }
    virtual void request_new_process_for_navigation(URL::URL const&) { }
    virtual Gfx::Palette palette() const = 0;
    virtual DevicePixelRect screen_rect() const = 0;
    virtual double device_pixels_per_css_pixel() const = 0;
    virtual CSS::PreferredColorScheme preferred_color_scheme() const = 0;
    virtual CSS::PreferredContrast preferred_contrast() const = 0;
    virtual CSS::PreferredMotion preferred_motion() const = 0;
    virtual void paint_next_frame() = 0;
    virtual void process_screenshot_requests() = 0;
    virtual void paint(DevicePixelRect const&, Painting::BackingStore&, PaintOptions = {}) = 0;
    virtual Queue<QueuedInputEvent>& input_event_queue() = 0;
    virtual void report_finished_handling_input_event(u64 page_id, EventResult event_was_handled) = 0;
    virtual void page_did_change_title(ByteString const&) { }
    virtual void page_did_change_url(URL::URL const&) { }
    virtual void page_did_request_refresh() { }
    virtual void page_did_request_resize_window(Gfx::IntSize) { }
    virtual void page_did_request_reposition_window(Gfx::IntPoint) { }
    virtual void page_did_request_restore_window() { }
    virtual void page_did_request_maximize_window() { }
    virtual void page_did_request_minimize_window() { }
    virtual void page_did_request_fullscreen_window() { }
    virtual void page_did_start_loading(URL::URL const&, bool is_redirect) { (void)is_redirect; }
    virtual void page_did_create_new_document(Web::DOM::Document&) { }
    virtual void page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document&) { }
    virtual void page_did_finish_loading(URL::URL const&) { }
    virtual void page_did_request_cursor_change(Gfx::Cursor const&) { }
    virtual void page_did_request_context_menu(CSSPixelPoint) { }
    virtual void page_did_request_link_context_menu(CSSPixelPoint, URL::URL const&, [[maybe_unused]] ByteString const& target, [[maybe_unused]] unsigned modifiers) { }
    virtual void page_did_request_image_context_menu(CSSPixelPoint, URL::URL const&, [[maybe_unused]] ByteString const& target, [[maybe_unused]] unsigned modifiers, Optional<Gfx::Bitmap const*>) { }
    virtual void page_did_request_media_context_menu(CSSPixelPoint, [[maybe_unused]] ByteString const& target, [[maybe_unused]] unsigned modifiers, Page::MediaContextMenu const&) { }
    virtual void page_did_click_link(URL::URL const&, [[maybe_unused]] ByteString const& target, [[maybe_unused]] unsigned modifiers) { }
    virtual void page_did_middle_click_link(URL::URL const&, [[maybe_unused]] ByteString const& target, [[maybe_unused]] unsigned modifiers) { }
    virtual void page_did_request_tooltip_override(CSSPixelPoint, ByteString const&) { }
    virtual void page_did_stop_tooltip_override() { }
    virtual void page_did_enter_tooltip_area(ByteString const&) { }
    virtual void page_did_leave_tooltip_area() { }
    virtual void page_did_hover_link(URL::URL const&) { }
    virtual void page_did_unhover_link() { }
    virtual void page_did_change_favicon(Gfx::Bitmap const&) { }
    virtual void page_did_layout() { }
    virtual void page_did_request_alert(String const&) { }
    virtual void page_did_request_confirm(String const&) { }
    virtual void page_did_request_prompt(String const&, String const&) { }
    virtual void page_did_request_set_prompt_text(String const&) { }
    virtual void page_did_request_accept_dialog() { }
    virtual void page_did_request_dismiss_dialog() { }
    virtual Vector<Web::Cookie::Cookie> page_did_request_all_cookies(URL::URL const&) { return {}; }
    virtual Optional<Web::Cookie::Cookie> page_did_request_named_cookie(URL::URL const&, String const&) { return {}; }
    virtual String page_did_request_cookie(URL::URL const&, Cookie::Source) { return {}; }
    virtual void page_did_set_cookie(URL::URL const&, Cookie::ParsedCookie const&, Cookie::Source) { }
    virtual void page_did_update_cookie(Web::Cookie::Cookie const&) { }
    virtual void page_did_expire_cookies_with_time_offset(AK::Duration) { }
    virtual void page_did_update_resource_count(i32) { }
    struct NewWebViewResult {
        GC::Ptr<Page> page;
        String window_handle;
    };
    virtual NewWebViewResult page_did_request_new_web_view(HTML::ActivateTab, HTML::WebViewHints, HTML::TokenizedFeature::NoOpener) { return {}; }
    virtual void page_did_request_activate_tab() { }
    virtual void page_did_close_top_level_traversable() { }
    virtual void page_did_update_navigation_buttons_state([[maybe_unused]] bool back_enabled, [[maybe_unused]] bool forward_enabled) { }
    virtual void page_did_allocate_backing_stores([[maybe_unused]] i32 front_bitmap_id, [[maybe_unused]] Gfx::ShareableBitmap front_bitmap, [[maybe_unused]] i32 back_bitmap_id, [[maybe_unused]] Gfx::ShareableBitmap back_bitmap) { }

    virtual void request_file(FileRequest) = 0;

    // https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
    virtual void page_did_request_color_picker([[maybe_unused]] Color current_color) { }
    virtual void page_did_request_file_picker([[maybe_unused]] HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles) { }
    virtual void page_did_request_select_dropdown([[maybe_unused]] Web::CSSPixelPoint content_position, [[maybe_unused]] Web::CSSPixels minimum_width, [[maybe_unused]] Vector<Web::HTML::SelectItem> items) { }

    virtual void page_did_finish_test([[maybe_unused]] String const& text) { }
    virtual void page_did_set_test_timeout([[maybe_unused]] double milliseconds) { }

    virtual void page_did_set_browser_zoom([[maybe_unused]] double factor) { }

    virtual void page_did_change_theme_color(Gfx::Color) { }

    virtual void page_did_insert_clipboard_entry([[maybe_unused]] StringView data, [[maybe_unused]] StringView presentation_style, [[maybe_unused]] StringView mime_type) { }

    virtual void page_did_change_audio_play_state(HTML::AudioPlayState) { }

    virtual IPC::File request_worker_agent() { return IPC::File {}; }

    virtual void page_did_mutate_dom([[maybe_unused]] FlyString const& type, [[maybe_unused]] DOM::Node const& target, [[maybe_unused]] DOM::NodeList& added_nodes, [[maybe_unused]] DOM::NodeList& removed_nodes, [[maybe_unused]] GC::Ptr<DOM::Node> previous_sibling, [[maybe_unused]] GC::Ptr<DOM::Node> next_sibling, [[maybe_unused]] Optional<String> const& attribute_name) { }

    virtual void received_message_from_web_ui([[maybe_unused]] String const& name, [[maybe_unused]] JS::Value data) { }

    virtual bool is_ready_to_paint() const = 0;

    virtual DisplayListPlayerType display_list_player_type() const = 0;

    virtual bool is_headless() const = 0;

protected:
    virtual ~PageClient() = default;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::Page::MediaContextMenu const&);

template<>
ErrorOr<Web::Page::MediaContextMenu> decode(Decoder&);

}
