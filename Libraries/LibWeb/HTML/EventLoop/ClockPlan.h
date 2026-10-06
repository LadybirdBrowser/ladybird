/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::HTML {

// Seals the plan of the clock lease of the tasks after a rendering update of `document`, in place of the last one:
// which elements the render clock may sample the running animations of, and until when, or none unless `may_plan`.
// Answers whether there is one.
bool seal_clock_plan(DOM::Document&, bool may_plan);

// Whether `document` has a running animation that a clock plan can advance and the compositor does not run, so a
// rendering update may make a clock plan for it.
bool runs_animations_for_clock_plan(DOM::Document const&);

}
