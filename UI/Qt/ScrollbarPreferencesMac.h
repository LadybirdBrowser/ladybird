/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>

namespace Ladybird {

// Whether a press on a scrollbar track beside the thumb jumps to that spot rather than paging toward it, as the "Click
// in the scroll bar to" setting of the system says.
bool scrollbar_track_press_jumps();

// Calls the given function whenever that setting may have changed.
void on_scrollbar_track_press_behavior_change(Function<void()>);

}
