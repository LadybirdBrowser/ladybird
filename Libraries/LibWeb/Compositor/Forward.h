/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

// Forward declarations of the types the compositor process shares with WebContent.

namespace Web {

class CSSPixels;

enum class WheelDeltaPrecision : u8;
enum class ScrollGesturePhase : u8;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(i64, UniqueNodeID, Comparison, Increment, CastToUnderlying);

}

namespace Web::Painting {

class AccumulatedVisualContextTree;
class Canvas2DCommandStream;
struct Canvas2DCommandStreamSegment;
class CanvasSurfaceRegistry;
class DisplayList;
struct DisplayListCommandRun;
struct DisplayListGlyph;
class DisplayListPlayerSkia;
class DisplayListResourceStorage;
struct DisplayListResourceSet;
enum class CompositorScrollNodeKind : u8;
class ScrollStateSnapshot;

}
