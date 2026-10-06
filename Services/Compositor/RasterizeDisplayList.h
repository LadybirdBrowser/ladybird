/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Forward.h>

namespace Compositor {

// Plays a display list that WebContent recorded outside any compositor context, such as one frame of an SVG image,
// into a bitmap it shares with this process. The transaction must carry every resource the list refers to.
ErrorOr<void> rasterize_display_list(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Compositing::DisplayListResourceTransaction&&, Gfx::Bitmap& target);

}
