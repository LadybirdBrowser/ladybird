/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/DecodedImageFrame.h>

namespace Compositing::WebGL {

using ImageSourceFrame = Variant<Gfx::DecodedImageFrame, CapturedVideoFrame>;

}
