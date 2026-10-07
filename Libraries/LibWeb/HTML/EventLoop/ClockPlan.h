/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::HTML {

// Seals the plan of the clock lane of the frame a rendering update of `document` presented, in place of the last one:
// which elements the render clock may sample the running animations of, and until when, where `may_animate`, and what
// it hovers under the pointer, or none unless `may_plan`. Answers whether there is one.
bool seal_clock_plan(DOM::Document&, bool may_plan, bool may_animate);

// Whether the render clock may hover what is under the pointer while a task runs.
bool hover_lane_is_enabled();

}
