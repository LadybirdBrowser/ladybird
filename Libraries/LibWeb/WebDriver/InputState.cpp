/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/InputState.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-browsing-context-input-state-map
// NB: The map is keyed by the navigable of the top-level browsing context, which a process hosting a frame of the tab
//     holds as well.
static HashMap<GC::RawPtr<HTML::Navigable>, InputState>& browsing_context_input_state_map()
{
    static NeverDestroyed<HashMap<GC::RawPtr<HTML::Navigable>, InputState>> map;
    return *map;
}

InputState::InputState() = default;
InputState::~InputState() = default;

// https://w3c.github.io/webdriver/#dfn-get-the-input-state
InputState& get_input_state(HTML::Navigable& top_level_traversable)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(top_level_traversable.is_top_level_traversable());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map does not contain browsing context, set input state map[browsing context] to create an input state.
    auto& input_state = browsing_context_input_state_map().ensure(top_level_traversable);

    // 4. Return input state map[browsing context].
    return input_state;
}

// https://w3c.github.io/webdriver/#dfn-reset-the-input-state
void reset_input_state(HTML::Navigable& top_level_traversable)
{
    // 1. Assert: browsing context is a top-level browsing context.
    VERIFY(top_level_traversable.is_top_level_traversable());

    // 2. Let input state map be session's browsing context input state map.
    // 3. If input state map[browsing context] exists, then remove input state map[browsing context].
    browsing_context_input_state_map().remove(top_level_traversable);
}

}
