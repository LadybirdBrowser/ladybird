/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::DOM {

class Node;

}

namespace Web::CSS::Invalidation {

void invalidate_node_style(DOM::Node&, DOM::StyleInvalidationReason);
void invalidate_node_style_for_properties(
    DOM::Node&,
    DOM::StyleInvalidationReason,
    Vector<CSS::InvalidationSet::Property> const&,
    DOM::StyleInvalidationOptions);

}
