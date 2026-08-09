/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;
class Node;

}

namespace Web::CSS::Invalidation {

// Both publish the state the element now has and leave what it invalidates to StyleEngine.
void invalidate_style_after_checked_state_change(DOM::Element&);
void invalidate_style_after_placeholder_shown_change(DOM::Element&);
void invalidate_style_after_validity_change(DOM::Element&);

// A control leaving the tree takes its constraints with it, so the form and the fieldsets it was
// under answer differently for `:valid`/`:invalid` without anything about them having moved. The
// control is gone by the time this runs, so the containers are reached from where it was.
void invalidate_style_after_form_control_left(DOM::Node& old_ancestor);

}
