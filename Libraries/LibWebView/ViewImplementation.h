/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <AK/String.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PageInfo.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

class ViewImplementation {
public:
    virtual ~ViewImplementation();

    static void for_each_view(Function<IterationDecision(ViewImplementation&)>);
    static Optional<ViewImplementation&> find_view_by_id(u64);

    u64 view_id() const { return m_view_id; }

    void set_url(Badge<WebContentClient>, URL::URL url) { m_url = move(url); }
    URL::URL const& url() const { return m_url; }

    void set_title(Badge<WebContentClient>, ByteString title) { m_title = move(title); }
    ByteString const& title() const { return m_title; }

    String const& handle() const { return m_client_state.client_handle; }

    void create_new_process_for_cross_site_navigation(URL::URL const&);

    void server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size);

    void set_window_position(Gfx::IntPoint);
    void set_window_size(Gfx::IntSize);
    void did_update_window_rect();

    void set_system_visibility_state(Web::HTML::VisibilityState);

    void load(URL::URL const&);
    void load_html(StringView);
    void load_empty_document();
    void reload();
    void traverse_the_history_by_delta(int delta);

    void zoom_in();
    void zoom_out();
    void set_zoom(double zoom_level);
    void reset_zoom();
    float zoom_level() const { return m_zoom_level; }
    float device_pixel_ratio() const { return m_device_pixel_ratio; }

    void enqueue_input_event(Web::InputEvent);
    void did_finish_handling_input_event(Badge<WebContentClient>, Web::EventResult event_result);

    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme);
    void set_preferred_contrast(Web::CSS::PreferredContrast);
    void set_preferred_motion(Web::CSS::PreferredMotion);

    void set_preferred_languages(ReadonlySpan<String>);

    void set_enable_do_not_track(bool);

    void set_enable_autoplay(bool);

    ByteString selected_text();
    Optional<String> selected_text_with_whitespace_collapsed();
    void select_all();
    void find_in_page(String const& query, CaseSensitivity = CaseSensitivity::CaseInsensitive);
    void find_in_page_next_match();
    void find_in_page_previous_match();
    void paste(String const&);

    void get_source();

    void inspect_dom_tree();
    void inspect_accessibility_tree();
    void get_hovered_node_id();

    void inspect_dom_node(Web::UniqueNodeID node_id, DOMNodeProperties::Type, Optional<Web::CSS::PseudoElement> pseudo_element);
    void clear_inspected_dom_node();

    void highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element);
    void clear_highlighted_dom_node();

    void set_listen_for_dom_mutations(bool);
    void get_dom_node_inner_html(Web::UniqueNodeID node_id);
    void get_dom_node_outer_html(Web::UniqueNodeID node_id);
    void set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html);
    void set_dom_node_text(Web::UniqueNodeID node_id, String const& text);
    void set_dom_node_tag(Web::UniqueNodeID node_id, String const& name);
    void add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes);
    void replace_dom_node_attribute(Web::UniqueNodeID node_id, String const& name, ReadonlySpan<Attribute> replacement_attributes);
    void create_child_element(Web::UniqueNodeID node_id);
    void create_child_text_node(Web::UniqueNodeID node_id);
    void insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id);
    void clone_dom_node(Web::UniqueNodeID node_id);
    void remove_dom_node(Web::UniqueNodeID node_id);

    void list_style_sheets();
    void request_style_sheet_source(Web::CSS::StyleSheetIdentifier const&);

    void debug_request(ByteString const& request, ByteString const& argument = {});

    void run_javascript(String const&);
    void js_console_input(String const&);
    void js_console_request_messages(i32 start_index);

    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(Optional<String> const& response);
    void color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state);
    void file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    void toggle_media_play_state();
    void toggle_media_mute_state();
    void toggle_media_loop_state();
    void toggle_media_controls_state();

    Web::HTML::MuteState page_mute_state() const { return m_mute_state; }
    void toggle_page_mute_state();

    void did_change_audio_play_state(Badge<WebContentClient>, Web::HTML::AudioPlayState);
    Web::HTML::AudioPlayState audio_play_state() const { return m_audio_play_state; }

    void did_update_navigation_buttons_state(Badge<WebContentClient>, bool back_enabled, bool forward_enabled) const;

    void did_allocate_backing_stores(Badge<WebContentClient>, i32 front_bitmap_id, Gfx::ShareableBitmap const&, i32 back_bitmap_id, Gfx::ShareableBitmap const&);
