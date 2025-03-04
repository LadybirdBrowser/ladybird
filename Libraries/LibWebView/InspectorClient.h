/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibWebView/Attribute.h>
#include <LibWebView/ViewImplementation.h>

#pragma once

namespace WebView {

class InspectorClient {
public:
    InspectorClient(ViewImplementation& content_web_view, ViewImplementation& inspector_web_view);
    ~InspectorClient();

    void inspect();
    void reset();

    void select_hovered_node();
    void select_default_node();
    void clear_selection();

    void context_menu_edit_dom_node();
    void context_menu_copy_dom_node();
    void context_menu_screenshot_dom_node();
    void context_menu_create_child_element();
    void context_menu_create_child_text_node();
    void context_menu_clone_dom_node();
    void context_menu_remove_dom_node();
    void context_menu_add_dom_node_attribute();
    void context_menu_remove_dom_node_attribute();
    void context_menu_copy_dom_node_attribute_value();
    void context_menu_delete_cookie();
    void context_menu_delete_all_cookies();

    Function<void(Gfx::IntPoint)> on_requested_dom_node_text_context_menu;
    Function<void(Gfx::IntPoint, String const&)> on_requested_dom_node_tag_context_menu;
    Function<void(Gfx::IntPoint, String const&, Attribute const&)> on_requested_dom_node_attribute_context_menu;
    Function<void(Gfx::IntPoint, Web::Cookie::Cookie const&)> on_requested_cookie_context_menu;

private:
    void load_inspector();

    String generate_dom_tree(JsonObject const&);
    String generate_accessibility_tree(JsonObject const&);
    void select_node(Web::UniqueNodeID);

    void load_cookies();

    void request_console_messages();
    void console_message_available(i32 message_index);
    void console_messages_received(i32 start_index, ReadonlySpan<String> message_types, ReadonlySpan<String> messages);

    void append_console_source(StringView);
    void append_console_message(StringView);
    void append_console_warning(StringView);
    void append_console_output(StringView);
    void clear_console_output();

    void begin_console_group(StringView label, bool start_expanded);
    void end_console_group();

    ViewImplementation& m_content_web_view;
    ViewImplementation& m_inspector_web_view;

    Optional<Web::UniqueNodeID> m_body_or_frameset_node_id;
    Optional<Web::UniqueNodeID> m_pending_selection;

    bool m_inspector_loaded { false };
    bool m_dom_tree_loaded { false };

    struct ContextMenuData {
        Web::UniqueNodeID dom_node_id;
        Optional<String> tag;
        Optional<Attribute> attribute;
    };
    Optional<ContextMenuData> m_context_menu_data;

    HashMap<Web::UniqueNodeID, Vector<Attribute>> m_dom_node_attributes;

    Vector<Web::Cookie::Cookie> m_cookies;
    Optional<size_t> m_cookie_context_menu_index;

    i32 m_highest_notified_message_index { -1 };
    i32 m_highest_received_message_index { -1 };
    bool m_waiting_for_messages { false };
};

}
