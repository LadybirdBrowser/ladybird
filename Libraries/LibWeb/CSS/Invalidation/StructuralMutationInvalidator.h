/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::DOM {

class Node;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_same_parent_move(DOM::Node&, DOM::StyleInvalidationReason);
void invalidate_structurally_affected_siblings(DOM::Node&, DOM::StyleInvalidationReason);
void mark_ancestors_as_having_child_needing_style_update(DOM::Node&);

}
