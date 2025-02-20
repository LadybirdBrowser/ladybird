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
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace DevTools {

class DevToolsDelegate {
public:
    virtual ~DevToolsDelegate() = default;

    virtual Vector<TabDescription> tab_list() const { return {}; }
    virtual Vector<CSSProperty> css_property_list() const { return {}; }

    using OnTabInspectionComplete = Function<void(ErrorOr<JsonValue>)>;
    virtual void inspect_tab(TabDescription const&, OnTabInspectionComplete) const { }

    virtual void highlight_dom_node(TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::Selector::PseudoElement::Type>) const { }
    virtual void clear_highlighted_dom_node(TabDescription const&) const { }
};

}
