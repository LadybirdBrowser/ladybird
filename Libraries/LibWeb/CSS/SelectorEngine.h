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

bool matches(CSS::Selector const&, Optional<CSS::CSSStyleSheet const&> style_sheet_for_rule, DOM::Element const&, GC::Ptr<DOM::Element const> shadow_host, Optional<CSS::Selector::PseudoElement::Type> = {}, GC::Ptr<DOM::ParentNode const> scope = {}, SelectorKind selector_kind = SelectorKind::Normal, GC::Ptr<DOM::Element const> anchor = nullptr);

[[nodiscard]] bool fast_matches(CSS::Selector const&, Optional<CSS::CSSStyleSheet const&> style_sheet_for_rule, DOM::Element const&, GC::Ptr<DOM::Element const> shadow_host);
[[nodiscard]] bool can_use_fast_matches(CSS::Selector const&);

[[nodiscard]] bool matches_hover_pseudo_class(DOM::Element const&);

}
