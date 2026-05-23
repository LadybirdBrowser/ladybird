/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PseudoClassBitmap.h>

namespace Web::CSS {

struct SelectorInsights {
    bool has_has_selectors { false };
    bool has_has_selectors_with_relative_selector_that_has_sibling_combinator { false };
    bool has_local_link_selectors { false };
    PseudoClassBitmap pseudo_classes;
};

}
