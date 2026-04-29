/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Invalidation/AdoptedStyleSheetInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

static CSS::StyleScope& style_scope_for_document_or_shadow_root(DOM::Node& document_or_shadow_root)
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        return shadow_root->style_scope();
    return document_or_shadow_root.document().style_scope();
}

static void invalidate_style_after_adopted_style_sheet_list_change(DOM::Node& document_or_shadow_root)
{
    style_scope_for_document_or_shadow_root(document_or_shadow_root).invalidate_rule_cache();
    document_or_shadow_root.invalidate_style(DOM::StyleInvalidationReason::AdoptedStyleSheetsList);
}

void invalidate_style_after_adopting_style_sheet(DOM::Node& document_or_shadow_root, CSSStyleSheet& style_sheet)
{
    style_sheet.add_owning_document_or_shadow_root(document_or_shadow_root);
    style_sheet.load_pending_image_resources(document_or_shadow_root.document());

    // Evaluate the sheet's media queries before the next style update so the cascade can see its rules. Otherwise the
    // rule cache may be rebuilt (e.g. via a :has() invalidation pass) before Document::evaluate_media_rules has a
    // chance to populate MediaList::m_matches.
    style_sheet.evaluate_media_queries(document_or_shadow_root.document());

    invalidate_style_after_adopted_style_sheet_list_change(document_or_shadow_root);
}

void invalidate_style_after_removing_adopted_style_sheet(DOM::Node& document_or_shadow_root, CSSStyleSheet& style_sheet)
{
    style_sheet.remove_owning_document_or_shadow_root(document_or_shadow_root);
    invalidate_style_after_adopted_style_sheet_list_change(document_or_shadow_root);
}

}
