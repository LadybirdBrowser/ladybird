/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCompositing/DisplayList/ContextRef.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Color.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Compositing {

namespace RustFFI {

struct FfiRasterPainter;

}

// Builds a visual context tree node by node in Rust for unit tests, which otherwise only ever
// receive trees the layout arena built.
class COMPOSITING_API VisualContextTreeTestBuilder {
    AK_MAKE_NONCOPYABLE(VisualContextTreeTestBuilder);
    AK_MAKE_NONMOVABLE(VisualContextTreeTestBuilder);

public:
    VisualContextTreeTestBuilder();
    ~VisualContextTreeTestBuilder();

    EffectNodeIndex append_image_filter(u64 image_id, Gfx::IntRect);
    // Ends adding nodes. The lists below are recorded against the tree as it is then.
    void finish();

    // Records a list against the tree, which the builder replays from then on.
    void record_filled_rect(ContextRef, Gfx::IntRect, Gfx::Color);
    void record_rounded_rect_ring(ContextRef, FillRoundedRectRing const&);

    // Replays the list recorded last, with nothing scrolled, as it shows at `time_nanoseconds`.
    void replay(RustFFI::FfiRasterPainter const&, i64 time_nanoseconds) const;

private:
    void* m_builder { nullptr };
};

}
