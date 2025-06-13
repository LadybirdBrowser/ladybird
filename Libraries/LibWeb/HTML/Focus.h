/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

enum class FocusTrigger : u8 {
    Click,
    Key,
    Script,
    Other,
};

void run_focusing_steps(DOM::Node* new_focus_target, DOM::Node* fallback_target = nullptr, FocusTrigger focus_trigger = FocusTrigger::Other);
void run_unfocusing_steps(DOM::Node* old_focus_target);

}
