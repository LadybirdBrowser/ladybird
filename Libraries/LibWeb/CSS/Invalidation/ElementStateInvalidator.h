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

void invalidate_style_after_active_state_change(DOM::Element&);
void invalidate_style_after_modal_state_change(DOM::Element&);
void invalidate_style_after_open_state_change(DOM::Element&);
void invalidate_style_after_option_selected_state_change(DOM::Element&);
void invalidate_style_after_input_open_state_change(DOM::Element&);
void invalidate_style_after_select_open_state_change(DOM::Element&);
void invalidate_style_after_shadow_root_change(DOM::Element&);

}