#ifdef AK_OS_MACOS
    void did_allocate_iosurface_backing_stores(i32 front_bitmap_id, Core::MachPort&&, i32 back_bitmap_id, Core::MachPort&&);
#endif

    enum class ScreenshotType {
        Visible,
        Full,
    };
    NonnullRefPtr<Core::Promise<LexicalPath>> take_screenshot(ScreenshotType);
    NonnullRefPtr<Core::Promise<LexicalPath>> take_dom_node_screenshot(Web::UniqueNodeID);
    virtual void did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const&);

    NonnullRefPtr<Core::Promise<String>> request_internal_page_info(PageInfoType);
    void did_receive_internal_page_info(Badge<WebContentClient>, PageInfoType, String const&);

    ErrorOr<LexicalPath> dump_gc_graph();

    void set_user_style_sheet(String const& source);
    // Load Native.css as the User style sheet, which attempts to make WebView content look as close to
    // native GUI widgets as possible.
    void use_native_user_style_sheet();

    Function<void()> on_ready_to_paint;
    Function<String(Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<u64>)> on_new_web_view;
    Function<void()> on_activate_tab;
    Function<void()> on_close;
    Function<void(Gfx::IntPoint screen_position)> on_context_menu_request;
    Function<void(URL::URL const&, Gfx::IntPoint screen_position)> on_link_context_menu_request;
    Function<void(URL::URL const&, Gfx::IntPoint screen_position, Optional<Gfx::ShareableBitmap> const&)> on_image_context_menu_request;
    Function<void(Gfx::IntPoint screen_position, Web::Page::MediaContextMenu const&)> on_media_context_menu_request;
    Function<void(URL::URL const&)> on_link_hover;
    Function<void()> on_link_unhover;
    Function<void(URL::URL const&, ByteString const& target, unsigned modifiers)> on_link_click;
    Function<void(URL::URL const&, ByteString const& target, unsigned modifiers)> on_link_middle_click;
    Function<void(ByteString const&)> on_title_change;
    Function<void(URL::URL const&)> on_url_change;
    Function<void(URL::URL const&, bool)> on_load_start;
    Function<void(URL::URL const&)> on_load_finish;
    Function<void(ByteString const& path, i32)> on_request_file;
    Function<void(Gfx::Bitmap const&)> on_favicon_change;
    Function<void(Gfx::Cursor const&)> on_cursor_change;
    Function<void(Gfx::IntPoint, ByteString const&)> on_request_tooltip_override;
    Function<void()> on_stop_tooltip_override;
    Function<void(ByteString const&)> on_enter_tooltip_area;
    Function<void()> on_leave_tooltip_area;
    Function<void(String const& message)> on_request_alert;
    Function<void(String const& message)> on_request_confirm;
    Function<void(String const& message, String const& default_)> on_request_prompt;
    Function<void(String const& message)> on_request_set_prompt_text;
    Function<void()> on_request_accept_dialog;
    Function<void()> on_request_dismiss_dialog;
    Function<void(URL::URL const&, URL::URL const&, String const&)> on_received_source;
    Function<void(JsonObject)> on_received_dom_tree;
    Function<void(DOMNodeProperties)> on_received_dom_node_properties;
    Function<void(JsonObject)> on_received_accessibility_tree;
    Function<void(Web::UniqueNodeID)> on_received_hovered_node_id;
    Function<void(Mutation)> on_dom_mutation_received;
    Function<void(Optional<Web::UniqueNodeID> const& node_id)> on_finshed_editing_dom_node;
    Function<void(String)> on_received_dom_node_html;
    Function<void(Vector<Web::CSS::StyleSheetIdentifier>)> on_received_style_sheet_list;
    Function<void(Web::CSS::StyleSheetIdentifier const&, URL::URL const&, String const&)> on_received_style_sheet_source;
    Function<void(JsonValue)> on_received_js_console_result;
    Function<void(i32 message_id)> on_console_message_available;
    Function<void(i32 start_index, Vector<ConsoleOutput>)> on_received_console_messages;
    Function<void(i32 count_waiting)> on_resource_status_change;
    Function<void()> on_restore_window;
    Function<void(Gfx::IntPoint)> on_reposition_window;
    Function<void(Gfx::IntSize)> on_resize_window;
    Function<void()> on_maximize_window;
    Function<void()> on_minimize_window;
    Function<void()> on_fullscreen_window;
    Function<void(Color current_color)> on_request_color_picker;
    Function<void(Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles)> on_request_file_picker;
    Function<void(Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)> on_request_select_dropdown;
    Function<void(Web::KeyEvent const&)> on_finish_handling_key_event;
    Function<void(Web::DragEvent const&)> on_finish_handling_drag_event;
    Function<void(String const&)> on_test_finish;
    Function<void(double milliseconds)> on_set_test_timeout;
    Function<void(double factor)> on_set_browser_zoom;
    Function<void(size_t current_match_index, Optional<size_t> const& total_match_count)> on_find_in_page;
    Function<void(Gfx::Color)> on_theme_color_change;
    Function<void(String const&, String const&, String const&)> on_insert_clipboard_entry;
    Function<void(Web::HTML::AudioPlayState)> on_audio_play_state_changed;
    Function<void(bool, bool)> on_navigation_buttons_state_changed;
    Function<void()> on_web_content_crashed;

    virtual Web::DevicePixelSize viewport_size() const = 0;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const = 0;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const = 0;

