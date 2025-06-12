/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/Vector.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Forward.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class DevToolsDelegate {
public:
    virtual ~DevToolsDelegate() = default;

    virtual Vector<TabDescription> tab_list() const { return {}; }
    virtual Vector<CSSProperty> css_property_list() const { return {}; }

    using OnTabInspectionComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void inspect_tab(TabDescription const&, OnTabInspectionComplete) const { }

    using OnDOMNodePropertiesReceived = Function<void(WebView::DOMNodeProperties)>;
    virtual void listen_for_dom_properties(TabDescription const&, OnDOMNodePropertiesReceived) const { }
    virtual void stop_listening_for_dom_properties(TabDescription const&) const { }
    virtual void inspect_dom_node(TabDescription const&, WebView::DOMNodeProperties::Type, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const { }
    virtual void clear_inspected_dom_node(TabDescription const&) const { }

    virtual void highlight_dom_node(TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const { }
    virtual void clear_highlighted_dom_node(TabDescription const&) const { }

    using OnDOMMutationReceived = Function<void(WebView::Mutation)>;
    virtual void listen_for_dom_mutations(TabDescription const&, OnDOMMutationReceived) const { }
    virtual void stop_listening_for_dom_mutations(TabDescription const&) const { }

    using OnDOMNodeHTMLReceived = Function<void(ErrorOr<String>)>;
    using OnDOMNodeEditComplete = Function<void(ErrorOr<Web::UniqueNodeID>)>;
    virtual void get_dom_node_inner_html(TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const { }
    virtual void get_dom_node_outer_html(TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const { }
    virtual void set_dom_node_outer_html(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void set_dom_node_text(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void set_dom_node_tag(TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const { }
    virtual void add_dom_node_attributes(TabDescription const&, Web::UniqueNodeID, ReadonlySpan<WebView::Attribute>, OnDOMNodeEditComplete) const { }
    virtual void replace_dom_node_attribute(TabDescription const&, Web::UniqueNodeID, String const&, ReadonlySpan<WebView::Attribute>, OnDOMNodeEditComplete) const { }
    virtual void create_child_element(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }
    virtual void insert_dom_node_before(TabDescription const&, Web::UniqueNodeID, Web::UniqueNodeID, Optional<Web::UniqueNodeID>, OnDOMNodeEditComplete) const { }
    virtual void clone_dom_node(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }
    virtual void remove_dom_node(TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const { }

    using OnStyleSheetsReceived = Function<void(ErrorOr<Vector<Web::CSS::StyleSheetIdentifier>>)>;
    using OnStyleSheetSourceReceived = Function<void(Web::CSS::StyleSheetIdentifier const&, String)>;
    virtual void retrieve_style_sheets(TabDescription const&, OnStyleSheetsReceived) const { }
    virtual void retrieve_style_sheet_source(TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const { }
    virtual void listen_for_style_sheet_sources(TabDescription const&, OnStyleSheetSourceReceived) const { }
    virtual void stop_listening_for_style_sheet_sources(TabDescription const&) const { }

    using OnScriptEvaluationComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void evaluate_javascript(TabDescription const&, String const&, OnScriptEvaluationComplete) const { }

    using OnConsoleMessageAvailable = Function<void(i32 message_id)>;
    using OnReceivedConsoleMessages = Function<void(i32 start_index, Vector<WebView::ConsoleOutput>)>;
    virtual void listen_for_console_messages(TabDescription const&, OnConsoleMessageAvailable, OnReceivedConsoleMessages) const { }
    virtual void stop_listening_for_console_messages(TabDescription const&) const { }
    virtual void request_console_messages(TabDescription const&, i32) const { }
};

}
