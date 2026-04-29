/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Slottable.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_slottable_assignment_change(DOM::Slottable const&);
void invalidate_assigned_slottables_after_slot_style_change(DOM::Element&);

}
