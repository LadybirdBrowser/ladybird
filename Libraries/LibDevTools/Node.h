/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/DOM/NodeType.h>
#include <LibWeb/Forward.h>

namespace DevTools {

struct DEVTOOLS_API NodeIdentifier {
    static NodeIdentifier for_node(JsonObject const& node);

    bool operator==(NodeIdentifier const&) const = default;

    Web::UniqueNodeID id { 0 };
    Optional<Web::CSS::PseudoElement> pseudo_element;
};

struct DEVTOOLS_API Node {
    JsonObject const& node;
    NodeIdentifier identifier;
    NonnullRefPtr<TabActor> tab;
};

static constexpr Web::DOM::NodeType parse_dom_node_type(StringView type)
{
    if (type == "document"sv)
        return Web::DOM::NodeType::DOCUMENT_NODE;
    if (type == "element"sv)
        return Web::DOM::NodeType::ELEMENT_NODE;
    if (type == "text"sv)
        return Web::DOM::NodeType::TEXT_NODE;
    if (type == "comment"sv)
        return Web::DOM::NodeType::COMMENT_NODE;
    return Web::DOM::NodeType::INVALID;
}

}
