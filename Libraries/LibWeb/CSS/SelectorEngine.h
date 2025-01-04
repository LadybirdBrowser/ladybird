/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>

namespace Web::SelectorEngine {

enum class SelectorKind {
    Normal,
    Relative,
};

struct MatchContext {
    GC::Ptr<CSS::CSSStyleSheet const> style_sheet_for_rule {};
    bool did_match_any_hover_rules { false };
};

bool matches(CSS::Selector const&, DOM::Element const&, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, Optional<CSS::Selector::PseudoElement::Type> = {}, GC::Ptr<DOM::ParentNode const> scope = {}, SelectorKind selector_kind = SelectorKind::Normal, GC::Ptr<DOM::Element const> anchor = nullptr);

[[nodiscard]] bool fast_matches(CSS::Selector const&, DOM::Element const&, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context);
[[nodiscard]] bool can_use_fast_matches(CSS::Selector const&);

[[nodiscard]] bool matches_hover_pseudo_class(DOM::Element const&);

}
