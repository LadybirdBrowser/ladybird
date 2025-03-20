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
    GC::Ptr<DOM::Element const> subject {};
    bool collect_per_element_selector_involvement_metadata { false };
    bool did_match_any_hover_rules { false };
};

bool matches(CSS::Selector const&, DOM::Element const&, GC::Ptr<DOM::Element const> shadow_host, MatchContext& context, Optional<CSS::PseudoElement> = {}, GC::Ptr<DOM::ParentNode const> scope = {}, SelectorKind selector_kind = SelectorKind::Normal, GC::Ptr<DOM::Element const> anchor = nullptr);

}
