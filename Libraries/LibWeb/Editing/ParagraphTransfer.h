/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

struct ParagraphTransferResult {
    GC::Root<DOM::Node> first_node;
    GC::Root<DOM::Node> last_node;
    DOM::BoundaryPoint start;
    DOM::BoundaryPoint end;
    Optional<DOM::BoundaryPoint> destination_text_seam;
};

enum class ParagraphTransferStyle {
    MatchDestination,
    PreserveMarkup,
    PreserveVisualAppearance,
};

enum class ParagraphTransferSeam {
    Preserve,
    CoalesceDestinationText,
};

struct ParagraphTransferOptions {
    // INTEROP: A paragraph merge can preserve the visual style of either the paragraph being moved or the paragraph
    //          receiving it. Blink and WebKit get this distinction from the direction of their serialize-and-replace
    //          operation; direct transfer must carry it explicitly so style preparation remains part of the move.
    ParagraphTransferStyle source_style { ParagraphTransferStyle::PreserveMarkup };
    ParagraphTransferStyle destination_style { ParagraphTransferStyle::PreserveMarkup };
    ParagraphTransferSeam seam { ParagraphTransferSeam::Preserve };
};

// Move one rendered paragraph's inline contents to another caret position. Blink and WebKit implement this as a
// composite serialize-and-replace command. Ladybird preserves node identity instead because its editing history is a
// flat sequence of reversible mutations, while applying the same visual-style normalization before moving the nodes.
// The returned range is the semantic selection produced by the move: it starts at the destination seam rather than at
// the first moved leaf, which may be preceded by content in the same text node after completion.
ParagraphTransferResult transfer_paragraph_contents(DOM::Node& source_paragraph, DOM::BoundaryPoint destination, ParagraphTransferOptions = {});

}
