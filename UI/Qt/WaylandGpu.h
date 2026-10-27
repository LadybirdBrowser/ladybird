/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>

#include <sys/types.h>

namespace Ladybird {

// The DRM device the Wayland compositor presents on, as it names it in its default dma-buf feedback. It is unknown off
// Wayland, and when the compositor does not send dma-buf feedback.
Optional<dev_t> wayland_compositor_main_device();

}
