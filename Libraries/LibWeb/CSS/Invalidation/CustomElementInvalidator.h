/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_custom_element_state_change(DOM::Element&);
void invalidate_style_after_custom_state_set_change(DOM::Element&);

}
