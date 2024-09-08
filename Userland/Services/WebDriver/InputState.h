/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Queue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <WebDriver/InputSource.h>
#include <WebDriver/Session.h>

namespace Web::WebDriver {

// FIXME: Action object is an object, constructed with fields
// property id, type, and subtype
// this type might not express that
using ActionObject = JsonObject;

// https://w3c.github.io/webdriver/#input-state
class InputState {
public:
    // To create an input state:
    // FIXME: 1. Let input state be an input state with the input state map set to an empty map, and the input cancel list set to an empty list.
    // FIXME: 2. Return input state
    InputState() = default;

    HashMap<String, NonnullOwnPtr<InputSource>> const& get_input_state_map() const { return m_input_state_map; }

private:
    // A map where keys are input ids, and the values are input sources.
    // Input ids are UUIDs, which are strings
    HashMap<String, NonnullOwnPtr<InputSource>> m_input_state_map;

    // An input cancel list, which is a list of action objects. This list is used to manage
    // dispatching events when resetting the state of the input source
    Vector<ActionObject> m_input_cancel_list;

    // An actions queue which is a queue that ensures that access to the input state is serialized.
    // FIXME: template type - who accesses the ActionsQueue? I.e. how to reference the actions
    // to cancel efficiently?
    Queue<int> m_actions_queue;
};

}
