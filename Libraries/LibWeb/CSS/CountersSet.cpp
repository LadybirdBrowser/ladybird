/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::CSS {

static Layout::RustFFI::DocumentHost* document_host(DOM::Element const& element)
{
    auto* arena = element.document().layout_node_arena_if_created();
    return arena ? arena->host() : nullptr;
}

bool innermost_list_item_counter_is_own_forward_counter(Layout::BegunRead const& read, DOM::Element const& element)
{
    auto* host = document_host(element);
    if (!host)
        return false;
    return Layout::RustFFI::render_state_innermost_list_item_counter_is_own_forward_counter(host, &read, element.style_node_id().value());
}

Utf16FlyString const& list_item_counter_name()
{
    static NeverDestroyed<Utf16FlyString> name = "list-item"_utf16_fly_string;
    return *name;
}

}
