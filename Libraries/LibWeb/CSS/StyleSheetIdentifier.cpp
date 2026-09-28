/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

Optional<StyleSheetIdentifier> style_sheet_identifier_for(StyleSheetState const& sheet)
{
    StyleSheetIdentifier identifier {};

    if (sheet.owner_import()) {
        identifier.type = StyleSheetIdentifier::Type::ImportRule;
    } else if (auto* node = sheet.owner_node()) {
        if (node->is_html_style_element() || node->is_svg_style_element()) {
            identifier.type = StyleSheetIdentifier::Type::StyleElement;
        } else if (node->is_html_link_element()) {
            identifier.type = StyleSheetIdentifier::Type::LinkElement;
        } else {
            dbgln("Can't identify where style sheet came from; owner node is {}", node->debug_description());
            identifier.type = StyleSheetIdentifier::Type::StyleElement;
        }
        identifier.dom_element_unique_id = node->unique_id();
    } else {
        dbgln("Style sheet has no owner rule or owner node; skipping");
        return {};
    }

    if (auto sheet_url = sheet.href_for_bindings(); sheet_url.has_value())
        identifier.url = sheet_url.release_value();

    identifier.rule_count = sheet.native_rules().size();
    return identifier;
}

}
