/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/JsonValue.h>
#include <AK/Queue.h>
#include <AK/Variant.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibHTTP/Header.h>
#include <LibIPC/Forward.h>
#include <LibIPC/TransportHandle.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/AgentType.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/DOM/RequestFullscreenError.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/HTML/WorkerAgentForward.h>
#include <LibWeb/IndexedDB/TransactionChanges.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWebView/StorageSetResult.h>

namespace Web {

class PageClient;
namespace Compositor {

class CompositorHost;

}

class WEB_API Page final : public JS::Cell {
    GC_CELL(Page, JS::Cell);
    GC_DECLARE_ALLOCATOR(Page);

public:
    static GC::Ref<Page> create(JS::VM&, GC::Ref<PageClient>);

    ~Page();

    PageClient& client() { return m_client; }
    PageClient const& client() const { return m_client; }
    bool has_compositor_host() const;
    void ensure_compositor_host();
    Compositor::CompositorHost& compositor_host();
    Compositor::CompositorHost const& compositor_host() const;

    void set_top_level_traversable(GC::Ref<HTML::LocalTraversableNavigable>);

    // FIXME: This is a hack.
    bool top_level_traversable_is_initialized() const;

    HTML::BrowsingContext& top_level_browsing_context();
    HTML::BrowsingContext const& top_level_browsing_context() const;

    GC::Ref<HTML::LocalTraversableNavigable> top_level_traversable() const;

    HTML::LocalNavigable& focused_navigable();
    HTML::LocalNavigable const& focused_navigable() const { return const_cast<Page*>(this)->focused_navigable(); }

    void set_focused_navigable(Badge<EventHandler>, HTML::LocalNavigable&);
    void navigable_document_destroyed(Badge<DOM::Document>, HTML::LocalNavigable&);

    void load(URL::URL const&, Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);
    void load(URL::URL const&, Variant<Empty, String, HTML::POSTResource>,
        Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);

    void load_html(StringView);
    void load_html(StringView, URL::URL const&);

    void reload();

    void traverse_the_history_by_delta(int delta);

    CSSPixelPoint device_to_css_point(DevicePixelPoint) const;
    DevicePixelPoint css_to_device_point(CSSPixelPoint) const;
    DevicePixelRect css_to_device_rect(CSSPixelRect) const;
    CSSPixelRect device_to_css_rect(DevicePixelRect) const;
    CSSPixelSize device_to_css_size(DevicePixelSize) const;
    DevicePixelRect enclosing_device_rect(CSSPixelRect) const;
    DevicePixelRect rounded_device_rect(CSSPixelRect) const;
    ChromeMetrics chrome_metrics() const;

    EventResult handle_mouseup(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousedown(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count);
    EventResult handle_mousemove(DevicePixelPoint, DevicePixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult handle_mouseleave();
    UniqueNodeID node_id_at_position(DevicePixelPoint);
    EventResult handle_mousewheel(DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y, bool async_scroll_performed_default_action = false, Optional<AsyncScrollOperation>* async_scroll_operation = nullptr);

    EventResult handle_drag_and_drop_event(DragEvent::Type, DevicePixelPoint, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);
    EventResult handle_pinch_event(DevicePixelPoint point, unsigned modifiers, double scale);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat, bool should_insert_text);
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    void handle_sdl_input_events();

    Gfx::Palette palette() const;
    CSSPixelRect web_exposed_screen_area() const;
    CSSPixelRect web_exposed_available_screen_area() const;
    CSS::PreferredColorScheme preferred_color_scheme() const;
    void set_preferred_color_scheme_override_for_testing(Optional<CSS::PreferredColorScheme> color_scheme) { m_preferred_color_scheme_override_for_testing = color_scheme; }
    CSS::PreferredContrast preferred_contrast() const;
    CSS::PreferredMotion preferred_motion() const;

    bool is_scripting_enabled() const { return m_is_scripting_enabled; }
    void set_is_scripting_enabled(bool b) { m_is_scripting_enabled = b; }

    bool should_block_pop_ups() const { return m_should_block_pop_ups; }
    void set_should_block_pop_ups(bool b) { m_should_block_pop_ups = b; }

    bool enable_autoscroll() const { return m_enable_autoscroll; }
    void set_enable_autoscroll(bool b) { m_enable_autoscroll = b; }

    bool enable_primary_paste() const { return m_enable_primary_paste; }
    void set_enable_primary_paste(bool b) { m_enable_primary_paste = b; }

    bool async_scrolling_enabled() const { return m_async_scrolling_enabled; }
    void set_async_scrolling_enabled(bool b) { m_async_scrolling_enabled = b; }
    u64 wheel_event_listener_state_generation() const { return m_wheel_event_listener_state_generation; }
    void invalidate_compositor_wheel_event_listener_state();
    bool needs_beforeunload_check() const { return m_needs_beforeunload_check; }
    void update_needs_beforeunload_check();

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

    void did_request_color_picker(GC::Weak<HTML::HTMLInputElement> target, Color current_color);
    void color_picker_update(Optional<Color> picked_color, HTML::ColorPickerUpdateState state);

    void did_request_file_picker(GC::Weak<HTML::HTMLInputElement> target, HTML::FileFilter const& accepted_file_types, HTML::AllowMultipleFiles);
    void file_picker_closed(Span<HTML::SelectedFile> selected_files);

    void did_request_select_dropdown(GC::Weak<HTML::HTMLSelectElement> target, Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    using ClipboardRequest = GC::Ref<GC::Function<void(Vector<Clipboard::SystemClipboardItem>)>>;
    void request_clipboard_entries(ClipboardRequest);
    void retrieved_clipboard_entries(u64 request_id, Vector<Clipboard::SystemClipboardItem>);

    enum class PendingNonBlockingDialog {
        None,
        ColorPicker,
        FilePicker,
        Select,
    };

    void register_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id);
    void unregister_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id);

