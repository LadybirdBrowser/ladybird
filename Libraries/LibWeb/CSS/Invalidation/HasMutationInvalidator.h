/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::DOM {

class Document;
class Element;
class Node;

}

namespace Web::CSS::Invalidation {

void invalidate_element_if_affected_by_has(DOM::Element&);
void invalidate_style_for_pending_has_mutations(DOM::Document&);
void schedule_has_invalidation_for_node(DOM::Node&, DOM::StyleInvalidationReason);
void schedule_has_invalidation_for_same_parent_move(DOM::Node&);

}
