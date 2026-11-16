/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Painting {

// Whether the hit-test items of the frame a recording made still stand for the document's boxes: not where the
// recording landed after the host wrote the document's rows.
enum class HitTestItemsStand {
    No,
    Yes,
};

// What a document did with the hit-test items of the frame a recording made as it took the recording in.
enum class HitTestItemsAdoption : u8 {
    Dropped,
    Kept,
    TookNew,
};

// A recording of a document's display list, started from the document as it was then: done in step with the host, or
// in flight beside the event loop until the event loop takes it in. Rust keeps what it records.
struct DisplayListRecording {
    // The structural epoch of the visual context tree the recording records against, which stays with the render state.
    u64 visual_context_tree_structural_epoch { 0 };
    PaintCommandCacheMode cache_mode;
    bool in_flight { false };
};

}