    void update_all_media_element_video_sinks();

    void register_canvas_element(Badge<HTML::HTMLCanvasElement>, UniqueNodeID canvas_id);
    void unregister_canvas_element(Badge<HTML::HTMLCanvasElement>, UniqueNodeID canvas_id);

    void prepare_canvas_contexts_for_compositing();
    void notify_all_canvas_elements_of_lost_backing_storage();
    void notify_all_webgl_contexts_lost();

    struct MediaContextMenu {
        URL::URL media_url;
        bool is_video { false };
        bool is_playing { false };
        bool is_muted { false };
        bool has_user_agent_controls { false };
        bool is_looping { false };
        bool is_fullscreen { false };
    };
    void did_request_media_context_menu(UniqueNodeID media_id, CSSPixelPoint, ByteString const& target, unsigned modifiers, MediaContextMenu const&);
    void toggle_media_play_state();
    void toggle_media_mute_state();
    void toggle_media_loop_state();
    void toggle_media_fullscreen_state();
    void toggle_media_controls_state();

    HTML::MuteState page_mute_state() const { return m_mute_state; }
    void toggle_page_mute_state();

    Optional<String> const& user_style() const { return m_user_style_sheet_source; }
    void set_user_style(String source);
    void set_content_blocking_enabled(bool);
    void invalidate_user_style();

    bool pdf_viewer_supported() const { return m_pdf_viewer_supported; }

    void clear_selection();

    enum class WrapAround {
        Yes,
        No,
    };
    enum class ClearSelectionOnNoMatch {
        Yes,
        No,
    };
    struct FindInPageQuery {
        String string {};
        CaseSensitivity case_sensitivity { CaseSensitivity::CaseInsensitive };
        WrapAround wrap_around { WrapAround::Yes };
        ClearSelectionOnNoMatch clear_selection_on_no_match { ClearSelectionOnNoMatch::Yes };
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

    void enqueue_fullscreen_enter(GC::Ref<DOM::Element>, GC::Ref<DOM::Document>, DOM::RequestFullscreenError, GC::Ref<WebIDL::Promise>);
    void enqueue_fullscreen_exit(GC::Ref<DOM::Document> doc, bool resize, GC::Ref<WebIDL::Promise>);
    void process_pending_fullscreen_operations();

    ViewportIsFullscreen viewport_is_fullscreen() const { return m_viewport_is_fullscreen; }
    void set_viewport_is_fullscreen(ViewportIsFullscreen);

private:
    explicit Page(GC::Ref<PageClient>);
    virtual void visit_edges(Visitor&) override;

    GC::Ptr<HTML::HTMLMediaElement> media_context_menu_element();

    template<typename Callback>
    void for_each_media_element(Callback&& callback);

    template<typename Callback>
    void for_each_canvas_element(Callback&& callback);

    Vector<GC::Root<DOM::Document>> documents_in_active_window() const;

    enum class SearchDirection {
        Forward,
        Backward,
    };
    FindInPageResult perform_find_in_page_query(FindInPageQuery const&, Optional<SearchDirection> = {});
    void update_find_in_page_selection(Vector<GC::Root<DOM::Range>> matches, ClearSelectionOnNoMatch);

