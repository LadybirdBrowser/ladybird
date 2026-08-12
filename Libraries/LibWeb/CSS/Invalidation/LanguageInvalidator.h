/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

}
