/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>

namespace Compositing {

// What a display list of a document's viewport is stamped with for async scrolling.
struct AsyncScrollingMetadata {
    // The document whose viewport this is, which the compositor names when it pans the visual viewport of a page that
    // has no scroll node.
    Optional<Web::UniqueNodeID> document_id;
    Gfx::IntRect viewport_rect;
    u64 wheel_event_listener_state_generation { 0 };
    bool has_blocking_wheel_event_listeners { false };
    bool has_blocking_wheel_event_region_covering_viewport { false };
    // Whether the viewport keeps the scrolls along an axis from the document it is nested in. A viewport with nothing to
    // scroll has no scroll node to carry its overscroll-behavior.
    bool viewport_keeps_horizontal_scrolls { false };
    bool viewport_keeps_vertical_scrolls { false };
    // Converts the compositor's device pixel offsets to the CSS pixels the snap geometry is in.
    double device_pixels_per_css_pixel { 1.0 };
    Compositing::KeyboardScrollState keyboard_scroll_state {};
};

// What travels alongside the tape of a display list for whatever plays the list.
struct DisplayListProperties {
    u64 id { 0 };
    u64 compatible_visual_context_tree_structural_epoch { 0 };
    Optional<Gfx::Color> surface_clear_color;
    Gfx::Color root_group_color { Gfx::Color::Transparent };
};

}

#include <LibCompositing/DisplayList/DisplayListPropertiesCodecs.h>
