/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/InputState.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-browsing-context-input-state-map
static HashMap<JS::RawGCPtr<HTML::BrowsingContext>, InputState> s_browsing_context_input_state_map;

InputState::InputState() = default;
InputState::~InputState() = default;

// https://w3c.github.io/webdriver/#dfn-get-the-input-state
InputState& get_input_state(HTML::BrowsingContext& browsing_context)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(browsing_context.is_top_level());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map does not contain browsing context, set input state map[browsing context] to create an input state.
    auto& input_state = s_browsing_context_input_state_map.ensure(browsing_context);

    // 4. Return input state map[browsing context].
    return input_state;
}

// https://w3c.github.io/webdriver/#dfn-reset-the-input-state
void reset_input_state(HTML::BrowsingContext& browsing_context)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(browsing_context.is_top_level());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map[browsing context] exists, then remove input state map[browsing context].
    s_browsing_context_input_state_map.remove(browsing_context);
}

}
