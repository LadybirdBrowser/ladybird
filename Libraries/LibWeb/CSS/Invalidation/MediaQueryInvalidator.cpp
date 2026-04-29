/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Invalidation/MediaQueryInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

void evaluate_media_rules_and_invalidate_style(DOM::Document& document)
{
    bool document_media_queries_changed_match_state = false;
    document.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
        if (style_sheet.evaluate_media_queries(document))
            document_media_queries_changed_match_state = true;
    });

    document.for_each_shadow_root([&](auto& shadow_root) {
        bool shadow_root_media_queries_changed_match_state = false;
        shadow_root.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
            if (style_sheet.evaluate_media_queries(document))
                shadow_root_media_queries_changed_match_state = true;
        });

        if (!shadow_root_media_queries_changed_match_state)
            return;

        shadow_root.style_scope().invalidate_rule_cache();
        shadow_root.invalidate_style(DOM::StyleInvalidationReason::MediaQueryChangedMatchState);
        CSS::invalidate_assigned_elements_for_dirty_slots(shadow_root);

        if (auto* host = shadow_root.host())
            host->root().invalidate_style(DOM::StyleInvalidationReason::MediaQueryChangedMatchState);
    });

    if (document_media_queries_changed_match_state) {
        document.style_scope().invalidate_rule_cache();
        document.invalidate_style(DOM::StyleInvalidationReason::MediaQueryChangedMatchState);
    }
}

}
