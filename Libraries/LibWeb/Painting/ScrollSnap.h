/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCompositing/Scrolling/ScrollSnapSelection.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Painting {

using Compositing::MomentumFlingEstimator;
using Compositing::SnapAreaGeometry;
using Compositing::SnapAreaIdentity;
using Compositing::SnapAxes;
using Compositing::SnapContainerGeometry;
using Compositing::SnapDestination;
using Compositing::SnappedAreas;
using Compositing::SnapSelectionStrategy;

WEB_API bool is_scroll_snap_container(Layout::Node const&);

// Registers a scroll container a layout tree build gave a style as a scroll snap container, or forgets its snapped areas
// if it does not snap.
WEB_API void take_built_scroll_container(Layout::BegunRead const&, DOM::Document&, Compositing::RustFFI::NodeSlotId, bool is_scroll_snap_container);

// The geometry snap position selection runs over, collected from the layout of a snap container and of the snap areas
// it captures.
WEB_API Optional<Compositing::SnapContainerGeometry> snap_container_geometry(Layout::Node const& snap_container);
WEB_API Vector<Compositing::SnapAreaGeometry> collect_snap_areas(Layout::Node const& snap_container);

WEB_API Compositing::SnapDestination adjust_scroll_destination_for_snapping(Layout::Node const& snap_container, CSSPixelPoint destination, Compositing::SnapSelectionStrategy const& strategy = {});

struct ResnapSelection {
    Compositing::SnappedAreas const& snapped_areas;
    GC::Ptr<DOM::Node const> focused_node;
    GC::Ptr<DOM::Element const> targeted_element;
};

WEB_API Compositing::SnapDestination select_resnap_destination(Layout::Node const& snap_container, CSSPixelPoint current_offset, ResnapSelection const&);

}
