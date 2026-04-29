/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;

}

namespace Web::CSS::Invalidation {

void invalidate_style_after_embedded_content_geometry_change(DOM::Element&);
void invalidate_style_after_object_representation_change(DOM::Element&);

}
