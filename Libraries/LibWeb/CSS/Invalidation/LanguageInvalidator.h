/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class CharacterData;
class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_language_change(DOM::Element&);
void invalidate_style_after_directionality_change(DOM::Element&);
void invalidate_style_after_text_directionality_change(DOM::CharacterData&);

}
