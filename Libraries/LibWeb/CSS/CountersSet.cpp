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

bool innermost_list_item_counter_is_own_forward_counter(DOM::Element const& element)
{
    // A layout tree build resolves the element's counters from the style it has installed.
    auto const& style = element.installed_style();
    if (!style)
        return false;
    return Layout::RustFFI::style_resets_forward_list_item_counter(style.view().payloads, style.view().payload_count);
}

Utf16FlyString const& list_item_counter_name()
{
    static NeverDestroyed<Utf16FlyString> name = "list-item"_utf16_fly_string;
    return *name;
}

}
