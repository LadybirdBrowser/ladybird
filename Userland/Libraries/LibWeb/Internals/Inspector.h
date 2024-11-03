/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Internals {

class Inspector final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Inspector, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Inspector);

public:
    virtual ~Inspector() override;

    void inspector_loaded();
    void inspect_dom_node(i64 node_id, Optional<i32> const& pseudo_element);

    void set_dom_node_text(i64 node_id, String const& text);
    void set_dom_node_tag(i64 node_id, String const& tag);
    void add_dom_node_attributes(i64 node_id, JS::NonnullGCPtr<DOM::NamedNodeMap> attributes);
    void replace_dom_node_attribute(i64 node_id, WebIDL::UnsignedLongLong attribute_index, JS::NonnullGCPtr<DOM::NamedNodeMap> replacement_attributes);

    void request_dom_tree_context_menu(i64 node_id, i32 client_x, i32 client_y, String const& type, Optional<String> const& tag, Optional<WebIDL::UnsignedLongLong> const& attribute_index);

    void request_cookie_context_menu(WebIDL::UnsignedLongLong cookie_index, i32 client_x, i32 client_y);

    void request_style_sheet_source(String const& type, Optional<i64> const& dom_node_unique_id, Optional<String> const& url);

    void execute_console_script(String const& script);

    void export_inspector_html(String const& html);

private:
    explicit Inspector(JS::Realm&);

    PageClient& inspector_page_client() const;

    virtual void initialize(JS::Realm&) override;
};

}
