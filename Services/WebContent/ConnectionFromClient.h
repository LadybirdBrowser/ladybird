/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Queue.h>
#include <AK/SourceLocation.h>
#include <LibGC/Root.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PageInfo.h>
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

    PageHost& page_host() { return *m_page_host; }
    PageHost const& page_host() const { return *m_page_host; }

    Function<void(IPC::File const&)> on_image_decoder_connection;

    Queue<Web::QueuedInputEvent>& input_event_queue() { return m_input_event_queue; }

private:
    explicit ConnectionFromClient(GC::Heap&, IPC::Transport);

    Optional<PageClient&> page(u64 index, SourceLocation = SourceLocation::current());
    Optional<PageClient const&> page(u64 index, SourceLocation = SourceLocation::current()) const;

    virtual Messages::WebContentServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void close_server() override;
    virtual Messages::WebContentServer::GetWindowHandleResponse get_window_handle(u64 page_id) override;
    virtual void set_window_handle(u64 page_id, String handle) override;
    virtual void connect_to_webdriver(u64 page_id, ByteString webdriver_ipc_path) override;
    virtual void connect_to_web_ui(u64 page_id, IPC::File web_ui_socket) override;
    virtual void connect_to_image_decoder(IPC::File image_decoder_socket) override;
    virtual void update_system_theme(u64 page_id, Core::AnonymousBuffer) override;
    virtual void update_screen_rects(u64 page_id, Vector<Web::DevicePixelRect>, u32) override;
    virtual void load_url(u64 page_id, URL::URL) override;
    virtual void load_html(u64 page_id, ByteString) override;
    virtual void reload(u64 page_id) override;
    virtual void traverse_the_history_by_delta(u64 page_id, i32 delta) override;
    virtual void set_viewport_size(u64 page_id, Web::DevicePixelSize) override;
    virtual void key_event(u64 page_id, Web::KeyEvent) override;
    virtual void mouse_event(u64 page_id, Web::MouseEvent) override;
    virtual void drag_event(u64 page_id, Web::DragEvent) override;
    virtual void ready_to_paint(u64 page_id) override;
    virtual void debug_request(u64 page_id, ByteString, ByteString) override;
    virtual void get_source(u64 page_id) override;
    virtual void inspect_dom_tree(u64 page_id) override;
    virtual void inspect_dom_node(u64 page_id, WebView::DOMNodeProperties::Type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) override;
    virtual void clear_inspected_dom_node(u64 page_id) override;
    virtual void highlight_dom_node(u64 page_id, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) override;
    virtual void inspect_accessibility_tree(u64 page_id) override;
    virtual void get_hovered_node_id(u64 page_id) override;

    virtual void list_style_sheets(u64 page_id) override;
    virtual void request_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier) override;

    virtual void set_listen_for_dom_mutations(u64 page_id, bool) override;
    virtual void get_dom_node_inner_html(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void get_dom_node_outer_html(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void set_dom_node_outer_html(u64 page_id, Web::UniqueNodeID node_id, String html) override;
    virtual void set_dom_node_text(u64 page_id, Web::UniqueNodeID node_id, String text) override;
    virtual void set_dom_node_tag(u64 page_id, Web::UniqueNodeID node_id, String name) override;
    virtual void add_dom_node_attributes(u64 page_id, Web::UniqueNodeID node_id, Vector<WebView::Attribute> attributes) override;
    virtual void replace_dom_node_attribute(u64 page_id, Web::UniqueNodeID node_id, String name, Vector<WebView::Attribute> replacement_attributes) override;
    virtual void create_child_element(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void create_child_text_node(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void insert_dom_node_before(u64 page_id, Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id) override;
    virtual void clone_dom_node(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void remove_dom_node(u64 page_id, Web::UniqueNodeID node_id) override;

    virtual void set_content_filters(u64 page_id, Vector<String>) override;
    virtual void set_autoplay_allowed_on_all_websites(u64 page_id) override;
    virtual void set_autoplay_allowlist(u64 page_id, Vector<String> allowlist) override;
    virtual void set_proxy_mappings(u64 page_id, Vector<ByteString>, HashMap<ByteString, size_t>) override;
    virtual void set_preferred_color_scheme(u64 page_id, Web::CSS::PreferredColorScheme) override;
    virtual void set_preferred_contrast(u64 page_id, Web::CSS::PreferredContrast) override;
    virtual void set_preferred_motion(u64 page_id, Web::CSS::PreferredMotion) override;
    virtual void set_preferred_languages(u64 page_id, Vector<String>) override;
    virtual void set_enable_do_not_track(u64 page_id, bool) override;
    virtual void set_has_focus(u64 page_id, bool) override;
    virtual void set_is_scripting_enabled(u64 page_id, bool) override;
    virtual void set_device_pixels_per_css_pixel(u64 page_id, float) override;
    virtual void set_window_position(u64 page_id, Web::DevicePixelPoint) override;
    virtual void set_window_size(u64 page_id, Web::DevicePixelSize) override;
    virtual void did_update_window_rect(u64 page_id) override;
    virtual void handle_file_return(u64 page_id, i32 error, Optional<IPC::File> file, i32 request_id) override;
    virtual void set_system_visibility_state(u64 page_id, Web::HTML::VisibilityState) override;

    virtual void js_console_input(u64 page_id, String) override;
    virtual void run_javascript(u64 page_id, String) override;
    virtual void js_console_request_messages(u64 page_id, i32) override;

    virtual void alert_closed(u64 page_id) override;
    virtual void confirm_closed(u64 page_id, bool accepted) override;
    virtual void prompt_closed(u64 page_id, Optional<String> response) override;
    virtual void color_picker_update(u64 page_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state) override;
    virtual void file_picker_closed(u64 page_id, Vector<Web::HTML::SelectedFile> selected_files) override;
    virtual void select_dropdown_closed(u64 page_id, Optional<u32> selected_item_id) override;

    virtual void toggle_media_play_state(u64 page_id) override;
    virtual void toggle_media_mute_state(u64 page_id) override;
    virtual void toggle_media_loop_state(u64 page_id) override;
    virtual void toggle_media_controls_state(u64 page_id) override;

    virtual void toggle_page_mute_state(u64 page_id) override;

    virtual void set_user_style(u64 page_id, String) override;

    virtual void take_document_screenshot(u64 page_id) override;
    virtual void take_dom_node_screenshot(u64 page_id, Web::UniqueNodeID node_id) override;

    virtual void request_internal_page_info(u64 page_id, WebView::PageInfoType) override;

    virtual Messages::WebContentServer::GetLocalStorageEntriesResponse get_local_storage_entries(u64 page_id) override;
    virtual Messages::WebContentServer::GetSessionStorageEntriesResponse get_session_storage_entries(u64 page_id) override;

    virtual Messages::WebContentServer::GetSelectedTextResponse get_selected_text(u64 page_id) override;
    virtual void select_all(u64 page_id) override;

    virtual void find_in_page(u64 page_id, String query, CaseSensitivity) override;
    virtual void find_in_page_next_match(u64 page_id) override;
    virtual void find_in_page_previous_match(u64 page_id) override;

    virtual void paste(u64 page_id, String text) override;

    virtual void system_time_zone_changed() override;

    GC::Heap& m_heap;
    NonnullOwnPtr<PageHost> m_page_host;

    HashMap<int, Web::FileRequest> m_requested_files {};
    int last_id { 0 };

    void enqueue_input_event(Web::QueuedInputEvent);

    Queue<Web::QueuedInputEvent> m_input_event_queue;
};

}
