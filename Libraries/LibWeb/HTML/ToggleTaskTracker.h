/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#toggle-task-tracker
struct ToggleTaskTracker {
    // https://html.spec.whatwg.org/multipage/interaction.html#toggle-task-task
    // NOTE: We store the task's ID rather than the task itself to avoid ownership issues.
    Optional<HTML::TaskID> task_id;

    // https://html.spec.whatwg.org/multipage/interaction.html#toggle-task-old-state
    String old_state;
};

}
