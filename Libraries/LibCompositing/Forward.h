/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWebCommon/Forward.h>

// Forward declarations of the types the compositor process shares with WebContent.

namespace Compositing {

enum class PausedDebuggerOverlayAction : u8;

class AccumulatedVisualContextTree;
class Canvas2DCommandStream;
struct Canvas2DCommandStreamSegment;
class CanvasSurfaceRegistry;
class DisplayList;
struct DisplayListGlyph;
class DisplayListResourceStorage;
struct DisplayListResourceSet;
enum class CompositorScrollNodeKind : u8;
class ScrollStateSnapshot;

}
