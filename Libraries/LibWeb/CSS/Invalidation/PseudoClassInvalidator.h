/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Selector.h>

namespace Web::DOM {

class Document;
class Element;
class Node;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass, DOM::Document&, GC::Ptr<DOM::Node>& state_slot, DOM::Node& invalidation_root, GC::Ptr<DOM::Node> new_state);
void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass, DOM::Document&, GC::Ptr<DOM::Element>& state_slot, DOM::Node& invalidation_root, GC::Ptr<DOM::Element> new_state);

}
