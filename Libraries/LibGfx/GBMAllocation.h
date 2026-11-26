/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <LibGfx/LinuxDmaBufHandle.h>
#include <LibGfx/Size.h>
#include <sys/types.h>

namespace Gfx {

// Allocates a single-plane dma-buf through the system's GBM on the render node, in one of the layouts. Unlike Vulkan,
// GBM can tell the driver that a display may scan the memory out.
ErrorOr<LinuxDmaBufHandle> allocate_scanout_capable_linux_dmabuf(dev_t render_node, IntSize, u32 drm_format, ReadonlySpan<u64> modifiers);

}
