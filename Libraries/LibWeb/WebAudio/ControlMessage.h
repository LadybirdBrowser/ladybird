/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>

namespace Web::WebAudio {

using SourceId = u64;

struct StartSource {
    SourceId id { 0 }; // FIXME: stable per-source id for render-thread routing
    double when { 0.0 };
};

struct StopSource {
    SourceId id { 0 }; // FIXME: stable per-source id for render-thread routing
    double when { 0.0 };
};

// FIXME: add more event types

// https://webaudio.github.io/web-audio-api/#control-message
using ControlMessage = Variant<StartSource, StopSource>;

}
