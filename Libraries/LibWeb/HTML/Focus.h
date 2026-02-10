/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

enum class FocusTrigger : u8 {
    Click,
    Key,
    Script,
    Other,
};

WEB_API void run_focusing_steps(DOM::Node* new_focus_target, DOM::Node* fallback_target = nullptr, FocusTrigger focus_trigger = FocusTrigger::Other);
WEB_API void run_unfocusing_steps(DOM::Node* old_focus_target);

}
