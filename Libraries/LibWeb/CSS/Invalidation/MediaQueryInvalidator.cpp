/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/MediaQueryInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetState.h>
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

    HashTable<StyleScope*> scopes_with_changed_layer_order;
    Function<void(StyleSheetState&)> evaluate_author_sheet = [&](StyleSheetState& style_sheet) {
        Parser::ValueParserFFI::NativeStyleSheetMediaEvaluation result {};
        if (!style_sheet.evaluate_media_queries(document, result))
            return;
        if (result.font_rule_inputs_changed)
            style_sheet.reload_fonts_after_media_query_change();
        // The evaluation recorded the changed rule conditions already; publish the sheet gate too.
        style_sheet.record_conditions_for_owners();
        style_sheet.for_each_owning_style_scope([&](StyleScope& style_scope) {
            if (result.style_cache_inputs_changed)
                style_scope.invalidate_style_cache();
            if (result.layer_order_inputs_changed)
                scopes_with_changed_layer_order.set(&style_scope);
        });
    };
    document.style_scope().for_each_active_css_style_sheet(evaluate_author_sheet);

    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        document.style_scope().for_each_stylesheet(origin, [&](CSS::StyleSheetState& style_sheet) {
            auto changed = style_sheet.evaluate_media_queries(document);
            record_stylesheet_rule_conditions(style_sheet, document);

            for (auto const& entry : document.style_computer().non_author_style_sheets()) {
                if (entry.sheet.ptr() == &style_sheet) {
                    document.style_computer().style_engine().set_sheet_conditions_hold(
                        entry.sheet_id, !style_sheet.disabled() && style_sheet.native_media_list().matches());
                    break;
                }
            }

            if (!changed)
                return;
            document.style_scope().invalidate_style_cache();
            style_sheet.reload_fonts_after_media_query_change();
        });
    }

    document.for_each_shadow_root([&](auto& shadow_root) {
        shadow_root.style_scope().for_each_active_css_style_sheet(evaluate_author_sheet);
    });

    for (auto* style_scope : scopes_with_changed_layer_order)
        style_scope->publish_cascade_layer_order();
}

}
