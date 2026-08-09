/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS {

void invalidate_rule_cache_after_style_sheet_change(DOM::Node& document_or_shadow_root, CSSStyleSheet const& sheet)
{
    if (sheet.rules().length() == 0)
        return;

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        shadow_root->style_scope().invalidate_style_cache();
    else
        document_or_shadow_root.document().style_scope().invalidate_style_cache();
}

void invalidate_rule_cache_for_style_sheet_owners(CSSStyleSheet const& style_sheet)
{
    style_sheet.for_each_owning_style_scope([](StyleScope& style_scope) {
        style_scope.invalidate_style_cache();
    });
}

}
