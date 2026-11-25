/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibPlatformFeatures/SystemFacts.h>

// Only builds that decode video through Vulkan define these. The system FFmpeg is only described once the bundled one has
// been, as loading it relies on the bundled one.
Optional<PlatformFeatures::FFmpegFacts> describe_bundled_ffmpeg();
Optional<PlatformFeatures::FFmpegFacts> describe_system_ffmpeg();
