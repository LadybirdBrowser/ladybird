/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

Gfx::IntRect get_replaced_box_painting_area(Paintable const&, DisplayListRecordingContext const&, CSS::ObjectFit, CSSPixelSize content_size);

}
