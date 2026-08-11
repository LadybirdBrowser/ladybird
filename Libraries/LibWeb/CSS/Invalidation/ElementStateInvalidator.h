/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;

}

namespace Web::HTML {

class HTMLMediaElement;
class HTMLMeterElement;

}

namespace Web::CSS::Invalidation {

// Each of these reports one element-local pseudo-class whose truth just changed, with the value it
// changed to. The value is what StyleEngine needs: a state fact is a delta, and an engine told only
// that "something changed" cannot tell a transition from a repetition. What the change invalidates
// is StyleEngine's to decide, so publishing the fact is the whole of the work.
void invalidate_style_after_active_state_change(DOM::Element&, bool is_active);
void invalidate_style_after_modal_state_change(DOM::Element&, bool is_modal);
void invalidate_style_after_open_state_change(DOM::Element&, bool is_open);
void invalidate_style_after_option_selected_state_change(DOM::Element&, bool is_selected);
void invalidate_style_after_input_open_state_change(DOM::Element&, bool is_open);
void invalidate_style_after_select_open_state_change(DOM::Element&, bool is_open);
void invalidate_style_after_indeterminate_state_change(DOM::Element&, bool is_indeterminate);
void invalidate_style_after_default_state_change(DOM::Element&, bool was_default);

// Read-write state can be inherited through an editing host, so callers snapshot the old answer
// before changing editability and publish only elements whose answer moved.
void invalidate_style_after_read_write_state_change(DOM::Element&, bool was_read_write);

void invalidate_style_after_media_muted_state_change(HTML::HTMLMediaElement&, bool is_muted);
void invalidate_style_after_media_paused_state_change(HTML::HTMLMediaElement&, bool is_paused);
void invalidate_style_after_media_seeking_state_change(HTML::HTMLMediaElement&, bool is_seeking);
void invalidate_style_after_media_ready_state_change(HTML::HTMLMediaElement&, bool was_buffering);

// A meter's five value pseudo-classes are worked out from its attributes rather than written, and
// they are worked out after the element has already published the facts it arrived with, so the
// element publishes them again whenever it recomputes them.
void invalidate_style_after_meter_value_state_change(HTML::HTMLMeterElement&);

}
