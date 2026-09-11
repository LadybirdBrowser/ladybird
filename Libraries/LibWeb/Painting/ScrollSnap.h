/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

using Compositor::MomentumFlingEstimator;
using Compositor::SnapAreaGeometry;
using Compositor::SnapAreaIdentity;
using Compositor::SnapAxes;
using Compositor::SnapContainerGeometry;
using Compositor::SnapDestination;
using Compositor::SnappedAreas;
using Compositor::SnapSelectionStrategy;

WEB_API SnapAxes snap_axes_of_scroll_container(Layout::Node const& snap_container);

WEB_API bool is_scroll_snap_container(Layout::Node const&);

// The geometry snap position selection runs over, collected from the layout of a snap container and of the snap areas
// it captures.
WEB_API Optional<SnapContainerGeometry> snap_container_geometry(Layout::Node const& snap_container);
WEB_API Vector<SnapAreaGeometry> collect_snap_areas(Layout::Node const& snap_container);

WEB_API SnapDestination adjust_scroll_destination_for_snapping(Layout::Node const& snap_container, CSSPixelPoint destination, SnapSelectionStrategy const& strategy = {});

struct ResnapSelection {
    SnappedAreas const& snapped_areas;
    GC::Ptr<DOM::Node const> focused_node;
    GC::Ptr<DOM::Element const> targeted_element;
};

WEB_API SnapDestination select_resnap_destination(Layout::Node const& snap_container, CSSPixelPoint current_offset, ResnapSelection const&);

}
