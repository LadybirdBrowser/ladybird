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
// Whether a rule that changed activation reaches what a scope's style cache holds: the keyframes,
// custom functions, container queries, counter styles and property registrations it indexes. A
// flip among style rules alone leaves the cache as it is; the rules' activation is the engine's.
static bool rule_reaches_style_cache(CSSRule const& rule)
{
    switch (rule.type()) {
    case CSSRule::Type::Style:
    case CSSRule::Type::Media:
    case CSSRule::Type::Supports:
    case CSSRule::Type::Scope:
    case CSSRule::Type::LayerBlock:
    case CSSRule::Type::LayerStatement:
    case CSSRule::Type::NestedDeclarations:
    case CSSRule::Type::Namespace:
    case CSSRule::Type::Page:
    case CSSRule::Type::Margin:
        return false;
    case CSSRule::Type::Import:
    case CSSRule::Type::FontFace:
    case CSSRule::Type::Keyframes:
    case CSSRule::Type::Keyframe:
    case CSSRule::Type::CounterStyle:
    case CSSRule::Type::FontFeatureValues:
    case CSSRule::Type::Property:
    case CSSRule::Type::Function:
    case CSSRule::Type::FunctionDeclarations:
    case CSSRule::Type::Container:
        return true;
    }
    return true;
}

void evaluate_media_rules_and_publish_conditions(DOM::Document& document)
{
    ++document.style_invalidation_counters().media_rule_evaluations;

    bool document_media_queries_changed_match_state = false;
    bool style_cache_inputs_changed = false;
    Function<void(CSSRule const&)> note_changed_rule = [&](CSSRule const& rule) {
        if (rule_reaches_style_cache(rule))
            style_cache_inputs_changed = true;
    };
    auto invalidate_style_cache_if_reached = [&](StyleScope& style_scope) {
        if (style_cache_inputs_changed)
            style_scope.invalidate_style_cache();
    };
    document.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
        if (style_sheet.evaluate_media_queries(document, note_changed_rule)) {
            document_media_queries_changed_match_state = true;
            style_sheet.reload_fonts_after_media_query_change();
            // The queries moved, so the rules they gate changed activation. That is what StyleEngine
            // routes; without it a viewport resize re-evaluates the queries and reaches nobody. The
            // evaluation recorded the rule conditions of every document holding the sheet already.
            style_sheet.record_conditions_for_owners();
            style_sheet.for_each_owning_style_scope([&](StyleScope& style_scope) {
                invalidate_style_cache_if_reached(style_scope);
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
            style_cache_inputs_changed = true;
            style_sheet.reload_fonts_after_media_query_change();
        });
    }

    document.for_each_shadow_root([&](auto& shadow_root) {
        bool shadow_root_media_queries_changed_match_state = false;
        shadow_root.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
            if (style_sheet.evaluate_media_queries(document, note_changed_rule)) {
                shadow_root_media_queries_changed_match_state = true;
                style_sheet.reload_fonts_after_media_query_change();
                style_sheet.record_conditions_for_owners();
                style_sheet.for_each_owning_style_scope([&](StyleScope& style_scope) {
                    invalidate_style_cache_if_reached(style_scope);
                    style_scope.publish_cascade_layer_order();
                });
            }
        });

        if (!shadow_root_media_queries_changed_match_state)
            return;

        invalidate_style_cache_if_reached(shadow_root.style_scope());
    });

    if (document_media_queries_changed_match_state)
        invalidate_style_cache_if_reached(document.style_scope());
}

}
