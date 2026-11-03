/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass, GC::Ptr<DOM::Node> old_state, GC::Ptr<DOM::Node> new_state);
void invalidate_style_after_subtree_place_changed(DOM::Node& subtree, GC::Ptr<DOM::Node> old_parent);
bool descendants_hold_a_propagating_state_source(DOM::Node&);
bool holds_a_propagating_state_source(DOM::Node&);
void invalidate_style_after_slot_assignment_changed(DOM::Element& slot);
void invalidate_style_after_subtree_holding_a_state_source_left(DOM::Node& subtree, GC::Ptr<DOM::Node> old_parent);
void invalidate_style_after_focused_element_keyboard_input_change(DOM::Document&);

}