    void on_pending_dialog_closed();

    GC::Ref<PageClient> m_client;

    GC::Weak<HTML::LocalNavigable> m_focused_navigable;

    GC::Ptr<HTML::LocalTraversableNavigable> m_top_level_traversable;

    bool m_is_scripting_enabled { true };
    bool m_should_block_pop_ups { true };
    bool m_enable_autoscroll { true };
    bool m_enable_primary_paste { true };
    bool m_async_scrolling_enabled { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    bool m_needs_beforeunload_check { true };

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
    GC::Weak<HTML::HTMLElement> m_pending_non_blocking_dialog_target;

    HashMap<u64, ClipboardRequest> m_pending_clipboard_requests;
    u64 m_next_clipboard_request_id { 0 };

    Vector<UniqueNodeID> m_media_elements;
    Vector<UniqueNodeID> m_canvas_elements;
    Optional<UniqueNodeID> m_media_context_menu_element_id;

    Web::HTML::MuteState m_mute_state { Web::HTML::MuteState::Unmuted };

    Optional<String> m_user_style_sheet_source;

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-supported
    // Each user agent has a PDF viewer supported boolean, whose value is implementation-defined (and might vary
    // according to user preferences).
    // NOTE: This value also impacts the navigation processing model.
    bool m_pdf_viewer_supported { true };

    size_t m_find_in_page_match_index { 0 };
    Optional<FindInPageQuery> m_last_find_in_page_query;
    URL::URL m_last_find_in_page_url;

    bool m_listen_for_dom_mutations { false };
    Optional<CSS::PreferredColorScheme> m_preferred_color_scheme_override_for_testing;

    struct PendingFullscreenEnter {
        GC::Ref<DOM::Element> element;
        GC::Ref<DOM::Document> pending_doc;
        DOM::RequestFullscreenError error;
        GC::Ref<WebIDL::Promise> promise;
    };

    struct PendingFullscreenExit {
        GC::Ref<DOM::Document> doc;
        bool resize;
        GC::Ref<WebIDL::Promise> promise;
    };

    using PendingFullscreenOperation = Variant<PendingFullscreenEnter, PendingFullscreenExit>;

    Queue<PendingFullscreenOperation> m_pending_fullscreen_operations;
    ViewportIsFullscreen m_viewport_is_fullscreen { ViewportIsFullscreen::No };
    bool m_fullscreen_ipc_sent_to_ui { false };
    bool m_processing_fullscreen_operations { false };
};

enum class ContextMenuForInputEventsTarget : u8 {
    No,
    Yes,
};

enum class HistoryTraversalPrecheck : u8 {
    Needed,
    AlreadyDone,
};

enum class NavigationTarget : u8 {
    TopLevel,
    IFrame,
};

enum class NavigationProcessDecision : u8 {
    Local,
    Remote,
};

class PageClient : public JS::Cell {
    GC_CELL(PageClient, JS::Cell);

public:
    virtual u64 id() const = 0;
    virtual Page& page() = 0;
    virtual Page const& page() const = 0;
    virtual bool is_connection_open() const = 0;
    virtual bool has_focus() const { return true; }
    virtual bool has_active_devtools_client() const { return false; }
    // In Ladybird, Remote currently implies replacing the WebContent process.
    virtual NavigationProcessDecision decide_navigation_process(
        [[maybe_unused]] URL::URL const& current_url,
        [[maybe_unused]] URL::URL const& target_url,
        [[maybe_unused]] NavigationTarget target = NavigationTarget::TopLevel,
        [[maybe_unused]] Optional<String> frame_id = {}) const
    {
        return NavigationProcessDecision::Local;
    }
    virtual void request_new_process_for_navigation(URL::URL const&, Variant<Empty, String, HTML::POSTResource>, Bindings::NavigationHistoryBehavior) { }
    virtual void request_new_process_for_child_frame_navigation(String const&, URL::URL const&, Variant<Empty, String, HTML::POSTResource>, Bindings::NavigationHistoryBehavior) { }
    virtual void page_did_create_child_frame(String const&, String const&) { }
    virtual void page_did_update_child_frame_viewport(String const&, CSSPixelRect) { }
    virtual void page_did_commit_child_frame_navigation(String const&, URL::URL const&) { }
    virtual void page_did_destroy_child_frame(String const&) { }
    virtual Optional<Compositor::CompositorContextId> compositor_context_id_for_remote_child_frame(String const&) const { return {}; }
    virtual String dump_site_isolation_process_tree_for_testing() { return {}; }
    virtual Gfx::Palette palette() const = 0;
    virtual DevicePixelRect screen_rect() const = 0;
    virtual double zoom_level() const = 0;
    virtual double device_pixel_ratio() const = 0;
    virtual double device_pixels_per_css_pixel() const = 0;
    virtual CSS::PreferredColorScheme preferred_color_scheme() const = 0;
    virtual CSS::PreferredContrast preferred_contrast() const = 0;
    virtual CSS::PreferredMotion preferred_motion() const = 0;
    virtual size_t screen_count() const = 0;
    virtual Queue<QueuedInputEvent>& input_event_queue() = 0;
    virtual void report_finished_handling_input_event(u64 page_id, EventResult event_was_handled) = 0;
    virtual Compositor::CompositorContextId allocate_compositor_context_id(Compositor::PagePresentationRegistration page_presentation_registration)
    {
        if (page_presentation_registration == Compositor::PagePresentationRegistration::Yes)
            return Compositor::compositor_context_id_for_page(id());
        VERIFY_NOT_REACHED();
    }
    virtual void request_frame() = 0;
    virtual void page_did_change_title(Utf16String const&) { }
    virtual void page_did_change_url(URL::URL const&) { }
    virtual void page_did_request_refresh() { }
    virtual void page_did_request_resize_window(Gfx::IntSize) { }
    virtual void page_did_request_reposition_window(Gfx::IntPoint) { }
    virtual void page_did_request_restore_window() { }
    virtual void page_did_request_maximize_window() { }
    virtual void page_did_request_minimize_window() { }
    virtual void page_did_request_fullscreen_window() { }
    virtual void page_did_request_exit_fullscreen() { }
    virtual void page_did_start_loading(URL::URL const&, Variant<Empty, String, HTML::POSTResource> document_resource, bool is_redirect, Bindings::NavigationHistoryBehavior history_handling = Bindings::NavigationHistoryBehavior::Auto)
    {
        (void)document_resource;
        (void)is_redirect;
        (void)history_handling;
    }
    virtual void page_did_cancel_loading(URL::URL const&) { }
    virtual void page_did_create_new_document(Web::DOM::Document&) { }
    virtual void page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document&) { }
    virtual void page_did_finish_loading(URL::URL const&) { }
    virtual Optional<u64> page_did_start_download(URL::URL const&, ByteString const& suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data)
    {
        (void)suggested_filename;
        (void)total_size;
        (void)request_server_client_id;
        (void)request_server_request_id;
        (void)initial_data;
        return {};
    }
    virtual Optional<u64> page_did_start_download(URL::URL const&, ByteString const& suggested_filename, Optional<u64> total_size)
    {
        (void)suggested_filename;
        (void)total_size;
        return {};
    }
    virtual void page_did_receive_download_data([[maybe_unused]] u64 download_id, [[maybe_unused]] ByteBuffer data) { }
    virtual void page_did_finish_download([[maybe_unused]] u64 download_id) { }
    virtual void page_did_fail_download([[maybe_unused]] u64 download_id, [[maybe_unused]] String const& error) { }
    virtual void page_did_register_download_controller([[maybe_unused]] u64 download_id, [[maybe_unused]] GC::Ref<Fetch::Infrastructure::FetchController>) { }
    virtual void page_did_register_download_reader([[maybe_unused]] u64 download_id, [[maybe_unused]] GC::Ref<Streams::ReadableStreamDefaultReader>) { }
    virtual void page_did_unregister_download([[maybe_unused]] u64 download_id) { }
    virtual bool page_is_download_canceled([[maybe_unused]] u64 download_id) const { return false; }
    virtual void page_did_request_cursor_change(Gfx::Cursor const&) { }
    virtual void page_did_request_context_menu(CSSPixelPoint, ContextMenuForInputEventsTarget) { }
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
    virtual void page_did_request_alert(String const&) { }
    virtual void page_did_request_confirm(String const&) { }
    virtual void page_did_request_prompt(String const&, String const&) { }
    virtual void page_did_request_set_prompt_text(String const&) { }
    virtual void page_did_request_accept_dialog() { }
    virtual void page_did_request_dismiss_dialog() { }
    virtual Optional<Core::SharedVersion> page_did_request_document_cookie_version([[maybe_unused]] Core::SharedVersionIndex document_index) { return {}; }
    virtual void page_did_receive_document_cookie_version_buffer([[maybe_unused]] Core::AnonymousBuffer document_cookie_version_buffer) { }
    virtual void page_did_request_document_cookie_version_index([[maybe_unused]] UniqueNodeID document_id, [[maybe_unused]] String const& domain) { }
    virtual void page_did_receive_document_cookie_version_index([[maybe_unused]] UniqueNodeID document_id, [[maybe_unused]] Core::SharedVersionIndex document_index) { }
    virtual Vector<HTTP::Cookie::Cookie> page_did_request_all_cookies_webdriver(URL::URL const&) { return {}; }
    virtual Vector<HTTP::Cookie::Cookie> page_did_request_all_cookies_cookiestore(URL::URL const&) { return {}; }
    virtual Optional<HTTP::Cookie::Cookie> page_did_request_named_cookie(URL::URL const&, String const&) { return {}; }
    virtual HTTP::Cookie::VersionedCookie page_did_request_cookie(URL::URL const&, HTTP::Cookie::Source) { return {}; }
    virtual void page_did_set_cookie(URL::URL const&, HTTP::Cookie::ParsedCookie const&, HTTP::Cookie::Source) { }
    virtual void page_did_update_cookie(HTTP::Cookie::Cookie const&) { }
    virtual void page_did_expire_cookies_with_time_offset(AK::Duration) { }
    virtual void page_did_delete_all_cookies(URL::URL const&, GC::Ref<WebIDL::Promise>) { }
    virtual void page_did_store_hsts_policy(String const&, HTTP::HSTS::ParsedHSTSPolicy const&) { }
    virtual bool page_did_is_known_hsts_host(String const&) { return false; }
    virtual Optional<String> page_did_request_storage_item([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& storage_key, [[maybe_unused]] String const& bottle_key) { return {}; }
    virtual WebView::StorageSetResult page_did_set_storage_item([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& storage_key, [[maybe_unused]] String const& bottle_key, [[maybe_unused]] String const& value) { return WebView::StorageOperationError::QuotaExceededError; }
    virtual void page_did_remove_storage_item([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& storage_key, [[maybe_unused]] String const& bottle_key) { }
    virtual Vector<String> page_did_request_storage_keys([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& storage_key) { return {}; }
    virtual u64 page_did_request_storage_usage([[maybe_unused]] String const& storage_key) { return {}; }
    virtual void page_did_clear_storage([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& storage_key) { }
    virtual void page_did_broadcast_storage_change([[maybe_unused]] Web::StorageAPI::StorageEndpointType storage_endpoint, [[maybe_unused]] String const& url, [[maybe_unused]] Optional<String> const& key, [[maybe_unused]] Optional<String> const& old_value, [[maybe_unused]] Optional<String> const& new_value) { }
    virtual void page_did_update_indexed_database([[maybe_unused]] String const& url, [[maybe_unused]] IndexedDB::TransactionChanges const&) { }
    virtual void page_did_update_resource_count(i32) { }
    struct NewWebViewResult {
        GC::Ptr<Page> page;
        String window_handle;
    };
    virtual NewWebViewResult page_did_request_new_web_view(HTML::ActivateTab, HTML::WebViewHints, HTML::TokenizedFeature::NoOpener) { return {}; }
    virtual void page_did_request_activate_tab() { }
    virtual void page_did_close_top_level_traversable() { }
    virtual bool should_report_session_history_updates() const { return true; }
    virtual void page_did_update_session_history([[maybe_unused]] Vector<HTML::SessionHistoryEntryDescriptor> const& entries, [[maybe_unused]] Vector<i32> const& used_steps, [[maybe_unused]] size_t current_used_step_index) { }
    virtual String page_did_request_ui_process_session_history_for_testing() { return "{}"_string; }
    virtual String page_did_update_session_history_and_request_ui_process_session_history_for_testing([[maybe_unused]] Vector<HTML::SessionHistoryEntryDescriptor> const& entries, [[maybe_unused]] Vector<i32> const& used_steps, [[maybe_unused]] size_t current_used_step_index) { return "{}"_string; }
    virtual bool page_did_request_traverse_the_history_by_delta([[maybe_unused]] int delta, [[maybe_unused]] HistoryTraversalPrecheck history_traversal_precheck) { return false; }
    virtual void page_did_change_needs_beforeunload_check([[maybe_unused]] bool needs_beforeunload_check) { }

    virtual void request_file(FileRequest) = 0;

    // https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
    virtual void page_did_request_color_picker([[maybe_unused]] Color current_color) { }
    virtual void page_did_request_file_picker([[maybe_unused]] HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles) { }
    virtual void page_did_request_select_dropdown([[maybe_unused]] Web::CSSPixelPoint content_position, [[maybe_unused]] Web::CSSPixels minimum_width, [[maybe_unused]] Vector<Web::HTML::SelectItem> items) { }

    virtual void page_did_finish_test([[maybe_unused]] String const& text) { }
    virtual void page_did_set_test_timeout([[maybe_unused]] double milliseconds) { }
    virtual void page_did_receive_reference_test_metadata(JsonValue) { }

    virtual void page_did_set_browser_zoom([[maybe_unused]] double factor) { }
    virtual void page_did_set_device_pixel_ratio_for_testing([[maybe_unused]] double ratio) { }

    virtual void page_did_change_theme_color(Gfx::Color) { }
    virtual void page_did_change_background_color(Gfx::Color) { }

    virtual void page_did_insert_clipboard_entry(Clipboard::SystemClipboardRepresentation const&, [[maybe_unused]] StringView presentation_style) { }
    virtual void page_did_request_clipboard_entries([[maybe_unused]] u64 request_id) { }
    virtual void page_did_request_primary_paste() { }
    virtual void page_did_update_primary_selection(String const&) { }

    virtual void page_did_change_audio_play_state(HTML::AudioPlayState) { }

    virtual void page_did_start_network_request([[maybe_unused]] u64 request_id, [[maybe_unused]] URL::URL const& url, [[maybe_unused]] ByteString const& method, [[maybe_unused]] Vector<HTTP::Header> const& request_headers, [[maybe_unused]] ReadonlyBytes request_body, [[maybe_unused]] Optional<String> initiator_type) { }
    virtual void page_did_receive_network_response_headers([[maybe_unused]] u64 request_id, [[maybe_unused]] u32 status_code, [[maybe_unused]] Optional<String> reason_phrase, [[maybe_unused]] Vector<HTTP::Header> const& response_headers) { }
    virtual void page_did_receive_network_response_body([[maybe_unused]] u64 request_id, [[maybe_unused]] ReadonlyBytes data) { }
    virtual void page_did_finish_network_request([[maybe_unused]] u64 request_id, [[maybe_unused]] u64 body_size, [[maybe_unused]] Requests::RequestTimingInfo const& timing_info, [[maybe_unused]] Optional<Requests::NetworkError> const& network_error) { }
    virtual void page_did_report_worker_exception([[maybe_unused]] String const& message, [[maybe_unused]] String const& filename, [[maybe_unused]] u32 lineno, [[maybe_unused]] u32 colno) { }
    virtual void page_did_register_javascript_source([[maybe_unused]] DOM::Document&, [[maybe_unused]] HTML::ScriptRegistry::Description const&) { }
    virtual void page_did_post_broadcast_channel_message([[maybe_unused]] HTML::BroadcastChannelMessage const& message) { }

    virtual HTML::WorkerAgentId start_worker_agent([[maybe_unused]] HTML::WorkerAgentStartRequest&& request) { return {}; }
    virtual void close_worker_agent([[maybe_unused]] HTML::WorkerAgentId agent_id, [[maybe_unused]] HTML::WorkerAgentOwnerToken owner_token) { }

    virtual void page_did_mutate_dom([[maybe_unused]] FlyString const& type, [[maybe_unused]] DOM::Node const& target, [[maybe_unused]] DOM::NodeList& added_nodes, [[maybe_unused]] DOM::NodeList& removed_nodes, [[maybe_unused]] GC::Ptr<DOM::Node> previous_sibling, [[maybe_unused]] GC::Ptr<DOM::Node> next_sibling, [[maybe_unused]] Optional<String> const& attribute_name) { }
    virtual void flush_pending_dom_mutations() { }

    virtual void page_did_take_screenshot(Gfx::ShareableBitmap const&) { }

    virtual void received_message_from_web_ui([[maybe_unused]] String const& name, [[maybe_unused]] JS::Value data) { }

    virtual bool is_headless() const = 0;

    virtual bool is_svg_page_client() const { return false; }
    virtual bool supports_compositor() const { return false; }
    virtual void ensure_compositor_host() { }
    virtual Compositor::CompositorHost* compositor_host() { return nullptr; }
    virtual Compositor::CompositorHost const* compositor_host() const { return nullptr; }

protected:
    virtual ~PageClient() = default;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Page::MediaContextMenu const&);

template<>
WEB_API ErrorOr<Web::Page::MediaContextMenu> decode(Decoder&);

}
