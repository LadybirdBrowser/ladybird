/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::DOM {

class Element;

}

namespace Web::CSS {

class InvalidationSet;

}

namespace Web::CSS::Invalidation {

bool compound_may_match_element(DOM::Element const&, CSS::Selector::CompoundSelector const&, Optional<CSS::PseudoClass> ignored_pseudo_class = {});
bool element_matches_any_invalidation_set_property(DOM::Element const&, InvalidationSet const&);

}
