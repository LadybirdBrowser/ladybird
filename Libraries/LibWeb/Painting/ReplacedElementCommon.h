/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

Gfx::IntRect get_replaced_box_painting_area(PaintableBox const& paintable, DisplayListRecordingContext const& context, CSS::ObjectFit object_fit, Gfx::IntSize content_size);

}
