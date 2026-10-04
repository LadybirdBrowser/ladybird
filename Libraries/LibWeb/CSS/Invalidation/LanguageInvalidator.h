/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::DOM {

class CharacterData;
class Element;

}

namespace Web::HTML {

class HTMLSlotElement;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_language_change(DOM::Element&);
void invalidate_style_after_directionality_change(DOM::Element&);
void invalidate_style_after_slot_assignment_change(HTML::HTMLSlotElement&);
void invalidate_style_after_text_change_under(DOM::Element& parent_of_text);
// The text under the element, its generated content's included, lays out again where it is cased by its language.
void enroll_text_after_language_change(Layout::BegunRead const&, DOM::Element&);

}
