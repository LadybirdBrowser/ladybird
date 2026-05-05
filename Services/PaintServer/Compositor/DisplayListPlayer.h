/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibPaintServer/Compositor/DisplayListPayload.h>
#include <PaintServer/Compositor/DrawCommandPlayer.h>

class SkCanvas;

namespace PaintServer {

struct DrawContext;

ErrorOr<DisplayListPayloadFooter> validate_display_list_payload(ReadonlyBytes payload);
ErrorOr<void> paint_display_list_payload(DrawContext const&, ReadonlyBytes payload, DisplayListPayloadFooter const&, SkCanvas& canvas);

}
