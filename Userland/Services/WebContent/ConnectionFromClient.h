/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Queue.h>
#include <AK/SourceLocation.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWebView/Forward.h>
#include <WebContent/Forward.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentConsoleClient.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace WebContent {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<WebContentClientEndpoint, WebContentServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override;

    virtual void die() override;

    void request_file(u64 page_id, Web::FileRequest);

    Optional<int> fd() { return socket().fd(); }

    PageHost& page_host() { return *m_page_host; }
    PageHost const& page_host() const { return *m_page_host; }

    Function<void(IPC::File const&)> on_image_decoder_connection;

private:
    explicit ConnectionFromClient(NonnullOwnPtr<Core::LocalSocket>);

    Optional<PageClient&> page(u64 index, SourceLocation = SourceLocation::current());
    Optional<PageClient const&> page(u64 index, SourceLocation = SourceLocation::current()) const;

    virtual void close_server() override;
    virtual Messages::WebContentServer::GetWindowHandleResponse get_window_handle(u64 page_id) override;
    virtual void set_window_handle(u64 page_id, String const& handle) override;
    virtual void connect_to_webdriver(u64 page_id, ByteString const& webdriver_ipc_path) override;
    virtual void connect_to_image_decoder(IPC::File const& image_decoder_socket) override;
    virtual void update_system_theme(u64 page_id, Core::AnonymousBuffer const&) override;
    virtual void update_screen_rects(u64 page_id, Vector<Web::DevicePixelRect> const&, u32) override;
    virtual void load_url(u64 page_id, URL::URL const&) override;
    virtual void load_html(u64 page_id, ByteString const&) override;
    virtual void reload(u64 page_id) override;
    virtual void traverse_the_history_by_delta(u64 page_id, i32 delta) override;
    virtual void set_viewport_size(u64 page_id, Web::DevicePixelSize const) override;
    virtual void key_event(u64 page_id, Web::KeyEvent const&) override;
    virtual void mouse_event(u64 page_id, Web::MouseEvent const&) override;
    virtual void ready_to_paint(u64 page_id) override;
    virtual void debug_request(u64 page_id, ByteString const&, ByteString const&) override;
    virtual void get_source(u64 page_id) override;
    virtual void inspect_dom_tree(u64 page_id) override;
    virtual void inspect_dom_node(u64 page_id, i32 node_id, Optional<Web::CSS::Selector::PseudoElement::Type> const& pseudo_element) override;
    virtual void inspect_accessibility_tree(u64 page_id) override;
    virtual void get_hovered_node_id(u64 page_id) override;

    virtual void set_dom_node_text(u64 page_id, i32 node_id, String const& text) override;
    virtual void set_dom_node_tag(u64 page_id, i32 node_id, String const& name) override;
    virtual void add_dom_node_attributes(u64 page_id, i32 node_id, Vector<WebView::Attribute> const& attributes) override;
    virtual void replace_dom_node_attribute(u64 page_id, i32 node_id, String const& name, Vector<WebView::Attribute> const& replacement_attributes) override;
    virtual void create_child_element(u64 page_id, i32 node_id) override;
    virtual void create_child_text_node(u64 page_id, i32 node_id) override;
    virtual void clone_dom_node(u64 page_id, i32 node_id) override;
    virtual void remove_dom_node(u64 page_id, i32 node_id) override;
    virtual void get_dom_node_html(u64 page_id, i32 node_id) override;

    virtual Messages::WebContentServer::DumpLayoutTreeResponse dump_layout_tree(u64 page_id) override;
    virtual Messages::WebContentServer::DumpPaintTreeResponse dump_paint_tree(u64 page_id) override;
    virtual Messages::WebContentServer::DumpTextResponse dump_text(u64 page_id) override;
    virtual void set_content_filters(u64 page_id, Vector<String> const&) override;
    virtual void set_autoplay_allowed_on_all_websites(u64 page_id) override;
    virtual void set_autoplay_allowlist(u64 page_id, Vector<String> const& allowlist) override;
    virtual void set_proxy_mappings(u64 page_id, Vector<ByteString> const&, HashMap<ByteString, size_t> const&) override;
    virtual void set_preferred_color_scheme(u64 page_id, Web::CSS::PreferredColorScheme const&) override;
    virtual void set_preferred_contrast(u64 page_id, Web::CSS::PreferredContrast const&) override;
    virtual void set_preferred_motion(u64 page_id, Web::CSS::PreferredMotion const&) override;
    virtual void set_enable_do_not_track(u64 page_id, bool) override;
    virtual void set_has_focus(u64 page_id, bool) override;
    virtual void set_is_scripting_enabled(u64 page_id, bool) override;
    virtual void set_device_pixels_per_css_pixel(u64 page_id, float) override;
    virtual void set_window_position(u64 page_id, Web::DevicePixelPoint) override;
    virtual void set_window_size(u64 page_id, Web::DevicePixelSize) override;
    virtual void handle_file_return(u64 page_id, i32 error, Optional<IPC::File> const& file, i32 request_id) override;
    virtual void set_system_visibility_state(u64 page_id, bool visible) override;

    virtual void js_console_input(u64 page_id, ByteString const&) override;
    virtual void run_javascript(u64 page_id, ByteString const&) override;
    virtual void js_console_request_messages(u64 page_id, i32) override;

    virtual void alert_closed(u64 page_id) override;
    virtual void confirm_closed(u64 page_id, bool accepted) override;
    virtual void prompt_closed(u64 page_id, Optional<String> const& response) override;
    virtual void color_picker_update(u64 page_id, Optional<Color> const& picked_color, Web::HTML::ColorPickerUpdateState const& state) override;
    virtual void file_picker_closed(u64 page_id, Vector<Web::HTML::SelectedFile> const& selected_files) override;
    virtual void select_dropdown_closed(u64 page_id, Optional<u32> const& selected_item_id) override;

    virtual void toggle_media_play_state(u64 page_id) override;
    virtual void toggle_media_mute_state(u64 page_id) override;
    virtual void toggle_media_loop_state(u64 page_id) override;
    virtual void toggle_media_controls_state(u64 page_id) override;

    virtual void toggle_page_mute_state(u64 page_id) override;

    virtual void set_user_style(u64 page_id, String const&) override;

    virtual void enable_inspector_prototype(u64 page_id) override;

    virtual void take_document_screenshot(u64 page_id) override;
    virtual void take_dom_node_screenshot(u64 page_id, i32 node_id) override;

    virtual Messages::WebContentServer::DumpGcGraphResponse dump_gc_graph(u64 page_id) override;

    virtual Messages::WebContentServer::GetLocalStorageEntriesResponse get_local_storage_entries(u64 page_id) override;
    virtual Messages::WebContentServer::GetSessionStorageEntriesResponse get_session_storage_entries(u64 page_id) override;

    virtual Messages::WebContentServer::GetSelectedTextResponse get_selected_text(u64 page_id) override;
    virtual void select_all(u64 page_id) override;

    virtual void find_in_page(u64 page_id, String const& query, CaseSensitivity) override;
    virtual void find_in_page_next_match(u64 page_id) override;
    virtual void find_in_page_previous_match(u64 page_id) override;

    virtual void paste(u64 page_id, String const& text) override;

    void report_finished_handling_input_event(u64 page_id, bool event_was_handled);

    NonnullOwnPtr<PageHost> m_page_host;

    HashMap<int, Web::FileRequest> m_requested_files {};
    int last_id { 0 };

    struct QueuedInputEvent {
        u64 page_id { 0 };
        Web::InputEvent event;
        size_t coalesced_event_count { 0 };
    };

    void enqueue_input_event(QueuedInputEvent);
    void process_next_input_event();

    Queue<QueuedInputEvent> m_input_event_queue;

    RefPtr<Web::Platform::Timer> m_input_event_queue_timer;
};

}
