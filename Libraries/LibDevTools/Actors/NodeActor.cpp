/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/NodeActor.h>
#include <LibDevTools/Actors/WalkerActor.h>

namespace DevTools {

NonnullRefPtr<NodeActor> NodeActor::create(DevToolsServer& devtools, String name, WeakPtr<WalkerActor> walker)
{
    return adopt_ref(*new NodeActor(devtools, move(name), move(walker)));
}

NodeActor::NodeActor(DevToolsServer& devtools, String name, WeakPtr<WalkerActor> walker)
    : Actor(devtools, move(name))
    , m_walker(move(walker))
{
}

NodeActor::~NodeActor() = default;

void NodeActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "getUniqueSelector"sv) {
        if (auto walker = m_walker.strong_ref()) {
            if (auto const& dom_node = walker->dom_node(name()); dom_node.has_value())
                response.set("value"sv, dom_node->node.get_string("name"sv)->to_ascii_lowercase());
        }

        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

}
