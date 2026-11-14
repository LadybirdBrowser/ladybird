/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, WebContentClientId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, PresentationId);

using ContextTimer = RustFFI::ContextTimer;

// A label of an overlay, shaped in a font that the overlays' resources of its context hold: the glyphs, where each goes
// along the line in device pixels, and the metrics of the font that placing the line needs.
struct OverlayLabel {
    u64 font_id { 0 };
    Vector<u32> glyph_ids;
    Vector<float> glyph_x_positions;
    float width { 0 };
    // At the size the label is shaped at: the ascent above the baseline, which is negative, and the descent.
    float ascent { 0 };
    float descent { 0 };
    // The ascent that the painter places glyphs below, at the font's own size.
    float font_ascent { 0 };
};

// What the scroll and input logic of the contexts asks of the process around it. None of these calls comes back into
// that logic before it returns.
class CompositorCoreHost {
public:
    virtual ~CompositorCoreHost() = default;

    // A message for a client that is gone is dropped.
    virtual void post_message_to_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // A client without a render clock channel hears nothing.
    virtual void post_message_to_render_clock_of_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // A message for a presentation that is gone is dropped.
    virtual void post_message_to_presentation(PresentationId, IPC::MessageBuffer&) = 0;
    // Each timer of a context fires once after the delay, and setting it again replaces the delay.
    virtual void set_timer(Web::CompositorContextId, ContextTimer, AK::Duration delay) = 0;
    virtual void cancel_timer(Web::CompositorContextId, ContextTimer) = 0;
    // In the font family given, or the default one. Empty where no font draws the label.
    virtual Optional<OverlayLabel> shape_overlay_label(Web::CompositorContextId, Optional<StringView> font_family, double device_pixel_ratio, StringView text) = 0;
};

}
