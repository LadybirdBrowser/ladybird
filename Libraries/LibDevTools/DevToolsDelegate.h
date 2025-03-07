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
#include <LibWeb/Forward.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class DevToolsDelegate {
public:
    virtual ~DevToolsDelegate() = default;

    virtual Vector<TabDescription> tab_list() const { return {}; }
    virtual Vector<CSSProperty> css_property_list() const { return {}; }

    using OnTabInspectionComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void inspect_tab(TabDescription const&, OnTabInspectionComplete) const { }

    using OnDOMNodeInspectionComplete = Function<void(ErrorOr<DOMNodeProperties>)>;
    virtual void inspect_dom_node(TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::Selector::PseudoElement::Type>, OnDOMNodeInspectionComplete) const { }
    virtual void clear_inspected_dom_node(TabDescription const&) const { }

    virtual void highlight_dom_node(TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::Selector::PseudoElement::Type>) const { }
    virtual void clear_highlighted_dom_node(TabDescription const&) const { }

    using OnDOMMutationReceived = Function<void(WebView::Mutation)>;
    virtual void listen_for_dom_mutations(TabDescription const&, OnDOMMutationReceived) const { }
    virtual void stop_listening_for_dom_mutations(TabDescription const&) const { }

    using OnScriptEvaluationComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void evaluate_javascript(TabDescription const&, String, OnScriptEvaluationComplete) const { }

    using OnConsoleMessageAvailable = Function<void(i32 message_id)>;
    using OnReceivedConsoleMessages = Function<void(i32 start_index, Vector<WebView::ConsoleOutput>)>;
    virtual void listen_for_console_messages(TabDescription const&, OnConsoleMessageAvailable, OnReceivedConsoleMessages) const { }
    virtual void stop_listening_for_console_messages(TabDescription const&) const { }
    virtual void request_console_messages(TabDescription const&, i32) const { }
};

}
