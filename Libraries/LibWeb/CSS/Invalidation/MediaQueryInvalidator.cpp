/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Invalidation/MediaQueryInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

struct MediaQueryRuleInvalidation {
    StyleSheetInvalidationSet style_sheet_invalidation_set;
    Vector<FlyString> keyframes_animation_names;

    void add_rule(CSSRule const& rule)
    {
        extend_style_sheet_invalidation_set_with_rule(style_sheet_invalidation_set, rule);

        if (auto const* keyframes_rule = as_if<CSSKeyframesRule>(rule))
            keyframes_animation_names.append(keyframes_rule->name());
    }
};

static void invalidate_style_after_media_rule_changes(DOM::Node& root, MediaQueryRuleInvalidation const& invalidation)
{
    auto style_sheet_invalidation_set = invalidation.style_sheet_invalidation_set;
    add_shadow_root_stylesheet_effects_for_broad_invalidation(
        root,
        style_sheet_invalidation_set,
        style_sheet_invalidation_set.invalidation_set.needs_invalidate_whole_subtree());

    invalidate_root_for_style_sheet_change(root, style_sheet_invalidation_set, DOM::StyleInvalidationReason::MediaQueryChangedMatchState);

    for (auto const& animation_name : invalidation.keyframes_animation_names)
        invalidate_root_for_keyframes_rule(root, animation_name);
}

void evaluate_media_rules_and_invalidate_style(DOM::Document& document)
{
    bool document_media_queries_changed_match_state = false;
    MediaQueryRuleInvalidation document_invalidation;
    document.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
        if (style_sheet.evaluate_media_queries(document, [&](CSSRule const& changed_rule) {
                document_invalidation.add_rule(changed_rule);
            }))
            document_media_queries_changed_match_state = true;
    });

    document.for_each_shadow_root([&](auto& shadow_root) {
        bool shadow_root_media_queries_changed_match_state = false;
        MediaQueryRuleInvalidation shadow_root_invalidation;
        shadow_root.style_scope().for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet) {
            if (style_sheet.evaluate_media_queries(document, [&](CSSRule const& changed_rule) {
                    shadow_root_invalidation.add_rule(changed_rule);
                }))
                shadow_root_media_queries_changed_match_state = true;
        });

        if (!shadow_root_media_queries_changed_match_state)
            return;

        shadow_root.style_scope().invalidate_rule_cache();
        invalidate_style_after_media_rule_changes(shadow_root, shadow_root_invalidation);
    });

    if (document_media_queries_changed_match_state) {
        document.style_scope().invalidate_rule_cache();
        invalidate_style_after_media_rule_changes(document, document_invalidation);
    }
}

}
