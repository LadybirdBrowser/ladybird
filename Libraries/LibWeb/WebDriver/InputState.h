/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/InputSource.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-input-state
struct InputState {
    InputState();
    ~InputState();

    // https://w3c.github.io/webdriver/#dfn-input-state-map
    HashMap<String, InputSource> input_state_map;

    // https://w3c.github.io/webdriver/#dfn-input-cancel-list
    Vector<ActionObject> input_cancel_list;

    // https://w3c.github.io/webdriver/#dfn-actions-queue
    Vector<String> actions_queue;
};

WEB_API InputState& get_input_state(HTML::Navigable& top_level_traversable);
WEB_API void reset_input_state(HTML::Navigable& top_level_traversable);

// Private command-boundary snapshots contain sources and cancellation records, never live action queue tokens.
WEB_API JsonObject serialize_input_state(InputState const&);
WEB_API ErrorOr<void, Error> restore_input_state(HTML::Navigable& top_level_traversable, JsonValue const&);

}
