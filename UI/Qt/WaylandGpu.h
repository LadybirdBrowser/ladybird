/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>

#include <sys/types.h>

namespace Ladybird {

struct WaylandCompositorDevice {
    dev_t device { 0 };
    // The modifiers of the opaque 32-bit RGB dma-bufs that the compositor can import on the device.
    Vector<u64> xrgb8888_modifiers;
};

// The DRM device the Wayland compositor presents on, as it names it in its default dma-buf feedback. It is unknown off
// Wayland, and when the compositor does not send dma-buf feedback.
Optional<WaylandCompositorDevice> wayland_compositor_main_device();

}
