/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/InspectorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/Inspector.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

JS_DEFINE_ALLOCATOR(Inspector);

Inspector::Inspector(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

Inspector::~Inspector() = default;

void Inspector::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Inspector);
}

PageClient& Inspector::inspector_page_client() const
{
    return verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).page().client();
}

void Inspector::inspector_loaded()
{
    inspector_page_client().inspector_did_load();
}

void Inspector::inspect_dom_node(i32 node_id, Optional<i32> const& pseudo_element)
{
    inspector_page_client().inspector_did_select_dom_node(node_id, pseudo_element.map([](auto value) {
        VERIFY(value < to_underlying(Web::CSS::Selector::PseudoElement::Type::KnownPseudoElementCount));
        return static_cast<Web::CSS::Selector::PseudoElement::Type>(value);
    }));
}

void Inspector::set_dom_node_text(i32 node_id, String const& text)
{
    inspector_page_client().inspector_did_set_dom_node_text(node_id, text);
}

void Inspector::set_dom_node_tag(i32 node_id, String const& tag)
{
    inspector_page_client().inspector_did_set_dom_node_tag(node_id, tag);
}

void Inspector::add_dom_node_attributes(i32 node_id, JS::NonnullGCPtr<DOM::NamedNodeMap> attributes)
{
    inspector_page_client().inspector_did_add_dom_node_attributes(node_id, attributes);
}

void Inspector::replace_dom_node_attribute(i32 node_id, WebIDL::UnsignedLongLong attribute_index, JS::NonnullGCPtr<DOM::NamedNodeMap> replacement_attributes)
{
    inspector_page_client().inspector_did_replace_dom_node_attribute(node_id, static_cast<size_t>(attribute_index), replacement_attributes);
}

void Inspector::request_dom_tree_context_menu(i32 node_id, i32 client_x, i32 client_y, String const& type, Optional<String> const& tag, Optional<WebIDL::UnsignedLongLong> const& attribute_index)
{
    inspector_page_client().inspector_did_request_dom_tree_context_menu(node_id, { client_x, client_y }, type, tag, attribute_index.map([](auto index) { return static_cast<size_t>(index); }));
}

void Inspector::request_style_sheet_source(String const& type_string, Optional<i32> const& dom_node_unique_id, Optional<String> const& url)
{
    auto type = CSS::style_sheet_identifier_type_from_string(type_string);
    VERIFY(type.has_value());

    inspector_page_client().inspector_did_request_style_sheet_source({
        .type = type.value(),
        .dom_element_unique_id = dom_node_unique_id,
        .url = url,
    });
}

void Inspector::execute_console_script(String const& script)
{
    inspector_page_client().inspector_did_execute_console_script(script);
}

void Inspector::export_inspector_html(String const& html)
{
    inspector_page_client().inspector_did_export_inspector_html(html);
}

}