protected:
    static constexpr auto ZOOM_MIN_LEVEL = 0.3f;
    static constexpr auto ZOOM_MAX_LEVEL = 5.0f;
    static constexpr auto ZOOM_STEP = 0.1f;

    ViewImplementation();

    WebContentClient& client();
    WebContentClient const& client() const;
    u64 page_id() const;
    virtual void update_zoom() = 0;

    void handle_resize();

    enum class CreateNewClient {
        No,
        Yes,
    };
    virtual void initialize_client(CreateNewClient = CreateNewClient::Yes);

    enum class LoadErrorPage {
        No,
        Yes,
    };
    void handle_web_content_process_crash(LoadErrorPage = LoadErrorPage::Yes);

    struct SharedBitmap {
        i32 id { -1 };
        Web::DevicePixelSize last_painted_size;
        RefPtr<Gfx::Bitmap> bitmap;
    };

    struct ClientState {
        RefPtr<WebContentClient> client;
        String client_handle;
        SharedBitmap front_bitmap;
        SharedBitmap back_bitmap;
        u64 page_index { 0 };
        bool has_usable_bitmap { false };
    } m_client_state;

    URL::URL m_url;
    ByteString m_title;

    float m_zoom_level { 1.0 };
    float m_device_pixel_ratio { 1.0 };

    Queue<Web::InputEvent> m_pending_input_events;

    RefPtr<Core::Timer> m_backing_store_shrink_timer;

    RefPtr<Gfx::Bitmap> m_backup_bitmap;
    Web::DevicePixelSize m_backup_bitmap_size;

    size_t m_crash_count = 0;
    RefPtr<Core::Timer> m_repeated_crash_timer;

    RefPtr<Core::Promise<LexicalPath>> m_pending_screenshot;
    RefPtr<Core::Promise<String>> m_pending_info_request;

    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };

    Web::HTML::AudioPlayState m_audio_play_state { Web::HTML::AudioPlayState::Paused };
    size_t m_number_of_elements_playing_audio { 0 };

    Web::HTML::MuteState m_mute_state { Web::HTML::MuteState::Unmuted };

    // FIXME: Reconcile this ID with `page_id`. The latter is only unique per WebContent connection, whereas the view ID
    //        is required to be globally unique for Firefox DevTools.
    u64 m_view_id { 0 };
};

}
