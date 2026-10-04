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

}
