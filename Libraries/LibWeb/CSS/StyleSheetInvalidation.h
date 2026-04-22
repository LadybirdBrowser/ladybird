/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// Targeted invalidation plan derived from a stylesheet (or a single inserted rule) for the add/remove and insertRule
// paths. Carries a primary invalidation set plus anchor-based rules for selectors whose rightmost compound is
// pseudo-element-only or trailing-universal.
struct StyleSheetInvalidationSet {
    InvalidationSet invalidation_set;
    bool may_match_shadow_host { false };
    bool may_match_light_dom_under_shadow_host { false };

    struct PseudoElementInvalidationRule {
        InvalidationSet anchor_set;
        RefPtr<Selector> anchor_selector;
        GC::Ptr<CSSStyleSheet const> style_sheet_for_rule;
    };
    struct TrailingUniversalInvalidationRule {
        InvalidationSet anchor_set;
        RefPtr<Selector> anchor_selector;
        Selector::Combinator combinator { Selector::Combinator::None };
        GC::Ptr<CSSStyleSheet const> style_sheet_for_rule;
    };
    Vector<PseudoElementInvalidationRule> pseudo_element_rules;
    Vector<TrailingUniversalInvalidationRule> trailing_universal_rules;
};

struct ShadowRootStylesheetEffects {
    bool may_match_shadow_host { false };
    bool may_match_light_dom_under_shadow_host { false };
    bool may_affect_assigned_nodes_via_slots { false };
};

// Extend `result` with the invalidation effects of `style_rule`'s selectors. Falls back to a whole-subtree
// invalidation flag inside `result` when a selector is not amenable to targeted invalidation.
void extend_style_sheet_invalidation_set_with_style_rule(StyleSheetInvalidationSet& result, CSSStyleRule const& style_rule);

// Shadow-root rules can escape the shadow tree either through ::slotted(...) or through :host with a combinator to
// another compound, such as :host > * or :host + .foo. Those selectors must fan out invalidation to the host side
// instead of treating the change as shadow-local.
bool selector_may_match_light_dom_under_shadow_host(Selector const&);
WEB_API bool selector_may_match_light_dom_under_shadow_host(StringView selector_text);

// Apply a built invalidation set to `root` (a Document or a ShadowRoot). When `force_broad_invalidation` is true,
// schedule a tree-wide restyle regardless of the targeted set; this is used when the sheet contains rule kinds (such
// as @property or @keyframes) whose effects are not captured by selector invalidation alone.
void invalidate_root_for_style_sheet_change(DOM::Node& root, StyleSheetInvalidationSet const&, DOM::StyleInvalidationReason, bool force_broad_invalidation = false);

// Summarize how any currently-active stylesheet in `shadow_root` can escape the shadow subtree. Used by mutation
// paths that need host-side fallout derived from the whole shadow scope rather than a single sheet.
ShadowRootStylesheetEffects determine_shadow_root_stylesheet_effects(DOM::ShadowRoot const&);

// Slotted light-DOM nodes inherit from their assigned <slot>, so any shadow invalidation that dirties slot elements
// must also dirty the flattened assignees outside the shadow subtree.
void invalidate_assigned_elements_for_dirty_slots(DOM::ShadowRoot&);

// Summarize how `style_sheet` can escape the shadow subtree across all shadow roots it is owned by. Used to snapshot
// the pre-mutation reach of a sheet whose own rules are about to change.
ShadowRootStylesheetEffects determine_shadow_root_stylesheet_effects(CSSStyleSheet const&);

// Apply a targeted invalidation to all documents and shadow roots that own `style_sheet` in response to inserting
// `style_rule` into it.
void invalidate_owners_for_inserted_style_rule(CSSStyleSheet const& style_sheet, CSSStyleRule const& style_rule, DOM::StyleInvalidationReason);

// Apply a targeted invalidation to all documents and shadow roots that own `style_sheet` in response to inserting
// `keyframes_rule` into it. Only elements already referencing the inserted animation-name are dirtied.
void invalidate_owners_for_inserted_keyframes_rule(CSSStyleSheet const& style_sheet, CSSKeyframesRule const& keyframes_rule);

}
