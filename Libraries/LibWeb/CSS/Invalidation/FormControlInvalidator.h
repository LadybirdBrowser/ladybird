/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::DOM {

class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_checked_state_change(DOM::Element&, DOM::StyleInvalidationReason);

}
