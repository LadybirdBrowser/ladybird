/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/ContextRef.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/Export.h>
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Compositing {

// Builds a visual context tree node by node in Rust for unit tests, which otherwise only ever
// receive trees the layout arena built.
class COMPOSITING_API VisualContextTreeTestBuilder {
    AK_MAKE_NONCOPYABLE(VisualContextTreeTestBuilder);
    AK_MAKE_NONMOVABLE(VisualContextTreeTestBuilder);

public:
    VisualContextTreeTestBuilder();
    ~VisualContextTreeTestBuilder();

    EffectNodeIndex append_image_filter(u64 image_id, Gfx::IntRect);
    static NonnullRefPtr<DisplayList> filled_rect_display_list(AccumulatedVisualContextTree const&, ContextRef, Gfx::IntRect, Gfx::Color);
    static NonnullRefPtr<DisplayList> rounded_rect_ring_display_list(AccumulatedVisualContextTree const&, ContextRef, FillRoundedRectRing const&);

    AccumulatedVisualContextTree finish();

private:
    void* m_builder { nullptr };
};

}
