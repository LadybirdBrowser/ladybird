/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS {

void invalidate_rule_cache_after_style_sheet_change(DOM::Node& document_or_shadow_root, StyleSheetState const& sheet)
{
    if (sheet.native_rules().size() == 0)
        return;

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        invalidate_rule_cache_after_style_sheet_change(shadow_root->style_scope(), sheet);
    else
        invalidate_rule_cache_after_style_sheet_change(document_or_shadow_root.document().style_scope(), sheet);
}

void invalidate_rule_cache_after_style_sheet_change(StyleScope& style_scope, StyleSheetState const& sheet)
{
    style_scope.invalidate_style_cache();
    if (sheet.has_counter_style_rules() || style_scope.sheets_define_counter_styles())
        style_scope.invalidate_counter_style_cache();
}

void invalidate_rule_cache_for_style_sheet_owners(StyleSheetState const& style_sheet)
{
    style_sheet.for_each_owning_style_scope([&](StyleScope& style_scope) {
        invalidate_rule_cache_after_style_sheet_change(style_scope, style_sheet);
    });
}

}
