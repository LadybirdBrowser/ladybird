/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Painting {

// Whether the hit-test list a recording made still names the document's boxes: not where the recording landed after
// the host wrote the document's rows.
enum class HitTestListStands {
    No,
    Yes,
};

// The read a recording's hit-test list is made in, `read`, where the list stands, and none otherwise.
inline Optional<Layout::BegunRead const&> hit_test_list_read(Layout::BegunRead const& read, HitTestListStands stands)
{
    if (stands == HitTestListStands::No)
        return {};
    return read;
}

// A recording of a document's display list, started from the document as it was then: done in step with the host, or
// in flight beside the event loop until the event loop takes it in. Its display list is made once it is published.
struct DisplayListRecording {
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    Optional<Gfx::Color> surface_clear_color;
    PaintCommandCacheMode cache_mode;
    bool in_flight { false };
    // What its display list is stamped with for async scrolling, sealed where the recording began, and the display list
    // it copied paint commands from then, which it publishes again where it recorded the same.
    Optional<Compositing::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata;
    RefPtr<Compositing::DisplayList> paint_command_cache_source;
};

}
