/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass, GC::Ptr<DOM::Node> old_state, GC::Ptr<DOM::Node> new_state);

}
