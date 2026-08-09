/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Invalidation/MediaQueryInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

// Re-evaluate every media query and report what moved.
//
// A condition coming true or stopping being true is a program input, and StyleEngine routes it to the
// rules it gates and the elements those decide for. Nothing here marks style: what it does is
// evaluate the queries, reload fonts whose availability the new match state changes, publish the
// conditions, and drop the scope's cached rule set.
void evaluate_media_rules_and_publish_conditions(DOM::Document& document)
{
    ++document.style_invalidation_counters().media_rule_evaluations;

    bool document_media_queries_changed_match_state = false;
    document.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
        if (style_sheet.evaluate_media_queries(document)) {
            document_media_queries_changed_match_state = true;
            style_sheet.reload_fonts_after_media_query_change();
            // The queries moved, so the rules they gate changed activation. That is what StyleEngine
            // routes; without it a viewport resize re-evaluates the queries and reaches nobody.
            style_sheet.record_conditions_for_owners();
            record_stylesheet_rule_conditions(style_sheet);
            style_sheet.for_each_owning_style_scope([](StyleScope& style_scope) {
                style_scope.invalidate_style_cache();
                style_scope.publish_cascade_layer_order();
            });
        }
    });

    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        document.style_scope().for_each_stylesheet(origin, [&](CSS::CSSStyleSheet& style_sheet) {
            auto changed = style_sheet.evaluate_media_queries(document);
            record_stylesheet_rule_conditions(style_sheet, document);

            for (auto const& entry : document.style_computer().non_author_style_sheets()) {
                if (entry.sheet.ptr() == &style_sheet) {
                    document.style_computer().style_engine().set_sheet_conditions_hold(
                        entry.sheet_id, !style_sheet.disabled() && style_sheet.media()->matches());
                    break;
                }
            }

            if (!changed)
                return;
            document_media_queries_changed_match_state = true;
            style_sheet.reload_fonts_after_media_query_change();
        });
    }

    document.for_each_shadow_root([&](auto& shadow_root) {
        bool shadow_root_media_queries_changed_match_state = false;
        shadow_root.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
            if (style_sheet.evaluate_media_queries(document)) {
                shadow_root_media_queries_changed_match_state = true;
                style_sheet.reload_fonts_after_media_query_change();
                style_sheet.record_conditions_for_owners();
                record_stylesheet_rule_conditions(style_sheet);
                style_sheet.for_each_owning_style_scope([](StyleScope& style_scope) {
                    style_scope.invalidate_style_cache();
                    style_scope.publish_cascade_layer_order();
                });
            }
        });

        if (!shadow_root_media_queries_changed_match_state)
            return;

        shadow_root.style_scope().invalidate_style_cache();
    });

    if (document_media_queries_changed_match_state) {
        document.style_scope().invalidate_style_cache();
    }
}

}
